/*
  ==============================================================================

    ChladniPaterns.h
    Created: 21 Apr 2026 10:55:46am
    Author:  Martin Peroncik

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include "AudioSourceInterface.h"
#include "AlterTheme.h"
#include "PitchUtils.h"
#include "AsyncVisualBase.h"
#include <cmath>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  ChladniPatternMeter  –  particle sand simulation
//
//  Chladni function (free-edge, cos-cos antisymmetric form):
//      Z(x,y) = cos(n·π·x)·cos(m·π·y/r) − cos(m·π·x)·cos(n·π·y/r)
//  Source: Leissa "Vibration of Plates" (1969).
//
//  SAND: thousands of particles random-walk with step ∝ |Z| (plate amplitude)
//  plus a weak gradient pull, so they migrate to and settle on nodal lines –
//  exactly like real sand on a vibrating plate. RMS drives the vibration.
//
//  AUDIO REACTIVE mode (default ON) – physical:
//  exactly like a real plate, the DOMINANT FREQUENCY in the audio excites the
//  eigenmode whose resonance frequency f(m,n) = k·(m²/r² + n²) is nearest to
//  it. The material (k) and aspect ratio (r) therefore change WHICH pattern a
//  given sound produces – switch aluminium → acrylic and the same bass note
//  lands on a completely different mode, exactly as in the physical world.
//
//  Colour: fixed sand colour, or "by tone" (DEFAULT) – the dominant pitch class
//  maps to a hue via PitchUtils::pitchClassHue, the wheel shared with Synesthesia
//  and Geometry: C=red, C#=rose, D=magenta, D#=violet, E=blue, F=azure, F#=cyan,
//  G=spring, G#=green, A=chartreuse, A#=yellow, B=orange. The wheel runs DOWNWARD
//  from red on purpose – see the note in PitchUtils.h before changing it.
// ─────────────────────────────────────────────────────────────────────────────
class ChladniPatternMeter : public AsyncVisualBase
{
public:
    explicit ChladniPatternMeter (IAudioSource& src)
        : AsyncVisualBase ("AlterChladni", 30), audioSource (src)
    {
        setCrispUpscale (true);   // sand grains stay single, clean pixels when enlarged
        startAsyncRender();
    }

    // Stop the worker BEFORE our members are destroyed (it calls renderImage()).
    ~ChladniPatternMeter() override { stopAsyncRender(); }

    // ── Control API ──────────────────────────────────────────────────────────

    void setM (int v)
    {
        manualM = juce::jlimit (1, 12, v);
        if (! audioReactive && m != manualM) { m = manualM; onModeChanged(); }
    }

    void setN (int v)
    {
        manualN = juce::jlimit (1, 12, v);
        if (! audioReactive && n != manualN) { n = manualN; onModeChanged(); }
    }

    /** Audio-reactive m/n (default true). When disabled, manual m/n apply. */
    void setAudioReactive (bool b)
    {
        if (audioReactive == b) return;
        audioReactive = b;
        if (! b) { m = manualM; n = manualN; onModeChanged(); }
    }

    /** Reactive mode shift: adds +s to the matched (m,n) while keeping full
        audio reactivity – richer figures from the same audio (0-6). */
    void setModeShift (int s)
    {
        s = juce::jlimit (0, 6, s);
        if (modeShift == s) return;
        modeShift = s;
        if (audioReactive && matchedValid)
            applyMatchedMode (true);
    }

    /** Aspect ratio r = Lx/Ly  (0.25–4.0, default 1.0 = square plate). */
    void setAspectRatio (float r)
    {
        ar = juce::jlimit (0.25f, 4.0f, r);
        modeTableDirty = true;
    }

    /** Sand movement smoothness 0–1: 0 = jumpy/fast, 1 = slow fluid settling. */
    void setSandSmoothness (float s01)
    {
        baseStep = juce::jmap (juce::jlimit (0.0f, 1.0f, s01), 0.020f, 0.0035f);
    }

    /** Number of sand particles (1000–15000). */
    void setParticleCount (int count)
    {
        particleCount = juce::jlimit (1000, 15000, count);
    }

    void setSandColour (juce::Colour c)  { sandColour = c; }

    /** 0 = fixed sand colour, 1 = colour follows the dominant tone (12-hue wheel). */
    void setColourByTone (bool b)        { colourByTone = b; }

    /** 'Tone smooth' 0..1: HIGH = heavier smoothing (slow colour reaction), LOW =
        fast. Inverted into the circular-lerp rate below, matching Synesthesia and
        Geometry — the label has to mean the same thing in every module.

        This used to be the bare constant 0.15 with no control at all, which made
        Chladni the only tone-colour module you could not tune. The DEFAULT is
        chosen so it still is 0.15 (see kToneSmooth in AlterState): an existing
        preset has no stored value, picks up that default, and looks exactly as it
        did before. */
    void setToneSmooth (float s01)
    {
        toneLerp = PitchUtils::toneSmoothToRate (s01);
    }

    /** 'Mirror tone color': reverses the tone→hue wheel direction (see PitchUtils).
        Only has an effect while colour-by-tone is on; the sand colour is untouched. */
    void setToneTwist (bool b)           { toneTwist = b; }

    /** Global parabolic sub-bin interpolation for the dominant-tone estimate. */
    void setSubBinInterp (bool b)        { subBinInterp = b; }

    /** Plate material preset: 0=Aluminium, 1=Steel, 2=Glass, 3=Acrylic.
        Material thickness differs per preset so each material has a clearly
        different mode map (physically: different plates). */
    void setMaterial (int idx) noexcept
    {
        switch (idx)
        {
            case 0: E = 70.0e9f;  rho = 2700.0f; nuP = 0.33f; h = 0.0010f; break; // Aluminium 1.0 mm
            case 1: E = 200.0e9f; rho = 7800.0f; nuP = 0.28f; h = 0.0005f; break; // Steel 0.5 mm
            case 2: E = 70.0e9f;  rho = 2500.0f; nuP = 0.22f; h = 0.0020f; break; // Glass 2.0 mm
            case 3: E = 3.2e9f;   rho = 1180.0f; nuP = 0.37f; h = 0.0030f; break; // Acrylic 3.0 mm
            default: break;
        }
        modeTableDirty = true;
    }

    int   getM()               const noexcept { return m; }
    int   getN()               const noexcept { return n; }

    /** Resonance frequency of mode (mi,ni) for the current plate. */
    float modeFreq (int mi, int ni) const noexcept
    {
        const float D = (E * h * h * h) / (12.0f * (1.0f - nuP * nuP));
        const float k = (juce::MathConstants<float>::pi / (2.0f * L * L)) * std::sqrt (D / (rho * h));
        return k * ((float) (mi * mi) / (ar * ar) + (float) (ni * ni));
    }

    float getTheoreticalFreq() const noexcept { return modeFreq (m, n); }

    // ── Rendered on the WORKER thread (AsyncVisualBase) ──────────────────────
    //  particles / canvas / modeTable are touched ONLY from this thread, so the
    //  whole simulation runs here lock-free.
    void renderImage (juce::Graphics& g, int w, int h) override
    {
        ensureParticles();
        analyseAudio();
        stepParticles();

        auto b = juce::Rectangle<int> (0, 0, w, h);
        const int W = b.getWidth(), H = b.getHeight();
        paintModuleBackground (g, b.toFloat());
        if (W < 8 || H < 8) return;

        const float fs   = juce::jlimit (9.0f, 13.0f, (float) juce::jmin (W, H) * 0.05f);
        const int   imgH = H;   // full height: the info is now shown by the host overlay (top-right)

        // Render the sand at the module's (already-capped) resolution so each grain is
        // a single clean pixel — no internal downscale-then-upscale blur. The base
        // class then upscales the whole frame crisply (nearest-neighbour).
        renderSand (W, imgH);

        // Fade the sand with the audio level: quieter = fainter (like Synesthesia).
        const float fade = juce::jlimit (0.0f, 1.0f, std::sqrt (juce::jmax (0.0f, drive)));
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);   // 1:1 blit, crisp
        g.setOpacity (fade);
        g.drawImage (canvas, juce::Rectangle<float> ((float) b.getX(), (float) b.getY(),
                                                     (float) W, (float) imgH));
        g.setOpacity (1.0f);

        if (m == n)
        {
            g.setFont (juce::Font (juce::FontOptions (fs)));
            g.setColour (AlterTheme::cerise.withAlpha (juce::jmax (0.25f, fade)));
            g.drawText ("m = n : blank pattern", b, juce::Justification::centred, false);
        }

        // RMS activity border glow
        if (drive > 0.02f)
            AlterTheme::glowRect (g, b.toFloat().reduced (1.5f),
                                  currentSandColour(), drive * 0.7f, 3.0f);
    }

    /** Info text for the host's top-right overlay (mode, m/n, resonance, aspect). */
    juce::String getInfoText() const
    {
        const float fTheo = getTheoreticalFreq();
        const juce::String freq = fTheo < 1000.0f ? juce::String (fTheo, 1) + " Hz"
                                                   : juce::String (fTheo / 1000.0f, 2) + " kHz";
        return juce::String (audioReactive ? "~" : "")
               + "(" + juce::String (m) + "," + juce::String (n) + ")  "
               + freq + "  AR:" + juce::String (ar, 2);
    }

private:
    // ── Physics (defaults = Steel 0.5 mm) ────────────────────────────────────
    float E { 200.0e9f }, rho { 7800.0f }, nuP { 0.28f };
    float h { 0.0005f }, L { 0.30f };

    // ── Chladni function (free-edge, cos-cos antisymmetric) ──────────────────
    inline float chladni (float x, float y) const noexcept
    {
        constexpr float pi = juce::MathConstants<float>::pi;
        const float fm = (float) m, fn = (float) n;
        return   std::cos (fn * pi * x) * std::cos (fm * pi * y / ar)
               - std::cos (fm * pi * x) * std::cos (fn * pi * y / ar);
    }

    // ── Fletcher-Munson / ISO 226:2003 equal-loudness weighting at 60 phon ─────────
    //   Linear amplitude factor: strong cut in the bass (ear insensitive), boost
    //   around 2-4 kHz (most sensitive) → the centroid tracks perceptual loudness.
    static float iso226WeightLinear (float fHz) noexcept
    {
        static const double iso_f[]  = { 20,25,31.5,40,50,63,80,100,125,160,200,250,315,400,
                                         500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,
                                         6300,8000,10000,12500 };
        static const double iso_af[] = { 0.532,0.506,0.480,0.455,0.432,0.409,0.387,0.367,0.349,
                                         0.330,0.315,0.301,0.288,0.276,0.267,0.259,0.253,0.250,
                                         0.246,0.244,0.243,0.243,0.243,0.242,0.242,0.245,0.254,
                                         0.271,0.301 };
        static const double iso_Lu[] = {-31.6,-27.2,-23.0,-19.1,-15.9,-13.0,-10.3,-8.1,-6.2,-4.5,
                                        -3.1,-2.0,-1.1,-0.4,0.0,0.3,0.5,0.0,-2.7,-4.1,-1.0,1.7,2.5,
                                         1.2,-2.1,-7.1,-11.2,-10.7,-3.1 };
        static const double iso_Tf[] = {78.5,68.7,59.5,51.1,44.0,37.5,31.5,26.5,22.1,17.9,14.4,
                                         11.4,8.6,6.2,4.4,3.0,2.2,2.4,3.5,1.7,-1.3,-4.2,-6.0,-5.4,
                                        -1.5,6.0,12.6,13.9,12.3 };
        constexpr int N = 29;
        constexpr double phon = 60.0;

        auto spl = [&] (int idx) -> double
        {
            const double af = iso_af[idx], lu = iso_Lu[idx], tf = iso_Tf[idx];
            const double Af = 4.47e-3 * (std::pow (10.0, 0.025 * phon) - 1.15)
                            + std::pow (0.4 * std::pow (10.0, (tf + lu) / 10.0 - 9.0), af);
            return (10.0 / af) * std::log10 (juce::jmax (Af, 1.0e-30)) - lu + 94.0;
        };
        const double spl1k = spl (17);   // 1 kHz reference

        double f = juce::jlimit ((double) iso_f[0], (double) iso_f[N - 1], (double) fHz);
        int lo = 0;
        for (int k = 0; k < N - 1; ++k)
            if (iso_f[k] <= f && f <= iso_f[k + 1]) { lo = k; break; }
        const double t = (std::log10 (f) - std::log10 (iso_f[lo]))
                       / (std::log10 (iso_f[lo + 1]) - std::log10 (iso_f[lo]));
        const double splF = spl (lo) + t * (spl (lo + 1) - spl (lo));
        return (float) std::pow (10.0, (spl1k - splF) / 20.0);
    }

    // ── Particle simulation ──────────────────────────────────────────────────
    //  Each grain has a finite lifetime (10-20 s). When it expires it respawns
    //  at a random position. Without this, grains slowly accumulate forever on
    //  the main diagonal – at AR=1 it is a nodal line of EVERY antisymmetric
    //  mode (Z(x,x) ≡ 0), so it is the one place sand can reach but never leave.
    struct Particle { float x, y; int life; };

    int randomLife() const
    {
        return 300 + juce::Random::getSystemRandom().nextInt (300);   // 10-20 s @ 30 fps
    }

    void ensureParticles()
    {
        if ((int) particles.size() == particleCount) return;

        auto& rng = juce::Random::getSystemRandom();
        particles.resize ((size_t) particleCount);
        for (auto& p : particles)
            p = { rng.nextFloat(), rng.nextFloat(),
                  rng.nextInt (600) + 1 };   // staggered ages → no mass respawns
    }

    void stepParticles()
    {
        if (particles.empty()) return;

        auto& rng = juce::Random::getSystemRandom();
        const float vib = juce::jmax (drive, scatterKick);
        if (vib < 0.01f) return;                 // silence: sand rests

        constexpr float eps = 0.004f;
        const float step    = baseStep * vib;            // max random hop this frame
        const float maxPull = step * 2.5f;               // max drift toward a node

        auto& lifeRng = juce::Random::getSystemRandom();

        for (auto& p : particles)
        {
            // lifetime: expired grains respawn somewhere fresh
            if (--p.life <= 0)
            {
                p = { lifeRng.nextFloat(), lifeRng.nextFloat(), randomLife() };
                continue;
            }

            const float Z = chladni (p.x, p.y);
            const float A = juce::jmin (std::abs (Z), 2.0f);   // local plate amplitude

            // random walk ∝ amplitude (sand jumps where the plate vibrates)
            p.x += (rng.nextFloat() * 2.0f - 1.0f) * step * (0.10f + A);
            p.y += (rng.nextFloat() * 2.0f - 1.0f) * step * (0.10f + A);

            // drift toward the nodal line: move along the NORMALISED gradient
            // direction by a small, clamped distance (gradients can be huge for
            // high m,n – never use them raw!)
            const float gx = (chladni (p.x + eps, p.y) - Z) / eps;
            const float gy = (chladni (p.x, p.y + eps) - Z) / eps;
            const float gMag = std::sqrt (gx * gx + gy * gy);
            if (gMag > 1.0e-4f)
            {
                const float d = juce::jlimit (-maxPull, maxPull, Z * 0.05f);
                p.x -= d * gx / gMag;
                p.y -= d * gy / gMag;
            }

            // reflect at the plate edges (clamping would pile sand on the rim)
            if (p.x < 0.0f) p.x = -p.x;        else if (p.x > 1.0f) p.x = 2.0f - p.x;
            if (p.y < 0.0f) p.y = -p.y;        else if (p.y > 1.0f) p.y = 2.0f - p.y;
            p.x = juce::jlimit (0.0f, 1.0f, p.x);
            p.y = juce::jlimit (0.0f, 1.0f, p.y);
        }

        scatterKick *= 0.90f;
        if (scatterKick < 0.02f) scatterKick = 0.0f;
    }

    void renderSand (int W, int imgH)
    {
        if (canvas.isNull() || canvas.getWidth() != W || canvas.getHeight() != imgH)
            canvas = juce::Image (juce::Image::ARGB, juce::jmax (1, W), juce::jmax (1, imgH),
                                  false, juce::SoftwareImageType());   // pixel access → software bitmap (Direct2D-safe)

        {   // background plate (Graphics must close before BitmapData opens)
            if (isTransparentBackground())
            {
                // THE PLATE IS A BACKGROUND TOO. paintModuleBackground above already
                // knows to skip the gradient, but this canvas is a second one, drawn
                // over the top — so honouring transparency there and not here put a
                // solid rectangle into every Fusion and every alpha export anyway.
                // The grains are the picture; on transparency they are ALL of it.
                //
                // Cleared explicitly because the image is allocated uninitialised
                // (the gradient used to cover that), and raw memory blitted over a
                // module is how the white rectangle in Spectrum happened.
                canvas.clear (canvas.getBounds(), juce::Colours::transparentBlack);
            }
            else
            {
                juce::Graphics ig (canvas);
                ig.setGradientFill ({ AlterTheme::bgDeep, 0, 0,
                                      AlterTheme::bgVoid, 0, (float) imgH, false });
                ig.fillAll();
            }
        }

        const auto col = currentSandColour();
        juce::Image::BitmapData bd (canvas, juce::Image::BitmapData::readWrite);

        // One clean pixel per grain (no dimmed neighbours → no gradient halo).
        for (const auto& p : particles)
        {
            const int px = (int) (p.x * (float) (W - 1));
            const int py = (int) (p.y * (float) (imgH - 1));
            bd.setPixelColour (px, py, col);
        }
    }

    juce::Colour currentSandColour() const
    {
        // Saturation is the SHARED reference value, not a local taste call. At the
        // old 0.80 the plate drew every note as a pastel while Synesthesia drew the
        // same note as a full primary — same hue, obviously different colour. The
        // grains are single hard pixels, so they show the note's colour undiluted;
        // this is the module the other two are matched against.
        return colourByTone ? juce::Colour::fromHSV (toneHue, PitchUtils::kToneSaturation, 1.0f, 1.0f)
                            : sandColour;
    }

    // MIDI override: exact loudest held note → Hz. >0 = use it; 0/<0 = no exact
    // note (keep the FFT-derived peak). Same premium/auto behaviour as ToneAnalyzer.
    float midiDominantHz()
    {
        if (! audioSource.getMidiNotes (midiScratch)) return 0.0f;
        if (midiScratch.empty())                      return -1.0f;
        const IAudioSource::MidiNote* top = &midiScratch.front();
        for (const auto& n : midiScratch)
            if (n.velocity > top->velocity) top = &n;
        return 440.0f * std::pow (2.0f, ((float) top->note - 69.0f) / 12.0f);
    }

    // ── Audio analysis ───────────────────────────────────────────────────────
    //  Bins are normalised dB [0..1] over 0..nyquist (assumed 48 kHz / 2).
    void analyseAudio()
    {
        const float rms = audioSource.getLastRms();
        const float rmsDb = (rms > 1e-5f) ? 20.0f * std::log10 (rms) : -60.0f;
        const float driveTarget = juce::jmap (juce::jlimit (-60.0f, 0.0f, rmsDb),
                                              -60.0f, 0.0f, 0.0f, 1.0f);
        drive += 0.3f * (driveTarget - drive);

        if (audioSource.getLastFft (fftScratch) < 64)
            return;

        const int   bins  = (int) fftScratch.size();
        const float binHz = (float) (audioSource.getSampleRate() * 0.5) / (float) bins;   // real device rate → correct mode/pitch
        const int   iLo   = juce::jmax (2, (int) (55.0f / binHz));
        const int   iHi   = juce::jmin (bins - 3, (int) (8000.0f / binHz));
        if (iHi <= iLo) return;

        // Dominant spectral peak — PERCEPTUALLY weighted with the Fletcher-Munson /
        // ISO 226 equal-loudness curve at 60 phon, so the pattern tracks the
        // frequency the ear actually leads on. Without this the loud-but-dull bass
        // always wins and the Chladni modes barely move; the weighting lifts the
        // mids/highs, where the eigenmodes (and the patterns) are far more varied.
        int   peakBin = iLo;
        float peakW   = 0.0f;
        float maxRaw  = 0.0f;
        for (int i = iLo; i <= iHi; ++i)
        {
            const float raw = fftScratch[(size_t) i];
            maxRaw = juce::jmax (maxRaw, raw);
            // Perceptual weighting is SOFTENED (square root → half the dB boost) so a
            // boosted 2–4 kHz harmonic no longer beats the fundamental. This removes the
            // strong shift to high values while still stopping dull bass from dominating.
            const float vw = raw * std::sqrt (iso226WeightLinear ((float) i * binHz));
            if (vw > peakW) { peakW = vw; peakBin = i; }
        }

        // ── COLOUR-BY-TONE — runs on its OWN detector, identical to Synesthesia ──
        //
        // The peak found above is the right input for the PHYSICS (which eigenmode the
        // plate falls into) and the wrong input for the COLOUR. It searches 55 Hz-8 kHz
        // and applies ISO 226 weighting that deliberately LIFTS 2-4 kHz — so on most
        // material the winning bin is a harmonic, not the fundamental. The 3rd harmonic
        // is a fifth up and the 5th is a major third up: both land on a DIFFERENT pitch
        // class, so the plate coloured a C as a G or an E. That is why it looked like it
        // wasn't resolving every semitone — it was resolving the wrong note, and two
        // different semitones could easily map onto the same harmonic-derived class.
        //
        // The hue therefore uses exactly Synesthesia's detector: 65 Hz-2 kHz (the
        // musically meaningful range, where the fundamental wins), flat magnitude, same
        // floor, same sub-bin refinement, same 0.15 circular lerp. Nothing here touches
        // the mode picking below, which keeps its perceptual weighting.
        //
        // It also sits BEFORE the freeze guard on purpose: the pattern should hold still
        // on a weak signal, but the colour should keep tracking a quiet but clearly
        // pitched note, exactly as Synesthesia does.
        {
            float hueHz = midiDominantHz();          // exact held MIDI note wins
            if (hueHz == 0.0f)                       // no MIDI on this track → FFT
            {
                const double sr = audioSource.getSampleRate();
                const float  bw = PitchUtils::binWidthHz (bins, sr);
                const int    lo = juce::jmax (1, (int) std::round (65.0f   / bw));
                const int    hi =               (int) std::round (2000.0f / bw);
                hueHz = PitchUtils::dominantFrequency (fftScratch, lo, hi, 0.01f,
                                                       subBinInterp, sr);
            }

            if (hueHz > 0.0f)                        // <0 = MIDI held nothing → hold hue
            {
                const float targetHue = PitchUtils::hueFromHz (hueHz, toneTwist);   // shared wheel
                float d = targetHue - toneHue;                   // circular lerp
                if (d >  0.5f) d -= 1.0f;
                if (d < -0.5f) d += 1.0f;
                toneHue = std::fmod (toneHue + toneLerp * d + 1.0f, 1.0f);
            }
        }

        if (maxRaw < 0.10f || drive < 0.03f)   // no meaningful signal: freeze pattern
        {
            stableFrames = 0;
            return;
        }

        // Optional sub-bin refinement of the dominant peak (sharper mode pick).
        float peakHz = PitchUtils::refinePeakBin (fftScratch, peakBin, subBinInterp) * binHz;
        if (const float mh = midiDominantHz(); mh > 0.0f)
            peakHz = mh;   // exact held MIDI note overrides the FFT peak

        if (! audioReactive) return;

        // ── PHYSICAL: the excitation frequency drives the eigenmode whose
        //    resonance is nearest (in log-frequency = perceptual distance) ────
        if (modeTableDirty) buildModeTable();

        int   bestM = matchedM, bestN = matchedN;
        float bestDist = 1.0e9f;
        for (const auto& md : modeTable)
        {
            const float dist = std::abs (std::log2 (md.f / peakHz));
            if (dist < bestDist) { bestDist = dist; bestM = md.m; bestN = md.n; }
        }

        // hysteresis: the new mode must hold ~0.13 s before the plate snaps
        if (bestM == pendingM && bestN == pendingN)
            ++stableFrames;
        else
        {
            pendingM = bestM;
            pendingN = bestN;
            stableFrames = 0;
        }

        if (stableFrames >= 4 && (pendingM != matchedM || pendingN != matchedN))
        {
            matchedM = pendingM;
            matchedN = pendingN;
            matchedValid = true;
            stableFrames = 0;
            applyMatchedMode (true);
        }
    }

    /** Applies matched mode + user shift to the displayed (m,n). */
    void applyMatchedMode (bool shake)
    {
        const int newM = juce::jlimit (1, 12, matchedM + modeShift);
        int       newN = juce::jlimit (1, 12, matchedN + modeShift);
        if (newM == newN)                       // both clamped to the same value
            newN = (newN > 1) ? newN - 1 : newN + 1;

        if (newM != m || newN != n)
        {
            m = newM;
            n = newN;
            if (shake) onModeChanged();
        }
    }

    // ── Eigenmode table: all modes (m<n, 1..12) of the current plate ─────────
    void buildModeTable()
    {
        modeTable.clear();
        for (int mi = 1; mi <= 12; ++mi)
            for (int ni = mi + 1; ni <= 12; ++ni)        // m<n: (n,m) has identical nodal lines
                modeTable.push_back ({ mi, ni, modeFreq (mi, ni) });
        modeTableDirty = false;
    }

    void onModeChanged() noexcept { scatterKick = 0.6f; }  // sand "shake"

    // ── Members ───────────────────────────────────────────────────────────────
    IAudioSource& audioSource;

    int  m { 2 }, n { 3 };
    int  manualM { 2 }, manualN { 3 };
    bool audioReactive { true };
    bool colourByTone  { false };
    // Circular-lerp RATE, not the slider value — setToneSmooth inverts. 0.15 is the
    // constant this was hardcoded to before the slider existed, so an untouched
    // module behaves identically.
    float toneLerp     { 0.15f };
    bool toneTwist     { false };   // 'Mirror tone color': reverse the hue wheel direction
    bool subBinInterp  { true };   // always on

    int  pendingM { 2 }, pendingN { 3 };
    int  matchedM { 2 }, matchedN { 3 };   // matched eigenmode (pre-shift)
    bool matchedValid { false };
    int  modeShift { 0 };
    int  stableFrames { 0 };
    std::vector<float> fftScratch;
    std::vector<IAudioSource::MidiNote> midiScratch;   // reused per frame (MIDI override)

    struct Mode { int m, n; float f; };
    std::vector<Mode> modeTable;
    bool modeTableDirty { true };

    float ar { 1.0f };
    int   particleCount { 5000 };
    float baseStep { 0.010f };     // sand movement (from smoothness)
    float drive { 0.0f };          // RMS-driven vibration 0..1
    float scatterKick { 0.0f };    // burst agitation after mode change
    float toneHue { 0.0f };

    juce::Colour sandColour { AlterTheme::iceBlue };

    std::vector<Particle> particles;
    juce::Image canvas;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChladniPatternMeter)
};
