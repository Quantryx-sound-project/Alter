/*
  ==============================================================================

    Synesthesia.h
    Fractal GPU visual. It no longer owns an OpenGL context: the whole window
    runs on ONE shared context (AlterGLHost), which draws this module's fractal
    into its rectangle using a single shared shader. This module just:
      * analyses the audio (pitch / octave / RMS) on its timer,
      * advances the animation clock,
      * registers itself with the host of whatever window it currently lives in,
      * exposes its per-frame state to the host via ISynShaderSource.

    The component paints nothing (it is transparent) so the host's fractal shows
    through, and 2D sibling modules composite on top on the GPU.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <functional>
#include "AudioSourceInterface.h"
#include "AlterTheme.h"
#include "SynShader.h"
#include "AlterGLHost.h"
#include "PitchUtils.h"
#include <cmath>

class VisualSynesthesia : public ThemedBackground,
                          public juce::Component,
                          private juce::Timer,
                          public  ISynShaderSource
{
public:
    explicit VisualSynesthesia (IAudioSource& receiver) : audioSource (receiver)
    {
        setOpaque (false);          // transparent: the GL host draws the fractal behind us
        startTimerHz (30);
    }

    /** The fractal is drawn by a shader inside AlterGLHost, and that shader now
        honours the flag: with uNoBg set it hands out the fractal alone, with
        coverage in the alpha channel. So the coverage Fusion receives is real. */
    bool transparencyReachesPixels() const override { return true; }

    ~VisualSynesthesia() override
    {
        stopTimer();
        if (currentHost != nullptr)
            currentHost->removeSource (this);
    }

    //  Control API (unchanged) 
    // Slider is "Tone smooth": HIGH = heavier smoothing (slow colour reaction),
    // LOW = fast reaction. The EMA coefficient is the opposite of that, so invert.
    void setSmoothAmount (float smooth01) { smoothAmount = juce::jlimit (0.01f, 0.99f, 1.0f - smooth01); }
    void setZoom (float z)        { zoom = juce::jlimit (0.5f, 2.0f, z); }
    void setRotation (float r)    { rotation = r; }
    void setSymmetry (int s)      { symmetry = juce::jlimit (1, 8, s); }
    void setSaturation (float s)  { saturation = juce::jlimit (0.0f, 2.0f, s); }
    void setBrightness (float b)  { brightness = juce::jlimit (0.0f, 2.0f, b); }   // 1 = neutral light output
    void setBloom (float b)       { bloom = juce::jlimit (-1.0f, 1.0f, b); }   // bipolar: − dims, + boosts
    void setSyncToBPM (bool sync) { syncToBPM = sync; }
    void setShaderSpeed (float s) { shaderSpeed = juce::jlimit (-2.0f, 2.0f, s); } // 0 = still, negative = reverse
    /** BPM mode: the shader evolution advances with this tempo (instead of Speed) and
        a pulse of light fires each beat-division. */
    void setBpm (float b)         { bpmValue = juce::jlimit (20.0f, 400.0f, b); }
    void setBeatDivision (int idx){ beatDivIdx = juce::jlimit (0, 11, idx); }

    // Beat-division table (matches Geometry): index → beats per pulse. No dotted values.
    static float beatsForDiv (int idx) noexcept
    {
        static const float b[12] = { 16.0f, 8.0f, 4.0f, 2.0f, 4.0f/3.0f, 1.0f,
                                     2.0f/3.0f, 0.5f, 1.0f/3.0f, 0.25f, 1.0f/6.0f, 0.125f };
        return b[juce::jlimit (0, 11, idx)];
    }

    /** Audio reactivity 0..1: 1 = fully follows the audio (fades in silence),
        0 = stays at a fixed baseline intensity (the fractal is always visible and
        does not respond to loudness). */
    void setReactivity (float r01) { reactivity = juce::jlimit (0.0f, 1.0f, r01); }

    /** Colour mode: true = manual base colour, false = pitch-derived (tone) hue. */
    void setColourMode (bool toneDependent) { manualColour = ! toneDependent; }
    /** Stores all three HSV axes, not just the hue — see SynShaderState::baseSat.
        Keeping only the hue is what made a white or black pick render as red. */
    void setBaseColour (juce::Colour c)
    {
        baseHue = c.getHue();
        baseSat = c.getSaturation();
        baseVal = c.getBrightness();
    }

    /** 'Mirror tone color': reverses the tone→hue wheel direction (see PitchUtils).
        Only has an effect in tone-colour mode; the manual base colour is untouched. */
    void setToneTwist (bool b)              { toneTwist = b; }

    /** 'Change' 0..1: smoothly morphs the fractal into a different-looking variation. */
    void setVariation (float v01) { variation = juce::jlimit (0.0f, 1.0f, v01); }

    /** 'Transmute' 0..1: a second, SYMMETRIC morph (radial falloff / glow / rings). */
    void setTransmute (float v01) { transmute = juce::jlimit (0.0f, 1.0f, v01); }

    /** Mirror fold: reflective symmetry (mirrors the field; not a spiral kaleidoscope). */
    void setMirror (bool on) { mirror = on; }

    /** Global parabolic sub-bin interpolation for pitch/octave estimation. */
    void setSubBinInterp (bool on) { subBinInterp = on; }

    /** Curve smoothing 0..1 ('ghost'): the displayed motion (evolution, structure,
        jitter, framing) lags toward its target, so when the picture changes fast the
        curves drift to their new place instead of snapping. 0 = instant. This is a
        separate pass from 'Tone smooth' (which smooths the pitch/tone response). */
    void setCurveSmooth (float v01) { curveSmoothAmt = juce::jlimit (0.0f, 1.0f, v01); }

    /** 'Clear' 0..1: final cleanup layer — keeps the main curves and merges
        overlapping small curves together (like bubbles fusing). */
    /** 'Tunnel' 0..1: morphs the shader into a symmetric fly-through tunnel —
        the deformation runs radially THROUGH the centre (never one-sided).
        0 = the shader exactly as it is today. */
    void setTunnel (float v01) { tunnelAmt = juce::jlimit (0.0f, 1.0f, v01); }

    /** 'Vortex' 0..1: swirls the tunnel walls into a spiral (only visible with Tunnel). */
    void setVortex (float v) { vortexAmt = juce::jlimit (-1.0f, 1.0f, v); }

    void setClear (float v01) { clearAmt = juce::jlimit (0.0f, 1.0f, v01); }

    /** 'Denoise' 0..1: the thin dashed secondary curves at higher octaves are fused
        into continuous strokes and kept dimmer, so they read as secondary layers
        instead of sparkly noise. 0 = off (raw dashed curves). */
    void setDenoise (float v01) { denoiseAmt = juce::jlimit (0.0f, 1.0f, v01); }

    //  ISynShaderSource
    SynShaderState getSynState() const override
    {
        const juce::ScopedLock sl (stateLock);
        return snapshot;
    }

    // Detached windows have no GL context: the HUD host renders us into an FBO
    // and hands back the image here (GL thread). We blit it in paint().
    void deliverOffscreenImage (const juce::Image& img) override
    {
        {
            const juce::ScopedLock sl (imgLock);
            offscreenImg = img;
            ++offscreenGen;
        }
        juce::Component::SafePointer<VisualSynesthesia> sp (this);
        juce::MessageManager::callAsync ([sp]() mutable { if (sp != nullptr) sp->repaint(); });
    }

    /** ANY THREAD. Runs `fn` on the last frame the GL host rendered for us, while
        holding the image lock so it cannot be replaced mid-read.

        For Fusion, which uploads a layer's frame into a texture from the GL
        thread. `generation` counts delivered frames, so a caller that caches the
        result gets false when nothing new has arrived. Keep `fn` short. */
    bool readOffscreenFrame (juce::uint32& generation,
                             const std::function<void (const juce::Image&)>& fn)
    {
        const juce::ScopedLock sl (imgLock);

        if (! offscreenImg.isValid() || offscreenGen == generation)
            return false;

        generation = offscreenGen;
        fn (offscreenImg);
        return true;
    }

    /** Render through the FBO even when this window HAS a GL context. Used by
        Fusion, which cannot composite a fractal that the host drew directly into
        the window — it needs the frame as an image. */
    void setForceOffscreen (bool shouldForce)
    {
        if (forceOffscreen == shouldForce) return;
        forceOffscreen = shouldForce;
        updateHostRegistration();
    }

private:
    //  Component
    void paint (juce::Graphics& g) override
    {
        if (isOffscreen)
        {
            // Detached: draw the image the HUD host rendered for us.
            const juce::ScopedLock sl (imgLock);
            if (offscreenImg.isValid())
                g.drawImageAt (offscreenImg, 0, 0);
        }
        // Inline: nothing — the host draws the fractal straight into the window.
    }

    void parentHierarchyChanged() override { updateHostRegistration(); }

    void updateHostRegistration()
    {
        // Host of THIS window: found for the HUD, null for a detached window.
        auto* localHost = AlterGLHost::forComponent (this);
        // Detached -> render offscreen via the primary (HUD) host instead.
        AlterGLHost* target = (localHost != nullptr) ? localHost : AlterGLHost::getPrimary();

        // forceOffscreen: a Fusion layer needs PIXELS it can composite, not a
        // fractal painted straight into the window behind everything. It uses the
        // host it already has, just through the FBO path.
        isOffscreen = forceOffscreen ? (target != nullptr)
                                     : ((localHost == nullptr) && (target != nullptr));

        if (target != currentHost)
        {
            if (currentHost != nullptr) currentHost->removeSource (this);
            currentHost = target;
            if (currentHost != nullptr) currentHost->addSource (this);
        }
    }

    //  Timer: audio analysis + animation clock + state snapshot 
    void timerCallback() override
    {
        if (AlterTheme::hudFrozen.load()) return;   // HUD "Hold": freeze shader state
        updateAudioData();

        if (smoothRMS > 0.02f)
        {
            // Shader EVOLUTION always follows Speed. BPM sync only drives the light-wave
            // pulse — a sharp flash on each beat-division that then decays.
            animationTime += (1.0 / 30.0) * shaderSpeed;
            if (syncToBPM)
            {
                // IMPULSE-SYNCED grid: the Creator plugin sends a one-shot sync impulse
                // when the DAW transport (re)starts. Each new impulse re-anchors the
                // beat phase to that instant; the RATE/DIVISION stay manual. Between
                // impulses (and with no plugin) the clock free-runs at the manual BPM.
                IAudioSource::HostSyncInfo hs;
                const bool haveHost = audioSource.getHostSync (hs);
                const double bps = (double) bpmValue / 60.0;   // manual RATE always

                if (haveHost && hs.pulseId != lastSyncPulseId)   // DAW downbeat/start → re-anchor phase
                {
                    lastSyncPulseId = hs.pulseId;
                    beatClock = (hs.pulseAgeMs == 0xFFFFFFFFu ? 0.0
                                                              : (double) hs.pulseAgeMs * 0.001 * bps);
                }

                const bool run = haveHost ? hs.playing : true;   // host: follow transport; standalone: run on sound
                if (run)
                    beatClock += (1.0 / 30.0) * bps;

                double ph = beatClock / (double) beatsForDiv (beatDivIdx);
                ph -= std::floor (ph);                                  // 0..1 within the division
                beatPulse = std::pow (1.0f - (float) ph, 3.0f);        // 1 at the beat → decays to 0
            }
            else
                beatPulse = 0.0f;
        }

        // Build the snapshot the GL thread reads. Rectangle is expressed in the
        // host component's coordinate space (where the host sets the viewport).
        SynShaderState st;
        st.pitch      = smoothPitch;
        st.octave     = smoothOctave;
        st.rms        = smoothRMS;       // intensity follows audio (natural look)
        st.time       = (float) animationTime;

        // React = audio-driven jitter/shake (like Geometry); it does NOT change
        // brightness. 0 = still, max = strong shake on loud audio.
        const float shakeAmt = reactivity * smoothRMS * 0.12f;
        auto& rng = juce::Random::getSystemRandom();
        st.shakeX = (rng.nextFloat() * 2.0f - 1.0f) * shakeAmt;
        st.shakeY = (rng.nextFloat() * 2.0f - 1.0f) * shakeAmt;
        st.zoom       = zoom;
        st.rotation   = rotation;
        st.symmetry   = symmetry;
        st.saturation = saturation;
        st.brightness = brightness;
        st.bloom      = bloom;
        st.manual     = manualColour;
        st.baseHue    = baseHue;
        // Tone mode owns the whole colour, so the manual sat/val must not leak into
        // it — a user who once picked a dark grey would otherwise find every note
        // rendered dark after switching tone colour back on.
        st.baseSat    = manualColour ? baseSat : 1.0f;
        st.baseVal    = manualColour ? baseVal : 1.0f;
        st.variation  = variation;
        st.transmute  = transmute;
        st.mirror     = mirror;
        st.tunnel     = tunnelAmt;
        st.vortex     = vortexAmt;
        st.clear      = clearAmt;
        st.denoise    = denoiseAmt;
        st.beatPulse  = beatPulse;

        st.offscreen = isOffscreen;
        st.noBackground = isTransparentBackground();
        if (isOffscreen)
        {
            // FBO size = our own pixel size; the detached window blits the result.
            st.x = 0; st.y = 0;
            st.w = getWidth(); st.h = getHeight();
            st.active = isShowing() && getWidth() > 1 && getHeight() > 1;
        }
        else if (currentHost != nullptr)
        {
            if (auto* hc = currentHost->getAttachedComponent())
            {
                const auto r = hc->getLocalArea (this, getLocalBounds());
                st.x = r.getX();  st.y = r.getY();
                st.w = r.getWidth(); st.h = r.getHeight();
                st.active = isShowing() && r.getWidth() > 1 && r.getHeight() > 1;
            }
        }

        // Curve smoothing ('ghost'): the DISPLAYED motion lags toward its target, so a
        // fast picture change drifts into place instead of snapping. Separate from
        // 'Tone smooth' (pitch/hue) — this eases the geometry-driving + framing fields.
        // curveSmoothAmt == 0 -> ga == 1 -> displayed = target (no change, instant).
        const float ga = 1.0f - curveSmoothAmt * 0.92f;
        dispTime     += ((float) st.time - dispTime)     * ga;
        dispOctave   += (st.octave        - dispOctave)   * ga;
        dispRMS      += (st.rms           - dispRMS)      * ga;
        dispShakeX   += (st.shakeX        - dispShakeX)   * ga;
        dispShakeY   += (st.shakeY        - dispShakeY)   * ga;
        dispZoom     += (st.zoom          - dispZoom)     * ga;
        dispRotation += (st.rotation      - dispRotation) * ga;
        st.time     = dispTime;
        st.octave   = dispOctave;
        st.rms      = dispRMS;
        st.shakeX   = dispShakeX;
        st.shakeY   = dispShakeY;
        st.zoom     = dispZoom;
        st.rotation = dispRotation;

        {
            const juce::ScopedLock sl (stateLock);
            snapshot = st;
        }

        // Repaint so the host's 2D overlay layer (the top-right info text) re-renders
        // each frame. The fractal itself is drawn by the GL host, so without this the
        // overlay would stay frozen — and the "Hide info" toggle wouldn't hide it.
        repaint();
    }

    void updateAudioData()
    {
        currentPitch  = detectPitch();
        currentOctave = detectOctave();
        currentRMS    = getRMSLevel();

        // CIRCULAR pitch-class smoothing. The pitch class feeds the hue (iPitch/12),
        // and hue is a WHEEL — 11 (B) and 0 (C) are neighbours. A plain lerp does not
        // know that: on B→C it counted DOWN 11→10→9…→0, so the fractal swept backwards
        // through every colour on the wheel while Chladni and Geometry — which both
        // already lerp their hue circularly — took the one-step path. That long way
        // round was the most visible colour difference between the modules on ordinary
        // material. Wrapping the delta into ±6 semitones makes all three agree.
        {
            float d = currentPitch - smoothPitch;
            if (d >  6.0f) d -= 12.0f;
            if (d < -6.0f) d += 12.0f;
            smoothPitch = std::fmod (smoothPitch + d * smoothAmount + 12.0f, 12.0f);
        }
        smoothOctave = smoothOctave + (currentOctave - smoothOctave) * (smoothAmount * 0.5f);
        smoothRMS    = smoothRMS    + (currentRMS    - smoothRMS)    * smoothAmount;
    }

    // MIDI override: exact loudest held note → Hz. >0 = use it; 0 = no MIDI on the
    // track (fall back to FFT detection); <0 = MIDI present but nothing held now
    // (caller should hold the last value). Same premium/auto behaviour as ToneAnalyzer.
    float midiDominantHz()
    {
        if (! audioSource.getMidiNotes (midiScratch)) return 0.0f;
        if (midiScratch.empty())                      return -1.0f;
        const IAudioSource::MidiNote* top = &midiScratch.front();
        for (const auto& n : midiScratch)
            if (n.velocity > top->velocity) top = &n;
        return 440.0f * std::pow (2.0f, ((float) top->note - 69.0f) / 12.0f);
    }

    float detectPitch()
    {
        const float mh = midiDominantHz();
        if (mh > 0.0f)
        {
            // ×12 of the shared hue so the shader's iPitch/12.0 lands on the exact
            // same wheel position Chladni and Geometry use.
            return PitchUtils::hueFromHz (mh, toneTwist) * 12.0f;
        }
        if (mh < 0.0f) return smoothPitch;   // MIDI on, nothing held → hold

        std::vector<float> spectrum;
        const int bins = audioSource.getLastFft (spectrum);
        if (bins == 0) return smoothPitch;

        // Search ~65 Hz..2 kHz expressed in bins of THIS spectrum (size-independent).
        const double sr = audioSource.getSampleRate();   // real device rate → correct pitch
        const float bw = PitchUtils::binWidthHz (bins, sr);
        const int lo = (int) std::round (65.0f   / bw);
        const int hi = (int) std::round (2000.0f / bw);
        const float freq = PitchUtils::dominantFrequency (spectrum, lo, hi, 0.01f, subBinInterp, sr);
        if (freq <= 0.0f) return smoothPitch;

        return PitchUtils::hueFromHz (freq, toneTwist) * 12.0f;   // shared wheel
    }

    float detectOctave()
    {
        const float mh = midiDominantHz();
        if (mh > 0.0f)
            return juce::jlimit (0.0f, 7.0f, std::log2 (mh / 16.35f));
        if (mh < 0.0f) return smoothOctave;   // MIDI on, nothing held → hold

        std::vector<float> spectrum;
        const int bins = audioSource.getLastFft (spectrum);
        if (bins == 0) return smoothOctave;

        const double sr = audioSource.getSampleRate();
        const float bw = PitchUtils::binWidthHz (bins, sr);
        const int lo = (int) std::round (16.0f   / bw);
        const int hi = (int) std::round (4000.0f / bw);
        const float freq = PitchUtils::dominantFrequency (spectrum, lo, hi, 0.01f, subBinInterp, sr);
        if (freq <= 0.0f) return smoothOctave;

        const float octave = std::log2 (freq / 16.35f);
        return juce::jlimit (0.0f, 7.0f, octave);
    }

    float getRMSLevel()
    {
        float raw = audioSource.getLastRms();
        if (raw < 0.001f) return 0.0f;                 // noise gate ~-60 dBFS
        float dB = 20.0f * std::log10 (raw);
        return juce::jlimit (0.0f, 1.0f, (dB + 36.0f) / 36.0f);
    }

    //  Members
    IAudioSource& audioSource;
    AlterGLHost*  currentHost = nullptr;
    bool          isOffscreen = false;     // detached window -> rendered via FBO image
    bool          forceOffscreen = false;  // hosted inside Fusion -> always via FBO image

    mutable juce::CriticalSection stateLock;
    SynShaderState snapshot;

    mutable juce::CriticalSection imgLock;
    juce::Image   offscreenImg;            // last frame rendered for a detached window
    juce::uint32 offscreenGen = 0;   // ditto; see readOffscreenFrame

    std::vector<IAudioSource::MidiNote> midiScratch;   // reused per frame (MIDI override)
    float currentPitch = 0.0f, currentOctave = 2.0f, currentRMS = 0.0f;
    float smoothPitch = 0.0f, smoothOctave = 2.0f, smoothRMS = 0.0f;
    float smoothAmount = 0.15f;
    bool  subBinInterp = true;      // parabolic sub-bin pitch refinement (always on)
    double animationTime = 0.0;

    float zoom = 1.0f;
    float rotation = 0.0f;
    int   symmetry = 1;
    float saturation = 1.0f;
    float brightness = 1.0f;
    float bloom = 0.0f;
    float shaderSpeed = 1.0f;
    float reactivity = 0.0f;       // default: not audio-reactive (static visible fractal)
    bool  manualColour = false;    // false = tone (pitch) hue, true = manual base colour
    bool  toneTwist = false;       // 'Mirror tone color': reverse the hue wheel direction
    float baseHue = 0.6f;          // manual colour hue
    float baseSat = 1.0f;          // manual colour saturation (0 = white/grey/black)
    float baseVal = 1.0f;          // manual colour brightness (0 = black)
    float variation = 0.0f;        // 'Change' morph 0..1
    float transmute = 0.0f;        // 'Transmute' morph 0..1
    bool  mirror = false;          // Mirror fold (reflective symmetry)
    float tunnelAmt = 0.0f;        // 'Tunnel' symmetric tunnel morph 0..1
    float vortexAmt = 0.0f;        // 'Vortex' swirl 0..1
    float clearAmt = 0.0f;         // 'Clear' cleanup/merge 0..1
    float denoiseAmt = 0.0f;       // 'Denoise' fuse dashed secondary curves 0..1
    float curveSmoothAmt = 0.0f;   // 'Curve smooth' / ghost motion lag 0..1

    // Displayed (lagged) motion state for the 'ghost' curve smoothing.
    float dispTime = 0.0f, dispOctave = 2.0f, dispRMS = 0.0f;
    float dispShakeX = 0.0f, dispShakeY = 0.0f, dispZoom = 1.0f, dispRotation = 0.0f;
    bool   syncToBPM = false;      // BPM mode off by default → Speed drives evolution
    float  bpmValue  = 120.0f;
    int    beatDivIdx = 5;         // 1/4 note
    double beatClock = 0.0;
    juce::uint32 lastSyncPulseId = 0;   // last DAW sync impulse we re-anchored to
    float  beatPulse = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualSynesthesia)
};
