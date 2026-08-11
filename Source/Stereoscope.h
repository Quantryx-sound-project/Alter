/*
  ==============================================================================

    Stereoscope.h
    Stereo field visualiser for ALTER.

    Five modes:
      0 PARTICLES   – vertical log-frequency axis with Hz checkpoints; for every
                      frequency band particles (single colour) spread out from a
                      central line by how much stereo plays in each channel. The
                      outer edge = full scale (0 dBFS / clip); inaudible (floor)
                      content draws nothing. DENSITY sets the particle count.
      1 GONIOMETER  – Lissajous mid/side scope on a circular grid. The circle is
                      the clipping level; the trace is clipped to it. PARTICLES
                      toggles between a clean line and a particle cloud (with
                      persistence); DENSITY sets the cloud's particle count.
      2 POLAR       – polar phase scope: samples shoot from the bottom centre
                      upward. 0° (up) = pure mono; the ±45° guides are the safe
                      limit, points past them (toward ±90° horizontal) are
                      anti-phase and cancel in mono. Radius = level (arc = clip).
      3 CORRELATION – scrolling history of L/R correlation (+1 in phase, 0
                      decorrelated, -1 anti-phase) plus the L/R balance line.
      4 CORRELOMETER – spectral correlometer: per-band L/R correlation as an
                      analog LED bar-graph over a log frequency axis. 1 (top)
                      = fully mono / in phase, 0 (bottom) = fully stereo
                      (decorrelated). 512 FFT bins by default; "use controller
                      bins" follows the global Max-bins setting instead.

    There is no sensitivity / gain control on purpose: every mode shows the
    absolute, true level so the clip edge means real clipping.

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <vector>
#include <array>
#include <cmath>
#include "AudioSourceInterface.h"
#include "AlterTheme.h"
#include "AsyncVisualBase.h"
#include "PitchUtils.h"

class VisualStereoscope : public AsyncVisualBase
{
public:
    explicit VisualStereoscope (IAudioSource& src)
        : AsyncVisualBase ("AlterStereoscope", 45), audioSource (src)
    {
        setCrispUpscale (true);   // keep particles/scope sharp when enlarged
        startAsyncRender();
    }

    // Stop the worker BEFORE our members are destroyed (it calls renderImage()).
    ~VisualStereoscope() override { stopAsyncRender(); }

    // ── Control API ───────────────────────────────────────────────────────────
    void setStereoMode (int m) noexcept { mode = juce::jlimit (0, 4, m); repaint(); }
    int  getStereoMode() const noexcept { return mode; }

    /** Correlometer: follow the controller's global Max-bins setting (else 512). */
    void setUseControllerBins (bool b) noexcept { useCtrlBins = b; }
    void setControllerBins (int n)     noexcept { ctrlBins = juce::jlimit (512, 8192, n); }

    void setColour (juce::Colour c) noexcept { lineColour = c; repaint(); }

    // ── Colour by tone ────────────────────────────────────────────────────────
    //  All five modes (Particles, Goniometer, Vectorscope, Correlation,
    //  Correlometer) build their palette out of ONE colour, so routing every read
    //  of it through activeColour() gives tone colour in all of them at once.
    //
    //  The deliberate exception is AlterTheme::cerise, the anti-phase / out-of-spec
    //  warning colour. That is a SEMANTIC colour, not a decorative one — it means
    //  "this content will collapse in mono". If it followed the note it would stop
    //  contrasting with the safe material the instant the tone landed near it, and
    //  the one thing these modes exist to warn about would become invisible.
    void setColourByTone (bool b) noexcept
    {
        if (colourByTone == b) return;
        colourByTone = b;
        if (b) toneHue.reset();   // snap to what is sounding, don't slide from a stale hue
        repaint();
    }

    /** 'Mirror tone color' — reverses the tone→hue wheel (see PitchUtils). */
    void setToneTwist (bool b) noexcept { toneTwist = b; repaint(); }

    /** 'Tone smooth' 0..1: HIGH = heavier smoothing (slow colour reaction), LOW =
        fast. Stored RAW — PitchUtils::toneSmoothToRate turns it into a chase rate,
        so the curve is defined once instead of once per module. */
    void setToneSmooth (float s01) noexcept { toneSmooth = juce::jlimit (0.0f, 1.0f, s01); }

    /** Goniometer: render as a particle cloud instead of a line. */
    void setParticles (bool b) noexcept { gonioParticles = b; repaint(); }

    /** 0..1 particle density (Particles mode + Goniometer particle cloud). */
    void setDensity (float d) noexcept { density = juce::jlimit (0.0f, 1.0f, d); repaint(); }

    /** 0..1 trace brightness for Goniometer / Polar (plain gain, no glow). */
    void setBrightness (float b) noexcept { brightness = juce::jlimit (0.0f, 1.0f, b); repaint(); }

    /** 0..1 LINE width for the Goniometer's continuous beam. 0.5 keeps the old
        1.6 px stroke, so an existing patch looks exactly as it did. */
    void setLineWidth (float w01) noexcept { lineWidth = juce::jlimit (0.0f, 1.0f, w01); repaint(); }

    /** 0..1 POINT size for every dotted trace — Particles mode, the Goniometer's
        particle cloud and the Polar scope. Same neutral-at-0.5 convention. */
    void setPointSize (float p01) noexcept { pointSize = juce::jlimit (0.0f, 1.0f, p01); repaint(); }

    /** 0..1 smoothing; also sets the Vectorscope / particle persistence. */
    void setSmoothAmount (float s01) noexcept
    {
        s01 = juce::jlimit (0.0f, 1.0f, s01);
        smoothAlpha = juce::jmap (s01, 0.0f, 1.0f, 1.0f, 0.06f);  // 1=instant, .06=very smooth
        persistence = juce::jmap (s01, 0.0f, 1.0f, 0.0f, 0.94f);  // 0=no trail, .94=long trails
    }

    void setAssumedSampleRate (double sr) noexcept { assumedSampleRateHz = (sr > 0.0 ? sr : 48000.0); }

    // ── Rendered on the WORKER thread (AsyncVisualBase) ──────────────────────
    //  All buffers / ping-pong images / FFT below are touched only from here, so
    //  the whole module renders lock-free.
    void renderImage (juce::Graphics& g, int w, int h) override
    {
        // ONE tone update per rendered frame, before anything reads activeColour().
        // The mode paint functions call activeColour() many times each; it must stay
        // a pure read, so the advance happens here and only here.
        if (colourByTone)
            toneHue.update (audioSource, toneTwist, toneSmooth);

        updateCorrelationHistory();   // keep the scrolling history advancing in every mode

        auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h);
        paintModuleBackground (g, bounds);
        // Kept in a transparent export like every other readable detail; "Hide
        // info" is the one switch that strips the module back to its picture.
        if (! AlterTheme::hudInfoHidden.load())
        {
            g.setColour (AlterTheme::navyEdge);
            g.drawRect (bounds, 1.0f);
        }

        // Audio-reactive intensity (quieter = fainter, like Synesthesia / Geometry).
        {
            const float raw = audioSource.getLastRms();
            const float lvl = (raw < 0.001f) ? 0.0f
                            : juce::jlimit (0.0f, 1.0f, (20.0f * std::log10 (raw) + 36.0f) / 36.0f);
            drive += 0.25f * (lvl - drive);
        }
        // sqrt curve: mid levels sit much closer to full intensity — the old linear
        // fade left the goniometer/polar traces at ~half brightness (grey) on
        // typical programme material.
        const float fade = 0.12f + 0.88f * std::sqrt (juce::jmax (0.0f, drive));
        g.setOpacity (fade);

        switch (mode)
        {
            case 1:  paintGoniometer   (g, bounds.reduced (4.0f));  break;
            case 2:  paintPolar        (g, bounds.reduced (4.0f));  break;
            case 3:  paintCorrelation  (g, bounds.reduced (4.0f));  break;
            case 4:  paintCorrelometer (g, bounds.reduced (4.0f));  break;
            default: paintParticles    (g, bounds.reduced (1.0f));  break;
        }

        g.setOpacity (1.0f);
    }

private:
    // =========================================================================
    // PARTICLES – log-frequency line, particles spread by per-channel stereo.
    // Edge = full scale (clip); the dB floor means inaudible content vanishes.
    // =========================================================================
    void paintParticles (juce::Graphics& g, juce::Rectangle<float> area)
    {
        // ONE analysis pass for all four spectra. L, R, MID and SIDE all fall out
        // of the same two complex transforms, so asking for them separately (as
        // computeStereoFfts + computeStereoMidSide did) fetched the waveform
        // twice, built four FFTs where two suffice, and heap-allocated ~128 KB of
        // scratch every single frame. On a 45 fps module that is ~5.7 MB/s of
        // allocator traffic on the worker thread — enough on its own to make the
        // frame time wander, which is felt as the cloud lurching.
        const int bins = computeStereoSpectra();
        const auto& specL = anaL; const auto& specR = anaR;
        const auto& specM = anaM; const auto& specS = anaS;

        const float cx = area.getCentreX();
        const float halfW = area.getWidth() * 0.5f - 4.0f;

        const double fmin = 20.0, fmax = 20000.0;
        const double logSpan = std::log10 (fmax / fmin);

        auto yForFreq = [&] (double f) -> float
        {
            const double u = std::log10 (juce::jlimit (fmin, fmax, f) / fmin) / logSpan;
            return area.getBottom() - (float) u * area.getHeight();
        };

        g.setColour (activeColour().withAlpha (0.30f));
        g.fillRect (cx - 0.5f, area.getY(), 1.0f, area.getHeight());

        const double checkHz[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
        g.setFont (10.0f);
        for (double hz : checkHz)
        {
            const float y = yForFreq (hz);
            g.setColour (juce::Colours::grey.withAlpha (0.18f));
            g.drawHorizontalLine ((int) std::round (y), area.getX(), area.getRight());
            g.setColour (juce::Colours::grey.withAlpha (0.7f));
            const juce::String txt = (hz >= 1000.0) ? juce::String (hz / 1000.0, 0) + "k"
                                                    : juce::String ((int) hz);
            g.drawText (txt, (int) area.getX() + 3, (int) y - 11, 34, 11, juce::Justification::centredLeft);
        }

        if (bins <= 0) return;

        // smL/smR = smoothed LEFT/RIGHT spectra; smM/smS = smoothed MID/SIDE spectra
        if ((int) smL.size() != bins)
        {
            smL.assign ((size_t) bins, 0.0f); smR.assign ((size_t) bins, 0.0f);
            smM.assign ((size_t) bins, 0.0f); smS.assign ((size_t) bins, 0.0f);
        }
        for (int i = 0; i < bins; ++i)
        {
            smL[(size_t) i] += smoothAlpha * (specL[(size_t) i] - smL[(size_t) i]);
            smR[(size_t) i] += smoothAlpha * (specR[(size_t) i] - smR[(size_t) i]);
            if (i < (int) specM.size())
            {
                smM[(size_t) i] += smoothAlpha * (specM[(size_t) i] - smM[(size_t) i]);
                smS[(size_t) i] += smoothAlpha * (specS[(size_t) i] - smS[(size_t) i]);
            }
        }

        const double binHz = (assumedSampleRateHz * 0.5) / (double) bins;
        const float countScale = juce::jmap (density, 0.0f, 1.0f, 1.0f, 6.0f);

        const juce::Colour vivid = activeColour().brighter (0.25f);   // more vibrant at the peaks
        // NOT `r`: the per-bin loop below already binds that name to the RIGHT
        // channel's level.
        const float dotR = particleRadiusPx();

        // ── WHY THE CLOUD USED TO SEEM TO STUTTER ────────────────────────────
        //
        // It was not the frame rate. Every particle's offset came from
        // rng.nextFloat() called fresh inside the draw loop, so no particle had
        // any identity from one frame to the next: the whole cloud was re-diced
        // sixty times a second. That is not motion, it is boiling, and the eye
        // reads a field of uncorrelated jumps as the harshest possible stutter —
        // which is why raising the fps never helped and never could.
        //
        // Now each particle is addressed by (bin, index) and keeps a PHASE and a
        // RATE derived from that address, both stable for as long as it exists.
        // Its offset is a sine of that phase advanced by the presentation clock,
        // so it travels a smooth continuous path. The cloud's shape still comes
        // from the audio (pan, width and energy, all already smoothed); what has
        // gone is the per-frame randomness laid on top of it.
        //
        // RELATIVE to the first frame, and that is not tidiness. The present
        // clock is an absolute reading in seconds — on a machine up for a week it
        // is around 6e5, where a float's spacing is about 0.06. Feeding that
        // straight into sin() would quantise the phase to ~60 ms steps and
        // reintroduce, in a subtler form, exactly the jerk this replaces. The
        // subtraction is done in double, and only the small result narrows.
        if (particleT0 <= 0.0) particleT0 = getPresentTimeSec();
        const float tNow = (float) (getPresentTimeSec() - particleT0);

        // Cheap integer hash -> two stable values in 0..1 for one particle.
        auto hash01 = [] (juce::uint32 a, juce::uint32 b) noexcept -> float
        {
            juce::uint32 x = a * 73856093u ^ b * 19349663u;
            x ^= x >> 16; x *= 0x7feb352du;
            x ^= x >> 15; x *= 0x846ca68bu;
            x ^= x >> 16;
            return (float) (x & 0xffffffu) * (1.0f / 16777215.0f);
        };

        // ── ONE DRAW CALL PER ALPHA BUCKET ───────────────────────────────────
        //
        // The old code issued a setColour + fillEllipse PER PARTICLE. At full
        // density that is up to ~30 000 anti-aliased path fills in a single
        // frame, each one setting up and tearing down its own rasterisation —
        // by far the most expensive thing this module did, and the reason the
        // frame time was long enough for the load governor to halve the rate.
        //
        // The Goniometer and the Polar scope were converted to batched
        // RectangleLists for exactly this reason (see their comments); Particles
        // was the one mode left doing it the slow way. Quantising alpha into a
        // few buckets costs nothing visible on dots this small and turns the
        // whole cloud into kNumAlphaBuckets fills.
        for (auto& b : particleBuckets) b.clear();

        // Placement per frequency:
        //   • PAN (L/R balance) = the cloud's CENTRE: only-left → far LEFT,
        //     only-right → far RIGHT, equal → centre.
        //   • WIDTH (SIDE vs MID energy, phase-aware) = how far the cloud SPREADS
        //     to the sides: mono/correlated → a tight dot in the middle, real
        //     (decorrelated) stereo → particles fan out towards L and R even when
        //     both channel levels are equal.
        for (int i = 1; i < bins; ++i)
        {
            const double f = ((double) i + 0.5) * binHz;
            if (f < fmin || f > fmax) continue;

            const float l = juce::jlimit (0.0f, 1.0f, smL[(size_t) i]);
            const float r = juce::jlimit (0.0f, 1.0f, smR[(size_t) i]);
            const float e = juce::jmax (l, r);                             // energy at this frequency
            if (e < 0.02f) continue;                                        // floor → invisible

            const float y   = yForFreq (f);
            const float raw = (r - l) / (l + r + 1.0e-4f);                  // -1 (L) .. +1 (R)
            float pan = (raw < 0.0f ? -1.0f : 1.0f) * std::sqrt (juce::jmin (1.0f, std::abs (raw)));
            pan = juce::jlimit (-1.0f, 1.0f, pan);

            // stereo width 0..1: SIDE share of the total mid/side energy (sqrt = sensitive)
            float width = 0.0f;
            if (i < (int) smM.size())
            {
                const float m  = juce::jlimit (0.0f, 1.0f, smM[(size_t) i]);
                const float sd = juce::jlimit (0.0f, 1.0f, smS[(size_t) i]);
                if (m + sd > 1.0e-4f)
                    width = std::sqrt (sd / (m + sd));
            }

            const int np = juce::jmax (1, (int) (e * countScale * 2.5f));

            // ALPHA IS PER BIN, not per particle: every dot of one frequency
            // already shared the same 0.3 + 0.7 * e, so the bucket is picked once
            // here instead of once per dot.
            const float  a   = juce::jlimit (0.08f, 1.0f, 0.3f + 0.7f * e);
            const int    bkt = juce::jlimit (0, kNumAlphaBuckets - 1,
                                             (int) (a * (float) kNumAlphaBuckets));
            auto& list = particleBuckets[(size_t) bkt];

            for (int k = 0; k < np; ++k)
            {
                const juce::uint32 id = (juce::uint32) k;

                // Stable identity: phase and rate belong to THIS particle and do
                // not change between frames, so its path is continuous.
                const float ph   = hash01 ((juce::uint32) i, id) * juce::MathConstants<float>::twoPi;
                const float rate = 0.35f + 0.9f * hash01 ((juce::uint32) i, id ^ 0x9e37u);
                const float phY  = hash01 ((juce::uint32) i, id ^ 0x51edu) * juce::MathConstants<float>::twoPi;

                // symmetric spread around the pan centre, clamped to the plot.
                // sin() of a stable phase, so the dot SLIDES across the width the
                // audio asks for rather than being re-thrown into it each frame.
                const float spread = std::sin (ph + tNow * rate) * width;
                const float px     = juce::jlimit (-1.0f, 1.0f, pan + spread);
                const float x      = cx + px * halfW;

                // the same small cloud jitter as before — but drifting, not diced
                const float jx = std::sin (phY + tNow * rate * 0.7f) * 2.0f;
                const float jy = y + std::cos (ph + tNow * rate * 0.5f) * 1.5f;

                list.addWithoutMerging ({ x + jx - dotR, jy - dotR, dotR * 2.0f, dotR * 2.0f });
            }
        }

        for (int b = 0; b < kNumAlphaBuckets; ++b)
        {
            auto& list = particleBuckets[(size_t) b];
            if (list.isEmpty()) continue;
            const float a = ((float) b + 0.5f) / (float) kNumAlphaBuckets;
            g.setColour (vivid.withAlpha (juce::jlimit (0.08f, 1.0f, a)));
            g.fillRectList (list);
        }
    }

    // =========================================================================
    // Shared circular grid for the scope modes
    // =========================================================================
    void drawScopeGrid (juce::Graphics& g, float cx, float cy, float radius, bool rings)
    {
        g.setColour (AlterTheme::navyEdge.withAlpha (0.8f));
        g.drawEllipse (cx - radius, cy - radius, radius * 2, radius * 2, 1.0f);
        if (rings)
        {
            g.setColour (AlterTheme::navyEdge.withAlpha (0.35f));
            g.drawEllipse (cx - radius * 0.66f, cy - radius * 0.66f, radius * 1.32f, radius * 1.32f, 1.0f);
            g.drawEllipse (cx - radius * 0.33f, cy - radius * 0.33f, radius * 0.66f, radius * 0.66f, 1.0f);
        }
        g.setColour (activeColour().withAlpha (0.16f));
        g.drawLine (cx, cy - radius, cx, cy + radius, 1.0f);
        g.drawLine (cx - radius, cy, cx + radius, cy, 1.0f);
        const float d = radius * 0.7071f;
        g.setColour (activeColour().withAlpha (0.10f));
        g.drawLine (cx - d, cy - d, cx + d, cy + d, 1.0f);
        g.drawLine (cx - d, cy + d, cx + d, cy - d, 1.0f);
        g.setColour (juce::Colours::grey.withAlpha (0.7f));
        g.setFont (10.0f);
        g.drawText ("L", (int) (cx - d - 14), (int) (cy - d - 6), 14, 12, juce::Justification::centred);
        g.drawText ("R", (int) (cx + d + 2),  (int) (cy - d - 6), 14, 12, juce::Justification::centred);
        g.drawText ("M", (int) cx - 7, (int) (cy - radius - 2), 14, 12, juce::Justification::centred);
    }

    // Accumulate a draw into a faded ping-pong image (motion-blur persistence),
    // then blit it over the grid. The draw lambda gets the image graphics and its
    // pixel size and is responsible for its own clipping/geometry (image-local).
    template <typename Fn>
    void accumulateScope (juce::Graphics& g, juce::Rectangle<float> area, Fn&& draw)
    {
        const int w = (int) area.getWidth(), h = (int) area.getHeight();
        if (w <= 0 || h <= 0) return;
        if (scopeW != w || scopeH != h)
        {
            // Force SOFTWARE bitmaps: with the Direct2D renderer, drawing into and
            // clearing a native (GPU) Image round-trips through readback and crashes.
            scopeImgA = juce::Image (juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
            scopeImgB = juce::Image (juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
            scopeW = w; scopeH = h;
        }

        juce::Image& dst = scopeUseA ? scopeImgA : scopeImgB;
        juce::Image& src = scopeUseA ? scopeImgB : scopeImgA;

        dst.clear (dst.getBounds());
        {
            juce::Graphics ig (dst);
            ig.setOpacity (persistence);
            ig.drawImageAt (src, 0, 0);
            ig.setOpacity (1.0f);
            draw (ig, (float) w, (float) h);
        }
        scopeUseA = ! scopeUseA;

        // full-alpha blit: the trace intensity is controlled purely by the
        // colours/alphas drawn into the image (i.e. by BRIGHTNESS), nothing
        // downstream dims it.
        g.setOpacity (1.0f);
        g.drawImageAt (dst, (int) area.getX(), (int) area.getY());
    }

    // =========================================================================
    // GONIOMETER – Lissajous scope. Circle = clip level; trace clipped to it.
    // Line trace, or particle cloud (with persistence) when enabled.
    // =========================================================================
    void paintGoniometer (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const float cx = area.getCentreX(), cy = area.getCentreY();
        const float radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f - 2.0f;

        drawScopeGrid (g, cx, cy, radius, true);   // bg circle + rings + guides

        std::vector<float> wL, wR;
        const int n = audioSource.getLastWaveform (wL, wR);
        if (n < 2) return;

        const float norm = radius;                 // single channel 0 dBFS → on circle
        const bool particles = gonioParticles;
        const int target = (int) juce::jmap (density, 0.0f, 1.0f, 256.0f, 4096.0f);
        const int step = particles ? juce::jmax (1, n / target) : juce::jmax (1, n / 2048);

        // BRIGHTNESS: plain, strong gain. Below 0.5 dims via alpha, above 0.5
        // pushes the colour towards white-hot. No glow, no extra draw cost.
        const float bGain = juce::jmap (brightness, 0.0f, 1.0f, 0.30f, 2.4f);

        // persistence image so SMOOTH (trail length) works for line AND particles
        accumulateScope (g, area, [&] (juce::Graphics& ig, float w, float h)
        {
            const float icx = w * 0.5f, icy = h * 0.5f;
            ig.saveState();
            juce::Path clip;
            clip.addEllipse (icx - radius, icy - radius, radius * 2, radius * 2);
            ig.reduceClipRegion (clip);

            // VIBRANT trace: saturated + brightened colour, and a soft halo layer
            // under a full-alpha core so the scope pops instead of reading grey.
            // PERF: the halo/core points are batched into RectangleLists and filled
            // with TWO draw calls total — per-point fillRect calls (thousands per
            // frame) made the enlarged goniometer stutter.
            const juce::Colour lc = activeColour().withMultipliedSaturation (1.25f)
                                       .brighter (0.18f + 0.8f * juce::jmax (0.0f, bGain - 1.0f));
            if (particles)
            {
                // Sizes come from the POINT SIZE control (neutral at 0.5 = the
                // 2.1 / 0.9 this used to hard-code), so the cloud can be dialled
                // down to a fine mist without touching the beam width.
                const float hr = dotHaloPx(), cr = dotCorePx();

                juce::RectangleList<float> halo, core;
                for (int i = 0; i < n; i += step)
                {
                    const float l = wL[(size_t) i], r = wR[(size_t) i];
                    const float x = icx + (r - l) * 0.7071f * norm;
                    const float y = icy - (l + r) * 0.7071f * norm;
                    halo.addWithoutMerging ({ x - hr, y - hr, hr * 2.0f, hr * 2.0f });
                    core.addWithoutMerging ({ x - cr, y - cr, cr * 2.0f, cr * 2.0f });
                }
                ig.setColour (lc.withAlpha (juce::jlimit (0.04f, 0.60f, 0.25f * bGain))); ig.fillRectList (halo);
                ig.setColour (lc.withAlpha (juce::jlimit (0.10f, 1.0f, bGain)));          ig.fillRectList (core);
            }
            else
            {
                juce::Path beam;
                bool started = false;
                for (int i = 0; i < n; i += step)
                {
                    const float l = wL[(size_t) i], r = wR[(size_t) i];
                    const float x = icx + (r - l) * 0.7071f * norm;
                    const float y = icy - (l + r) * 0.7071f * norm;
                    if (! started) { beam.startNewSubPath (x, y); started = true; }
                    else             beam.lineTo (x, y);
                }
                // single slightly-thicker bright stroke: vivid without the cost of
                // a second wide glow stroke (that pass stuttered when enlarged).
                //
                // WIDTH is now the user's, with the old brightness-driven bump
                // (2.0 px above bGain 1.6, 1.6 px below) kept as a small
                // MULTIPLIER rather than an absolute — otherwise turning the beam
                // right down would still snap back to 2 px the moment the
                // brightness went past its threshold, and the thin setting would
                // simply stop working at high brightness.
                ig.setColour (lc.withAlpha (juce::jlimit (0.10f, 1.0f, 0.95f * bGain)));
                ig.strokePath (beam, juce::PathStrokeType (
                    juce::jmax (0.2f, beamStrokePx() * (bGain > 1.6f ? 1.25f : 1.0f))));
            }
            ig.restoreState();
        });
    }

    // =========================================================================
    // POLAR – polar phase scope. Samples shoot from the bottom centre upward.
    // 0° (straight up) = pure mono; the ±45° guides are the safe limit; points
    // past them (toward the horizontal ±90°) are anti-phase and cancel in mono.
    // Radius = level (the arc = full scale / clip; over-level is cropped).
    // =========================================================================
    void polarGeometry (float W, float H, float& ox, float& oy, float& R) const
    {
        const float bottom = 8.0f;
        ox = W * 0.5f;
        oy = H - bottom;
        R  = juce::jmin (W * 0.5f - 4.0f, H - bottom - 4.0f);
    }

    void paintPolar (juce::Graphics& g, juce::Rectangle<float> area)
    {
        float ox, oy, R;
        polarGeometry (area.getWidth(), area.getHeight(), ox, oy, R);
        ox += area.getX();
        oy += area.getY();

        // ── grid ──
        juce::Path arc;
        arc.addCentredArc (ox, oy, R, R, 0.0f, -juce::MathConstants<float>::halfPi,
                           juce::MathConstants<float>::halfPi, true);
        g.setColour (AlterTheme::navyEdge.withAlpha (0.8f));
        g.strokePath (arc, juce::PathStrokeType (1.0f));
        g.drawLine (ox - R, oy, ox + R, oy, 1.0f);                       // baseline

        // level checkpoint arcs
        g.setColour (AlterTheme::navyEdge.withAlpha (0.35f));
        for (float fr : { 0.33f, 0.66f })
        {
            juce::Path a2;
            a2.addCentredArc (ox, oy, R * fr, R * fr, 0.0f, -juce::MathConstants<float>::halfPi,
                              juce::MathConstants<float>::halfPi, true);
            g.strokePath (a2, juce::PathStrokeType (1.0f));
        }

        auto rayEnd = [&] (float deg) -> juce::Point<float>
        {
            const float a = juce::degreesToRadians (deg);
            return { ox + std::sin (a) * R, oy - std::cos (a) * R };
        };

        // mono (0°) line
        g.setColour (activeColour().withAlpha (0.22f));
        g.drawLine (ox, oy, ox, oy - R, 1.0f);
        // ±45° safe-limit guides (highlighted)
        g.setColour (AlterTheme::cerise.withAlpha (0.55f));
        { auto e = rayEnd (-45.0f); g.drawLine (ox, oy, e.x, e.y, 1.2f); }
        { auto e = rayEnd ( 45.0f); g.drawLine (ox, oy, e.x, e.y, 1.2f); }

        g.setColour (juce::Colours::grey.withAlpha (0.8f));
        g.setFont (10.0f);
        g.drawText ("M", (int) ox - 7, (int) (oy - R - 13), 14, 12, juce::Justification::centred);
        g.drawText ("L", (int) (ox - R), (int) oy - 14, 22, 12, juce::Justification::centredLeft);
        g.drawText ("R", (int) (ox + R - 20), (int) oy - 14, 22, 12, juce::Justification::centredRight);
        g.setColour (AlterTheme::cerise.withAlpha (0.7f));
        { auto e = rayEnd (-45.0f); g.drawText ("-45", (int) e.x - 24, (int) e.y - 12, 24, 12, juce::Justification::centredRight); }
        { auto e = rayEnd ( 45.0f); g.drawText ("+45", (int) e.x + 2,  (int) e.y - 12, 24, 12, juce::Justification::centredLeft); }

        std::vector<float> wL, wR;
        const int n = audioSource.getLastWaveform (wL, wR);
        if (n < 2) return;

        const int target = (int) juce::jmap (density, 0.0f, 1.0f, 512.0f, 6144.0f);
        const int step = juce::jmax (1, n / target);
        // BRIGHTNESS: plain, strong gain (see goniometer) — no glow
        const float bGain = juce::jmap (brightness, 0.0f, 1.0f, 0.30f, 2.4f);
        // vibrant: saturated/brightened colours + halo-under-core points (see gonio)
        const float hot = 0.8f * juce::jmax (0.0f, bGain - 1.0f);
        const juce::Colour safeCol = activeColour().withMultipliedSaturation (1.25f).brighter (0.18f + hot);
        const juce::Colour antiCol = AlterTheme::cerise.brighter (0.12f + hot);

        accumulateScope (g, area, [&] (juce::Graphics& ig, float w, float h)
        {
            float iox, ioy, iR;
            polarGeometry (w, h, iox, ioy, iR);

            // clip to the upper half-disc
            ig.saveState();
            juce::Path clip;
            clip.addEllipse (iox - iR, ioy - iR, iR * 2, iR * 2);
            ig.reduceClipRegion (clip);
            ig.reduceClipRegion (juce::Rectangle<int> (0, 0, (int) w, (int) std::ceil (ioy)));

            // batched halo+core (4 fillRectList calls total — per-point fillRect
            // calls made the enlarged scope stutter)
            const float hr = dotHaloPx(), cr = dotCorePx();   // POINT SIZE control

            juce::RectangleList<float> safeHalo, safeCore, antiHalo, antiCore;
            for (int i = 0; i < n; i += step)
            {
                const float l = wL[(size_t) i], r = wR[(size_t) i];
                const float m   = l + r;
                const float s   = r - l;                                  // L-dominant → left
                const float mag = std::sqrt (l * l + r * r) * 0.7071f;    // mono 0dBFS → 1
                const float ang = std::atan2 (s, std::abs (m));           // 0=mono, ±90=anti
                const float x = iox + std::sin (ang) * mag * iR;
                const float y = ioy - std::cos (ang) * mag * iR;

                const bool anti = std::abs (ang) > juce::MathConstants<float>::pi * 0.25f;
                (anti ? antiHalo : safeHalo).addWithoutMerging ({ x - hr, y - hr, hr * 2.0f, hr * 2.0f });
                (anti ? antiCore : safeCore).addWithoutMerging ({ x - cr, y - cr, cr * 2.0f, cr * 2.0f });
            }
            const float haloA = juce::jlimit (0.04f, 0.60f, 0.25f * bGain);
            const float coreA = juce::jlimit (0.10f, 1.0f,  bGain);
            ig.setColour (safeCol.withAlpha (haloA)); ig.fillRectList (safeHalo);
            ig.setColour (antiCol.withAlpha (haloA)); ig.fillRectList (antiHalo);
            ig.setColour (safeCol.withAlpha (coreA)); ig.fillRectList (safeCore);
            ig.setColour (antiCol.withAlpha (coreA)); ig.fillRectList (antiCore);
            ig.restoreState();
        });
    }

    // =========================================================================
    // CORRELATION – scrolling phase-correlation + balance history
    // =========================================================================
    void paintCorrelation (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const float cy = area.getCentreY();

        g.setColour (juce::Colours::grey.withAlpha (0.25f));
        g.drawHorizontalLine ((int) area.getY(),      area.getX(), area.getRight());
        g.drawHorizontalLine ((int) cy,               area.getX(), area.getRight());
        g.drawHorizontalLine ((int) area.getBottom(), area.getX(), area.getRight());
        g.setColour (juce::Colours::grey.withAlpha (0.7f));
        g.setFont (10.0f);
        g.drawText ("+1 in phase", (int) area.getX() + 4, (int) area.getY() + 1,       90, 12, juce::Justification::centredLeft);
        g.drawText ("0",           (int) area.getX() + 4, (int) cy - 12,               30, 12, juce::Justification::centredLeft);
        g.drawText ("-1 anti",     (int) area.getX() + 4, (int) area.getBottom() - 13, 90, 12, juce::Justification::centredLeft);

        const int W = juce::jmax (8, (int) area.getWidth());
        if ((int) corrHist.size() != W) { corrHist.assign ((size_t) W, 0.0f); balHist.assign ((size_t) W, 0.0f); }

        const juce::Colour rCol = activeColour().withRotatedHue (0.45f);

        juce::Path corr, bal;
        for (int x = 0; x < W; ++x)
        {
            const float c = corrHist[(size_t) x];
            const float y = cy - c * (area.getHeight() * 0.5f);
            const float px = area.getX() + (float) x;
            if (x == 0) corr.startNewSubPath (px, y); else corr.lineTo (px, y);

            const float b = balHist[(size_t) x];
            const float by = cy - b * (area.getHeight() * 0.5f);
            if (x == 0) bal.startNewSubPath (px, by); else bal.lineTo (px, by);
        }

        g.setColour (rCol.withAlpha (0.55f));
        g.strokePath (bal, juce::PathStrokeType (1.0f));
        g.setColour (activeColour().withAlpha (0.95f));
        g.strokePath (corr, juce::PathStrokeType (1.8f));

        g.setColour (activeColour());
        g.drawText ("corr " + juce::String (corrHist.empty() ? 0.0f : corrHist.back(), 2),
                    (int) area.getRight() - 90, (int) area.getY() + 1, 86, 12,
                    juce::Justification::centredRight);
    }

    // =========================================================================
    // SPECTRAL CORRELOMETER – per-band L/R correlation as an analog LED wall.
    // 1 (top) = fully mono / in phase, 0 (bottom) = fully stereo. Cyberpunk
    // look: LED cell matrix, subtle analog gradient, glowing peak caps.
    // =========================================================================

    // Complex L/R FFTs → time-averaged per-bin coherence:
    //   corr_k = <Re(L·R*)> / sqrt(<|L|²>·<|R|²>)   (SMOOTH sets the averaging)
    // A single frame would always read |cos Δφ| ≈ 1; the running average is what
    // separates "same signal" (→1) from "independent channels" (→0).
    int computeBinCorrelation()
    {
        std::vector<float> wL, wR;
        const int n = audioSource.getLastWaveform (wL, wR);
        if (n < 512) return 0;

        // requested bins → FFT size (2×bins), capped by the available samples
        const int wantBins = useCtrlBins ? ctrlBins : 512;
        int order = 10;                                   // 1024 samples → 512 bins
        while ((1 << order) < wantBins * 2 && order < 14) ++order;
        while ((1 << order) > n && order > 9) --order;
        const int size = 1 << order;

        if (fft == nullptr || fftOrder != order)
        {
            fft = std::make_unique<juce::dsp::FFT> (order);
            fftOrder = order;
            hann.resize ((size_t) size);
            for (int i = 0; i < size; ++i)
                hann[(size_t) i] = 0.5f * (1.0f - std::cos (
                    2.0f * juce::MathConstants<float>::pi * (float) i / (float) (size - 1)));
        }

        corrWorkL.assign (2 * (size_t) size, 0.0f);
        corrWorkR.assign (2 * (size_t) size, 0.0f);
        const int offset = n - size;
        for (int i = 0; i < size; ++i)
        {
            corrWorkL[(size_t) i] = wL[(size_t) (offset + i)] * hann[(size_t) i];
            corrWorkR[(size_t) i] = wR[(size_t) (offset + i)] * hann[(size_t) i];
        }
        fft->performRealOnlyForwardTransform (corrWorkL.data());
        fft->performRealOnlyForwardTransform (corrWorkR.data());

        const int nb = size / 2;
        if ((int) corrNum.size() != nb)
        {
            // RESAMPLE the running state instead of zeroing it: a reset made the
            // whole wall (or random columns) black out for a moment whenever the
            // FFT size changed (bins toggle, fluctuating waveform length).
            auto resample = [nb] (std::vector<float>& v)
            {
                if (v.empty()) { v.assign ((size_t) nb, 0.0f); return; }
                const int old = (int) v.size();
                std::vector<float> nv ((size_t) nb);
                for (int k = 0; k < nb; ++k)
                    nv[(size_t) k] = v[(size_t) juce::jlimit (0, old - 1,
                                        (int) std::round ((double) k * old / nb))];
                v.swap (nv);
            };
            resample (corrNum); resample (corrDenL); resample (corrDenR);
            resample (binCorr); resample (binMag);
        }

        const float a   = smoothAlpha;
        const float nrm = 4.0f / (float) size;
        for (int k = 0; k < nb; ++k)
        {
            const float lr = corrWorkL[(size_t) (2 * k)], li = corrWorkL[(size_t) (2 * k + 1)];
            const float rr = corrWorkR[(size_t) (2 * k)], ri = corrWorkR[(size_t) (2 * k + 1)];

            const float num = lr * rr + li * ri;          // Re(L · conj R)
            const float pL  = lr * lr + li * li;          // |L|²
            const float pR  = rr * rr + ri * ri;          // |R|²

            corrNum [(size_t) k] += a * (num - corrNum [(size_t) k]);
            corrDenL[(size_t) k] += a * (pL  - corrDenL[(size_t) k]);
            corrDenR[(size_t) k] += a * (pR  - corrDenR[(size_t) k]);

            const float den = std::sqrt (corrDenL[(size_t) k] * corrDenR[(size_t) k]);
            binCorr[(size_t) k] = (den > 1.0e-12f) ? corrNum[(size_t) k] / den : 0.0f;

            // audibility gate: normalised dB of the louder channel. FAST ATTACK,
            // SLOW RELEASE — with symmetric smoothing, pulsing bass dipped below
            // the gate between beats and whole chunks of low-frequency columns
            // flickered dark, jumping around with the note being played.
            float dB = 20.0f * std::log10 (juce::jmax (std::sqrt (juce::jmax (pL, pR)) * nrm, 1.0e-12f));
            dB = juce::jlimit (kDbFloor, 0.0f, dB);
            const float mNorm = (dB - kDbFloor) / (0.0f - kDbFloor);
            const float aM = (mNorm > binMag[(size_t) k]) ? juce::jmax (a, 0.5f)   // attack
                                                          : a * 0.22f;             // release
            binMag[(size_t) k] += aM * (mNorm - binMag[(size_t) k]);
        }
        return nb;
    }

    void paintCorrelometer (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const int nb = computeBinCorrelation();
        animPhase = std::fmod (animPhase + 1.0f, 86400.0f);

        auto plot = area;
        plot.removeFromBottom (14.0f);                    // Hz labels
        plot.removeFromLeft   (32.0f);                    // scale labels
        plot.removeFromTop    (4.0f);

        const juce::Colour base = activeColour().withMultipliedSaturation (1.35f);
        const juce::Font mono (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                  9.0f, juce::Font::plain));
        g.setFont (mono);

        // ── backdrop: THEME background (bgDeep→bgVoid) with a faint colour tint,
        //    so switching themes restyles the wall; the grid keeps the set colour ──
        // A SECOND background, on top of the module's own. paintModuleBackground
        // knows to step aside on transparency; this tinted backdrop is drawn after
        // it and would put the theme back regardless, as an opaque rectangle in a
        // Fusion and over the footage an alpha export is meant to sit on.
        if (! isTransparentBackground())
        {
            juce::ColourGradient bg (AlterTheme::bgDeep.interpolatedWith (base, 0.06f),
                                     plot.getX(), plot.getY(),
                                     AlterTheme::bgVoid.interpolatedWith (base, 0.10f),
                                     plot.getX(), plot.getBottom(), false);
            g.setGradientFill (bg);
            g.fillRect (plot.expanded (2.0f));
        }

        // ── scale grid (0 = stereo … 1 = mono), dashed ──
        const float dashes[] = { 3.0f, 4.0f };
        for (float v : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            const float y = plot.getBottom() - v * plot.getHeight();
            g.setColour (base.withAlpha (v == 0.0f || v == 1.0f ? 0.35f : 0.13f));
            g.drawDashedLine (juce::Line<float> (plot.getX(), y, plot.getRight(), y),
                              dashes, 2, 1.0f);
            g.setColour (base.withAlpha (0.75f));
            g.drawText (juce::String (v, 2), (int) area.getX(), (int) y - 5, 28, 10,
                        juce::Justification::centredRight);
        }
        g.setColour (base.withAlpha (0.85f));
        g.drawText ("MONO",   (int) plot.getX() + 4, (int) plot.getY() + 3,       48, 10, juce::Justification::centredLeft);
        g.drawText ("STEREO", (int) plot.getX() + 4, (int) plot.getBottom() - 13, 48, 10, juce::Justification::centredLeft);

        // ── log-frequency axis + checkpoints, dashed ──
        const double fmin = 20.0, fmax = 20000.0;
        const double logSpan = std::log10 (fmax / fmin);
        auto xForFreq = [&] (double f) -> float
        {
            const double u = std::log10 (juce::jlimit (fmin, fmax, f) / fmin) / logSpan;
            return plot.getX() + (float) u * plot.getWidth();
        };

        const double checkHz[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
        for (double hz : checkHz)
        {
            const float x = xForFreq (hz);
            g.setColour (base.withAlpha (0.10f));
            g.drawDashedLine (juce::Line<float> (x, plot.getY(), x, plot.getBottom()),
                              dashes, 2, 1.0f);
            g.setColour (base.withAlpha (0.65f));
            const juce::String txt = (hz >= 1000.0) ? juce::String (hz / 1000.0, 0) + "k"
                                                    : juce::String ((int) hz);
            g.drawText (txt, (int) x - 14, (int) plot.getBottom() + 3, 28, 10,
                        juce::Justification::centred);
        }

        if (nb <= 0) return;

        // ── LED cell matrix (capped so huge panels stay cheap to fill) ──
        const int cols = juce::jlimit (8, 256, (int) (plot.getWidth()  / 5.0f));
        const int rows = juce::jlimit (8,  96, (int) (plot.getHeight() / 4.0f));
        const float cellW = plot.getWidth()  / (float) cols;
        const float cellH = plot.getHeight() / (float) rows;

        const double binHz = (assumedSampleRateHz * 0.5) / (double) nb;

        // per-column mono-ness: energy-weighted mean of the bins in the column
        // span, widened by ±1 bin — in the low end one column may fall between
        // bin centres and blank out (the jumping gaps).
        std::vector<int>  lit  ((size_t) cols, -1);        // -1 = silent column
        std::vector<char> anti ((size_t) cols, 0);         // net anti-phase band
        for (int c = 0; c < cols; ++c)
        {
            const double f0 = fmin * std::pow (fmax / fmin, (double)  c      / (double) cols);
            const double f1 = fmin * std::pow (fmax / fmin, (double) (c + 1) / (double) cols);
            int k0 = juce::jlimit (1, nb - 1, (int) std::floor (f0 / binHz) - 1);
            int k1 = juce::jlimit (1, nb - 1, (int) std::ceil  (f1 / binHz) + 1);
            if (k1 < k0) std::swap (k0, k1);

            float wsum = 0.0f, csum = 0.0f, peak = 0.0f;
            for (int k = k0; k <= k1; ++k)
            {
                const float m = binMag[(size_t) k];
                wsum += m;
                csum += m * binCorr[(size_t) k];
                peak  = juce::jmax (peak, m);
            }
            if (peak < 0.012f || wsum <= 1.0e-6f) continue;          // inaudible → dark

            const float corr = csum / wsum;                          // -1 .. +1
            const float mono = juce::jlimit (0.0f, 1.0f, corr);      // display clamps at "full stereo"
            lit [(size_t) c] = juce::jlimit (0, rows, (int) std::round (mono * (float) rows));
            anti[(size_t) c] = (corr < -0.05f) ? 1 : 0;
        }

        // peak-hold state (falling markers, classic analog meter behaviour)
        if ((int) colPeak.size() != cols) colPeak.assign ((size_t) cols, 0.0f);
        for (int c = 0; c < cols; ++c)
        {
            const float target = (float) juce::jmax (0, lit[(size_t) c]);
            if (target >= colPeak[(size_t) c]) colPeak[(size_t) c] = target;
            else colPeak[(size_t) c] = juce::jmax (target, colPeak[(size_t) c] - 0.30f);
        }

        // colours: subtle analog gradient built from the panel colour
        auto rowColour = [&] (float u)                     // u: 0 bottom … 1 top
        {
            return base.withRotatedHue (juce::jmap (u, -0.075f, 0.045f))
                       .brighter (0.10f * (1.0f - u));
        };

        // faint idle matrix (unlit cells) — one batched call
        {
            juce::RectangleList<float> idle;
            for (int c = 0; c < cols; ++c)
                for (int r = 0; r < rows; ++r)
                    idle.addWithoutMerging ({ plot.getX() + (float) c * cellW,
                                              plot.getBottom() - (float) (r + 1) * cellH,
                                              cellW - 1.0f, cellH - 1.0f });
            g.setColour (base.withAlpha (0.05f));
            g.fillRectList (idle);
        }

        // lit cells, batched per row; every 4th row slightly hotter (scan banding)
        for (int r = 0; r < rows; ++r)
        {
            juce::RectangleList<float> cells;
            for (int c = 0; c < cols; ++c)
                if (lit[(size_t) c] > r)
                    cells.addWithoutMerging ({ plot.getX() + (float) c * cellW,
                                               plot.getBottom() - (float) (r + 1) * cellH,
                                               cellW - 1.0f, cellH - 1.0f });
            if (cells.isEmpty()) continue;
            const float u = (float) r / (float) juce::jmax (1, rows - 1);
            g.setColour (rowColour (u).withAlpha ((r % 4 == 3) ? 0.98f : 0.85f));
            g.fillRectList (cells);
        }

        // neon peak caps + layered glow beams rising above them
        {
            juce::RectangleList<float> caps, glowNear, glowFar;
            for (int c = 0; c < cols; ++c)
            {
                const int t = lit[(size_t) c];
                if (t <= 0) continue;
                const float x = plot.getX() + (float) c * cellW;
                const float y = plot.getBottom() - (float) t * cellH;
                caps.addWithoutMerging     ({ x,        y,         cellW - 1.0f, cellH - 1.0f });
                glowNear.addWithoutMerging ({ x - 1.0f, y - 4.0f,  cellW + 1.0f, cellH + 6.0f });
                glowFar.addWithoutMerging  ({ x,        y - 10.0f, cellW - 1.0f, 10.0f });
            }
            g.setColour (base.brighter (0.4f).withAlpha (0.08f)); g.fillRectList (glowFar);
            g.setColour (base.brighter (0.5f).withAlpha (0.20f)); g.fillRectList (glowNear);
            g.setColour (base.brighter (0.65f).withAlpha (1.0f)); g.fillRectList (caps);
        }

        // anti-phase warning: the floor cell burns hot pink where corr < 0
        {
            juce::RectangleList<float> warn, warnHalo;
            for (int c = 0; c < cols; ++c)
            {
                if (! anti[(size_t) c]) continue;
                const float x = plot.getX() + (float) c * cellW;
                warn.addWithoutMerging     ({ x,        plot.getBottom() - cellH,        cellW - 1.0f, cellH - 1.0f });
                warnHalo.addWithoutMerging ({ x - 1.0f, plot.getBottom() - cellH - 3.0f, cellW + 1.0f, cellH + 4.0f });
            }
            g.setColour (AlterTheme::cerise.withAlpha (0.25f));               g.fillRectList (warnHalo);
            g.setColour (AlterTheme::cerise.brighter (0.2f).withAlpha (1.0f)); g.fillRectList (warn);
        }

        // falling peak-hold markers
        {
            juce::RectangleList<float> marks;
            for (int c = 0; c < cols; ++c)
                if (colPeak[(size_t) c] > 0.5f)
                    marks.addWithoutMerging ({ plot.getX() + (float) c * cellW,
                                               plot.getBottom() - colPeak[(size_t) c] * cellH - 1.0f,
                                               cellW - 1.0f, 2.0f });
            g.setColour (base.brighter (0.9f).withAlpha (0.9f));
            g.fillRectList (marks);
        }

        // slow scanline sweeping down the wall
        {
            const float sy = plot.getY() + std::fmod (animPhase * 0.6f, plot.getHeight());
            g.setColour (base.withAlpha (0.10f));
            g.fillRect (plot.getX(), sy - 3.0f, plot.getWidth(), 6.0f);
            g.setColour (base.brighter (0.4f).withAlpha (0.16f));
            g.fillRect (plot.getX(), sy, plot.getWidth(), 1.0f);
        }

        // neon frame + corner brackets
        {
            const auto f = plot.expanded (2.0f);
            g.setColour (base.withAlpha (0.30f));
            g.drawRect (f, 1.0f);

            const float bl = juce::jmin (14.0f, f.getWidth() * 0.1f);
            g.setColour (base.brighter (0.4f).withAlpha (0.9f));
            auto corner = [&] (float x, float y, float sx, float sy)
            {
                g.fillRect (juce::Rectangle<float> (juce::jmin (x, x + sx * bl), y - 1.0f, bl, 2.0f));
                g.fillRect (juce::Rectangle<float> (x - 1.0f, juce::jmin (y, y + sy * bl), 2.0f, bl));
            };
            corner (f.getX(),     f.getY(),      1.0f,  1.0f);
            corner (f.getRight(), f.getY(),     -1.0f,  1.0f);
            corner (f.getX(),     f.getBottom(), 1.0f, -1.0f);
            corner (f.getRight(), f.getBottom(),-1.0f, -1.0f);
        }

        // readout: resolution + wideband correlation (from the same accumulators)
        float wbNum = 0.0f; double wbL = 0.0, wbR = 0.0;
        for (int k = 1; k < nb; ++k)
        {
            wbNum += corrNum[(size_t) k];
            wbL   += corrDenL[(size_t) k];
            wbR   += corrDenR[(size_t) k];
        }
        const double wbDen = std::sqrt (wbL * wbR);
        const float wb = (wbDen > 1.0e-12) ? (float) (wbNum / wbDen) : 0.0f;

        const juce::String readout = "RES " + juce::String (nb).paddedLeft ('0', 4)
                                   + " / CORR " + (wb >= 0 ? "+" : "") + juce::String (wb, 2);
        const int rw = 130;
        const juce::Rectangle<int> rbox ((int) plot.getRight() - rw - 6,
                                         (int) plot.getBottom() - 16, rw, 13);
        g.setColour (AlterTheme::bgPanel.withAlpha (0.85f));    // theme box background
        g.fillRect (rbox);
        g.setColour ((wb < 0.0f ? AlterTheme::cerise : base).withAlpha (0.9f));
        g.drawRect (rbox, 1);
        g.setFont (mono);
        g.drawText (readout, rbox.reduced (4, 0), juce::Justification::centredRight);
    }

    // =========================================================================
    // Advance the scrolling correlation/balance history one step. Runs on the
    // worker thread (called from renderImage), so corrHist/balHist are local.
    void updateCorrelationHistory()
    {
        std::vector<float> wL, wR;
        const int n = audioSource.getLastWaveform (wL, wR);
        if (n >= 2)
        {
            double sLL = 0, sRR = 0, sLR = 0;
            for (int i = 0; i < n; ++i)
            {
                const double l = wL[(size_t) i], r = wR[(size_t) i];
                sLL += l * l; sRR += r * r; sLR += l * r;
            }
            const double denom = std::sqrt (sLL * sRR);
            const float corr = (denom > 1.0e-9) ? (float) (sLR / denom) : 0.0f;
            const double total = sLL + sRR;
            const float bal = (total > 1.0e-9) ? (float) ((sRR - sLL) / total) : 0.0f;

            if (! corrHist.empty())
            {
                std::rotate (corrHist.begin(), corrHist.begin() + 1, corrHist.end());
                std::rotate (balHist.begin(),  balHist.begin() + 1,  balHist.end());
                const float a = smoothAlpha;
                corrHist.back() = corrHist[corrHist.size() - 2] + a * (corr - corrHist[corrHist.size() - 2]);
                balHist.back()  = balHist[balHist.size() - 2]   + a * (bal  - balHist[balHist.size() - 2]);
            }
        }
    }

    // ── ONE analysis pass: L, R, MID and SIDE spectra together ───────────────
    //
    // These four are not four measurements, they are one. A complex forward
    // transform of L and of R already contains |L| and |R|, and MID / SIDE are
    // the sum and difference of those SAME complex bins — so the pair of
    // functions this replaces ran four transforms, and fetched the waveform
    // twice, to produce what two transforms and one fetch already hold.
    //
    // Every buffer it works in is a member, so a steady-state frame allocates
    // nothing. That matters more than the arithmetic saved: the old version
    // heap-allocated four scratch vectors per frame (~128 KB at order 12), and a
    // worker whose frame time jumps whenever the allocator does has an UNEVEN
    // frame time — which is what the eye reads as stutter, whatever the fps says.
    //
    // Results land in anaL / anaR / anaM / anaS. Returns the bin count; 0 means
    // there was not enough audio and the caller must not read the vectors.
    int computeStereoSpectra()
    {
        const int n = audioSource.getLastWaveform (anaWavL, anaWavR);
        if (n < 512) return 0;

        int order = 9;
        while ((1 << (order + 1)) <= n && order < 12) ++order;
        const int size = 1 << order;

        if (fft == nullptr || fftOrder != order)
        {
            fft = std::make_unique<juce::dsp::FFT> (order);
            fftOrder = order;
            hann.resize ((size_t) size);
            for (int i = 0; i < size; ++i)
                hann[(size_t) i] = 0.5f * (1.0f - std::cos (
                    2.0f * juce::MathConstants<float>::pi * (float) i / (float) (size - 1)));
        }

        // The transform wants 2 * size floats and reads its input from the first
        // `size` of them, so the tail must be CLEARED and not left holding the
        // previous frame. assign() on an already-correctly-sized vector does not
        // reallocate, it just overwrites — which is the whole point here.
        anaCL.assign ((size_t) (2 * size), 0.0f);
        anaCR.assign ((size_t) (2 * size), 0.0f);

        const int offset = n - size;
        for (int i = 0; i < size; ++i)
        {
            anaCL[(size_t) i] = anaWavL[(size_t) (offset + i)] * hann[(size_t) i];
            anaCR[(size_t) i] = anaWavR[(size_t) (offset + i)] * hann[(size_t) i];
        }
        fft->performRealOnlyForwardTransform (anaCL.data());
        fft->performRealOnlyForwardTransform (anaCR.data());

        const int   nb  = size / 2;
        const float nrm = 4.0f / (float) size;
        anaL.resize ((size_t) nb); anaR.resize ((size_t) nb);
        anaM.resize ((size_t) nb); anaS.resize ((size_t) nb);

        auto toNorm = [] (float m) -> float
        {
            float dB = 20.0f * std::log10 (juce::jmax (m, 1.0e-12f));
            dB = juce::jlimit (kDbFloor, 0.0f, dB);
            return (dB - kDbFloor) / (0.0f - kDbFloor);
        };

        for (int k = 0; k < nb; ++k)
        {
            const float lr = anaCL[(size_t) (2 * k)], li = anaCL[(size_t) (2 * k + 1)];
            const float rr = anaCR[(size_t) (2 * k)], ri = anaCR[(size_t) (2 * k + 1)];

            // |L| and |R| — identical to what performFrequencyOnlyForwardTransform
            // used to hand back, since that is precisely this magnitude.
            anaL[(size_t) k] = toNorm (std::sqrt (lr * lr + li * li) * nrm);
            anaR[(size_t) k] = toNorm (std::sqrt (rr * rr + ri * ri) * nrm);

            const float mR = lr + rr, mI = li + ri;       // L + R
            const float sR = lr - rr, sI = li - ri;       // L - R
            anaM[(size_t) k] = toNorm (std::sqrt (mR * mR + mI * mI) * 0.5f * nrm);
            anaS[(size_t) k] = toNorm (std::sqrt (sR * sR + sI * sI) * 0.5f * nrm);
        }
        return nb;
    }

    IAudioSource& audioSource;
    juce::Colour  lineColour { AlterTheme::pictonBlue };

    // Colour by tone. Advanced once per frame at the top of renderImage (worker
    // thread); every mode paint below only reads it via activeColour().
    bool  colourByTone { false };
    bool  toneTwist    { false };
    float toneSmooth   { 0.5f };    // the SLIDER value; the rate curve lives in PitchUtils
    PitchUtils::ToneHueTracker toneHue;

    /** The colour every mode's palette is built from: the note in tone mode, the
        user's pick otherwise. */
    juce::Colour activeColour() const noexcept
    {
        return (colourByTone && toneHue.hasTone()) ? toneHue.colour() : lineColour;
    }

    // ── Thickness mapping ────────────────────────────────────────────────────
    //  Both sliders are NEUTRAL AT 0.5, mapping to exactly the constants the
    //  module used before they existed. That is the whole reason for the two-leg
    //  jmap instead of one straight line: an existing patch that has never been
    //  told about these must come back looking identical, and it does — 0.5 is
    //  1.6 px of beam and a 0.9 px dot core, which is what was hard-coded here.
    //  Below 0.5 the traces get finer (the request), above it they get heavier.
    static float legMap (float v01, float lo, float mid, float hi) noexcept
    {
        return (v01 <= 0.5f) ? juce::jmap (v01, 0.0f, 0.5f, lo,  mid)
                             : juce::jmap (v01, 0.5f, 1.0f, mid, hi);
    }

    /** Goniometer beam stroke, in pixels. */
    float beamStrokePx() const noexcept { return legMap (lineWidth, 0.35f, 1.6f, 4.0f); }

    /** Dot CORE half-extent, in pixels (the old value was 0.9). */
    float dotCorePx() const noexcept { return legMap (pointSize, 0.25f, 0.9f, 2.6f); }

    /** Dot HALO half-extent. Tied to the core by the ratio the scopes already
        used (2.1 / 0.9), so thinning the points thins the glow with them instead
        of leaving a fat halo around a hairline dot. */
    float dotHaloPx() const noexcept { return dotCorePx() * (2.1f / 0.9f); }

    /** Particles mode dot radius. Its own leg map because that mode's dots were
        never the scopes' size — they were r = 1.7, and 0.5 has to still be 1.7. */
    float particleRadiusPx() const noexcept { return legMap (pointSize, 0.45f, 1.7f, 4.5f); }
    int   mode        { 0 };
    bool  gonioParticles { false };      // goniometer: particle cloud vs line
    float density     { 0.5f };          // particle count (Particles + Goniometer cloud)
    float brightness  { 0.5f };          // gonio/polar trace brightness (0.5 = neutral)
    float lineWidth   { 0.5f };          // goniometer beam thickness (0.5 = the old 1.6 px)
    float pointSize   { 0.5f };          // dot radius for every particle trace (0.5 = old size)
    float drive       { 0.0f };          // smoothed audio level → overall intensity
    float smoothAlpha { 0.35f };
    float persistence { 0.7f };          // motion-blur trail amount
    double assumedSampleRateHz { 48000.0 };
    static constexpr float kDbFloor = -90.0f;

    juce::Random rng;                    // (kept: used by nothing hot any more)

    std::vector<float> smL, smR;         // particles smoothing (per-channel)
    std::vector<float> smM, smS;         // particles smoothing (mid/side → stereo width)
    std::vector<float> corrHist, balHist;

    // Particles mode: one batched fill per alpha bucket (see paintParticles).
    // Members, not locals, so the rectangle storage is reused instead of being
    // grown from empty every frame. The count and the array are declared together
    // so an edit to one cannot silently disagree with the other.
    static constexpr int kNumAlphaBuckets = 6;
    std::array<juce::RectangleList<float>, (size_t) kNumAlphaBuckets> particleBuckets;

    // Time base for the particle drift, relative to the module's first frame
    // (see paintParticles for why an absolute reading will not do).
    double particleT0 { 0.0 };

    // Unified analysis output + its scratch. Persistent for the same reason.
    std::vector<float> anaL, anaR, anaM, anaS;
    std::vector<float> anaWavL, anaWavR, anaCL, anaCR;

    // correlometer state (worker thread only)
    bool useCtrlBins { false };          // follow the controller's Max-bins setting
    int  ctrlBins    { 512 };            // the controller's global Max-bins value
    std::vector<float> corrNum, corrDenL, corrDenR;   // running coherence accumulators
    std::vector<float> binCorr, binMag;               // per-bin correlation + level gate
    std::vector<float> corrWorkL, corrWorkR;          // complex FFT scratch
    std::vector<float> colPeak;                       // falling peak-hold per column
    float animPhase { 0.0f };                         // scanline sweep phase

    // scope persistence (ping-pong images)
    juce::Image scopeImgA, scopeImgB;
    bool scopeUseA { true };
    int  scopeW { 0 }, scopeH { 0 };

    std::unique_ptr<juce::dsp::FFT> fft;
    int fftOrder = 0;
    std::vector<float> work, hann;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualStereoscope)
};
