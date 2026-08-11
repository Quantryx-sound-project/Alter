#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <climits>
#include <vector>
#include <atomic>
#include "AudioSourceInterface.h"
#include "AlterTheme.h"
#include "AsyncVisualBase.h"
#include "ConstantQ.h"
#include "PitchUtils.h"

// Real-time spectrum renderer:
// - EMA smoothing (attack/release, štandardizované)
// - log-X resampling, linear interp (measurement) alebo Catmull-Rom (visual)
// - Psychoacoustic curve: Flat / A-weight / ISO 226 (Fletcher-Munson)
// - Measurement mode: power-average downsample, linear interp
// - Harmonics overlay: len zmena farby, NIE hodnôt krivky

class VisualSpectrum : public AsyncVisualBase
{
public:
    explicit VisualSpectrum (IAudioSource& r)
        : AsyncVisualBase ("AlterSpectrum", 60), audioSource (r)   // 60 fps → smoother motion (esp. fullscreen)
    {
        startAsyncRender();         // heavy rendering runs on a worker thread
    }

    // Stop the worker BEFORE our members are destroyed (it calls renderImage()).
    ~VisualSpectrum() override { stopAsyncRender(); }

    // --- Settery ---
    void setDisplayBins (int b)
    {
        // Accept any power of two from 512..8192 (follows the global Max-bins).
        if (b < 512 || b > 8192 || (b & (b - 1)) != 0) b = 2048;
        displayBins = b; repaint();
    }

    /** Constant-Q display: aggregate FFT bins per log-frequency band (no missed
        peaks; clean musical bands) instead of sampling the curve. */
    void setConstantQ (bool b) { constantQ = b; repaint(); }

    // smooth 0..1
    void setSmoothAmount (float s)
    {
        s = juce::jlimit (0.0f, 1.0f, s);
        // Štandardizovaná EMA: s=0 → okamžitá odozva, s=1 → max vyhladzovanie
        // attack je vždy rýchlejší ako release (SPAN-like)
        const float a = juce::jmap (s, 0.0f, 1.0f, 0.0f, 0.92f);
        inputAlpha   = 1.0f - juce::jlimit (0.02f, 0.85f, a);          // EMA koeficient vstupu
        attackAlpha  = juce::jlimit (0.15f, 0.95f, 1.0f - a * 0.6f);   // rýchle stúpanie
        releaseAlpha = juce::jlimit (0.02f, 0.35f, 1.0f - a * 0.95f);  // pomalé klesanie
    }

    void setAssumedDbFloor    (float dB)  { assumedDbFloor = dB; }
    void setAssumedSampleRate (double sr) { assumedSampleRateHz = (sr > 0.0 ? sr : 48000.0); needRebuildTables = true; }
    void setLineColour        (juce::Colour c) { lineColour = c; repaint(); }

    // ── Colour by tone ────────────────────────────────────────────────────────
    //  Everything this module draws — curve, bars, harmonic marks, background
    //  wash, peak-hold line, the stereo R hue offset and the L/R legend — is
    //  derived from ONE colour. So tone colour needs no per-mode special casing:
    //  it swaps that one colour for the dominant note's, and Stereo, Peak-hold,
    //  Constant-Q, Measurement and the reference curve all follow automatically.
    void setColourByTone (bool b)
    {
        if (colourByTone == b) return;
        colourByTone = b;
        // Coming back from manual, the held hue is however old the last tone-mode
        // frame was. Snap to the note that is actually sounding instead of sliding
        // across the wheel from it.
        if (b) toneHue.reset();
        repaint();
    }

    /** 'Mirror tone color' — reverses the tone→hue wheel (see PitchUtils). */
    void setToneTwist (bool b) { toneTwist = b; repaint(); }

    /** 'Tone smooth' 0..1: HIGH = heavier smoothing (slow colour reaction), LOW =
        fast. Stored RAW — PitchUtils::toneSmoothToRate turns it into a chase rate,
        so the curve is defined once instead of once per module. */
    void setToneSmooth (float s01) { toneSmooth = juce::jlimit (0.0f, 1.0f, s01); }

    /** MIRROR the frequency axis: high frequencies on the left, low on the right.

        A flip of the MAPPING, not of the finished picture. Turning the rendered
        image over would turn the Hz labels over with it, and a spectrum whose
        numbers read backwards is worse than one that runs the wrong way. So the
        single place that turns a position along the log-frequency scale into an x
        is xForU below, and EVERYTHING goes through it — curve, bars, peak hold,
        stereo R, the reference curve, the grid lines and their labels. */
    void setMirrorFreq (bool m)
    {
        if (mirrorFreq == m) return;
        mirrorFreq = m;
        // The cache is NOT dropped here: it belongs to the worker thread, which
        // may be blitting it this instant. renderImage compares bgCacheMirror
        // against this flag and rebuilds it there, where it is safe to.
        repaint();
    }

    /** x for a position along the log-frequency axis: u = 0 is the low end of the
        range, u = 1 the high end, whichever side of the module those land on. */
    float xForU (const juce::Rectangle<int>& plot, double u) const noexcept
    {
        return (float) plot.getX()
             + (float) ((mirrorFreq ? 1.0 - u : u) * (double) plot.getWidth());
    }

    // Psychoacoustic curve: 0=Flat, 1=A-weight, 2=ISO226
    void setPsychoacousticMode (int mode, int phonLevel = 60)
    {
        psychoMode  = juce::jlimit (0, 2, mode);
        phonLevelDb = juce::jlimit (20, 100, phonLevel);
        needRebuildTables = true;
        repaint();
    }

    // Measurement mode: true = SPAN-like (linear interp, power-avg downsample)
    void setMeasurementMode (bool on) { measurementMode = on; repaint(); }

    // Peak hold: overlay line that holds per-frequency maxima and slowly decays
    void setPeakHold (bool on)
    {
        if (peakHoldEnabled == on) return;
        peakHoldEnabled = on;
        resetPeak.store (true);   // worker clears the held peak line safely (not from this thread)
        repaint();
    }

    // Reference curve: 0=Off, 1=EDM, 2=Bass music, 3=House/Techno, 4=Hip-Hop, 5=Pop, 6=Rock
    void setReferenceGenre (int genre) { referenceGenre = juce::jlimit (0, 6, genre); repaint(); }

    // R-channel colour in stereo mode: 0 = complementary, 1 = analogous
    void setStereoColourMode (int mode) { stereoColourMode = juce::jlimit (0, 1, mode); repaint(); }

    // Stereo mode: two overlaid spectra (L + R), computed from the waveform stream
    void setStereoMode (bool on)
    {
        if (stereoMode == on) return;
        stereoMode = on;
        resetPipeline.store (true);   // worker forces pipeline realloc safely
        repaint();
    }

    // Getters
    int  getDisplayBins()          const { return displayBins; }
    bool isUsingAWeight()          const { return psychoMode == 1; }
    float getFftPacketsPerSecond() const { return (float) audioSource.getFftPacketsPerSecond(); }

    /** Peak readout for the host overlay label ("427 Hz  -21.8 dBFS"). */
    juce::String getPeakText() const
    {
        if (lastPeakDb <= assumedDbFloor + 0.5f) return {};
        return (lastPeakHz >= 1000.0 ? juce::String (lastPeakHz / 1000.0, 2) + " kHz"
                                     : juce::String ((int) lastPeakHz) + " Hz")
               + "  " + juce::String (lastPeakDb, 1) + " dBFS";
    }

    // ── Cursor readout (the host draws it — the view gets no mouse events) ────
    // Returns freq (from x) + dB (from y) at a point in the view's local coords.
    juce::String cursorText (juce::Point<float> p) const
    {
        const float w = (float) getWidth(), h = (float) getHeight();
        if (w < 20.0f || h < 20.0f || p.x < 0.0f || p.x > w || p.y < 0.0f || p.y > h) return {};
        const double fmin = 16.0, fmax = 20000.0;
        const double logSpan = std::log10 (fmax / fmin);
        const float  u  = juce::jlimit (0.0f, 1.0f, p.x / w);
        const double hz = fmin * std::pow (10.0, (double) u * logSpan);
        const float  nrm = juce::jlimit (0.0f, 1.0f, (h - p.y) / h);
        const float  db  = assumedDbFloor * (1.0f - nrm);
        const juce::String hzTxt = (hz >= 1000.0) ? juce::String (hz / 1000.0, 2) + " kHz"
                                                   : juce::String ((int) std::round (hz)) + " Hz";
        return hzTxt + "  " + juce::String (db, 1) + " dB";
    }

    // =====================================================================
    // Runs on the WORKER thread (AsyncVisualBase). Draws the whole module into g.
    void renderImage (juce::Graphics& g, int w, int h) override
    {
        // honour pending resets requested from the message thread (safe here)
        if (resetPipeline.exchange (false)) { prevInput.clear(); prevInputR.clear(); }
        if (resetPeak.exchange (false))    { peakLineHold.clear(); }

        // ONE tone update per frame, and only while tone colour is on — the
        // tracker's FFT copy + peak scan is cheap but not free, and nothing below
        // may call it again per bar or per point.
        if (colourByTone)
            toneHue.update (audioSource, toneTwist, toneSmooth);
        const juce::Colour lineCol = activeLineColour();

        auto bounds = juce::Rectangle<int> (0, 0, w, h);

        // STATIC layer from cache: the background gradient, border, dB/Hz grid
        // lines and ~20 text labels cost several ms per frame at large sizes but
        // only change on resize / theme / floor change — one 1:1 blit instead.
        // The transparent flag is part of the signature: joining or leaving an
        // Fusion changes what this cache should contain, and without it the module
        // would keep blitting the background it baked in before it became a layer.
        if (! bgCache.isValid() || bgCache.getWidth() != w || bgCache.getHeight() != h
            || bgCacheTheme != AlterTheme::themeGeneration
            || bgCacheClear != isTransparentBackground()
            || bgCacheMirror != mirrorFreq
            || bgCacheInfoHidden != AlterTheme::hudInfoHidden.load()
            || std::abs (bgCacheFloor - assumedDbFloor) > 0.01f)
            rebuildBgCache (w, h);
        g.drawImageAt (bgCache, 0, 0);

        // --- Responzívna veľkosť fontu ---
        const int shortSide = juce::jmin (bounds.getWidth(), bounds.getHeight());
        const float fSzInfo = juce::jlimit (7.0f, 11.0f, (float) shortSide * 0.058f);

        // no padding: the plot fills the module exactly (aligns with Spectrogram)
        auto plot = bounds;
        g.saveState();
        g.reduceClipRegion (plot);

        // Constant-Q: when active and data is present, the per-pixel curve is sampled
        // from the multi-resolution CQT stream instead of the linear FFT — but it then
        // flows through the SAME styling (bars/harmonics/glow) as the numeric bin modes.
        // STEREO + CQ: the source computes TRUE per-channel (L/R) constant-Q streams
        // on demand — full multi-resolution bass detail in both channels. If the
        // source cannot provide them (e.g. plugin/UDP instances), the view falls back
        // to per-channel FFTs aggregated per log band.
        // per-frame scratch buffers live as members: their capacity is reused, so
        // the worker does ZERO steady-state heap allocation per frame (repeated
        // alloc/free of up-to-8192-float vectors at 60 fps caused visible jitter
        // that scaled with the bin count).
        std::vector<float>& cqRaw  = cqRawBuf;
        std::vector<float>& cqRawR = cqRawRBuf;
        bool cqStereo = false;
        bool useCqt   = false;
        if (constantQ && stereoMode)
        {
            cqStereo = audioSource.getLastCqtStereo (cqRaw, cqRawR) >= 16
                       && cqRawR.size() == cqRaw.size();
            useCqt = cqStereo;
        }
        else
            useCqt = constantQ && audioSource.getLastCqt (cqRaw) >= 16;

        if (useCqt)
        {
            if (cqtDisp.size() != cqRaw.size()) cqtDisp = cqRaw;
            else for (size_t i = 0; i < cqRaw.size(); ++i) cqtDisp[i] += 0.5f * (cqRaw[i] - cqtDisp[i]);
            if (cqStereo)
            {
                if (cqtDispR.size() != cqRawR.size()) cqtDispR = cqRawR;
                else for (size_t i = 0; i < cqRawR.size(); ++i) cqtDispR[i] += 0.5f * (cqRawR[i] - cqtDispR[i]);
            }
        }

        std::vector<float>& in  = inBuf;
        std::vector<float>& inR = inRBuf;   // right channel (stereo mode only)
        int bins;

        if (stereoMode)
        {
            // compute L/R spectra locally from the stereo waveform stream
            bins = computeStereoFfts (in, inR);
        }
        else
        {
            bins = audioSource.getLastFft (in);
        }

        if (bins <= 0)
        {
            g.restoreState();
            g.setFont (fSzInfo);
            g.setColour (juce::Colours::white.withAlpha (0.6f));
            g.drawText ("waiting for audio...", plot, juce::Justification::centred, false);
            return;
        }

        // --- realloc bufferov ---
        if ((int) prevInput.size() != bins)
        {
            prevInput.assign  ((size_t) bins, 0.0f);
            disp.assign       ((size_t) bins, 0.0f);
            prevInputR.assign ((size_t) bins, 0.0f);
            dispR.assign      ((size_t) bins, 0.0f);
            aWeightDb.assign  ((size_t) bins, 0.0f);
            iso226Db.assign   ((size_t) bins, 0.0f);
            needRebuildTables = true;
        }

        if (needRebuildTables)
        {
            buildAWeightTable  (bins, assumedSampleRateHz);
            buildISO226Table   (bins, assumedSampleRateHz, phonLevelDb);
            needRebuildTables  = false;
        }

        // --- Psychoacoustic curve v dB doméne ---
        auto applyPsycho = [&] (std::vector<float>& buf)
        {
            if (psychoMode == 0) return;
            for (int i = 0; i < bins; ++i)
            {
                double dB = (double) assumedDbFloor + (double) buf[(size_t) i] * (0.0 - (double) assumedDbFloor);
                dB += (psychoMode == 1) ? (double) aWeightDb[(size_t) i]
                                        : (double) iso226Db[(size_t) i];
                const double n = (dB - (double) assumedDbFloor) / (0.0 - (double) assumedDbFloor);
                buf[(size_t) i] = (float) juce::jlimit (0.0, 1.0, n);
            }
        };

        // --- Vstupná EMA + výstupná EMA (attack/release) ---
        auto runEma = [&] (const std::vector<float>& src,
                           std::vector<float>& prevB, std::vector<float>& dispB)
        {
            for (int i = 0; i < bins; ++i)
            {
                const float p = prevB[(size_t) i];
                prevB[(size_t) i] = p + inputAlpha * (src[(size_t) i] - p);

                const float x = juce::jmin (prevB[(size_t) i], 1.0f);
                const float d = dispB[(size_t) i];
                dispB[(size_t) i] = (x > d) ? d + attackAlpha  * (x - d)
                                            : d + releaseAlpha * (x - d);
            }
        };

        applyPsycho (in);
        runEma (in, prevInput, disp);

        if (stereoMode && (int) inR.size() == bins)
        {
            applyPsycho (inR);
            runEma (inR, prevInputR, dispR);
        }

        // --- Downsample binov ---
        int drawBins = bins;
        std::vector<float>& dispDraw = dispDrawBuf;

        if (displayBins > 0 && displayBins < bins && (bins % displayBins) == 0)
        {
            drawBins = displayBins;
            const int group = bins / drawBins;
            dispDraw.assign ((size_t) drawBins, 0.0f);

            if (measurementMode)
            {
                // Measurement: power-average (vernejší energii)
                const double floor_lin = std::pow (10.0, (double) assumedDbFloor / 10.0);
                for (int i = 0; i < drawBins; ++i)
                {
                    double sumP = 0.0;
                    const int start = i * group;
                    for (int k = 0; k < group; ++k)
                    {
                        // 0..1 → dB → power
                        const double v  = (double) disp[(size_t) (start + k)];
                        const double dB = (double) assumedDbFloor + v * (0.0 - (double) assumedDbFloor);
                        sumP += std::pow (10.0, dB / 10.0);
                    }
                    const double avgP  = sumP / (double) group;
                    const double avgDb = 10.0 * std::log10 (juce::jmax (avgP, floor_lin));
                    const double n = (avgDb - (double) assumedDbFloor) / (0.0 - (double) assumedDbFloor);
                    dispDraw[(size_t) i] = (float) juce::jlimit (0.0, 1.0, n);
                }
            }
            else
            {
                // Visual: maximum (pôvodné správanie)
                for (int i = 0; i < drawBins; ++i)
                {
                    float m = 0.0f;
                    const int start = i * group;
                    for (int k = 0; k < group; ++k)
                        m = juce::jmax (m, disp[(size_t) (start + k)]);
                    dispDraw[(size_t) i] = m;
                }
            }
        }
        else
        {
            dispDraw.assign (disp.begin(), disp.begin() + (size_t) drawBins);
        }

        // --- log-resampling ---
        const int W = plot.getWidth();
        const int H = plot.getHeight();

        const double nyq     = assumedSampleRateHz * 0.5;
        const double binHz   = nyq / (double) drawBins;
        const double tblBinHz = nyq / (double) bins;   // psycho tables are per SOURCE bin
        const double fmin    = 16.0, fmax = 20000.0;   // down to sub-bass
        const double logSpan = std::log10 (fmax / fmin);

        // Cap the number of plotted points: more than ~1 per pixel is invisible
        // but costs a lot under the software renderer (and scales with HUD width).
        int points = juce::jmin (juce::jmax (W, juce::jmin (drawBins, W)), 4096);
        points = juce::jmin (points, 1100);
        std::vector<float>& line = lineBuf;
        line.assign ((size_t) points, 0.0f);

        auto sampleBuf = [&] (const std::vector<float>& buf, int idx) -> float
        {
            return buf[(size_t) juce::jlimit (0, (int) buf.size() - 1, idx)];
        };

        // Sample a constant-Q stream at frequency f, including the psychoacoustic
        // correction (A-weight / Fletcher-Munson) that the FFT path gets from
        // applyPsycho — shared by the mono curve and both stereo CQT channels.
        auto sampleCqtAt = [&] (const std::vector<float>& buf, double f) -> float
        {
            const double kf = (double) ConstantQAnalyzer::kBinsPerOctave
                            * std::log2 (f / ConstantQAnalyzer::kFMin);
            const int n = (int) buf.size();
            float v = 0.0f;
            if (kf >= 0.0 && kf < (double) (n - 1))
            {
                const int k0 = (int) kf; const float fr = (float) (kf - (double) k0);
                v = buf[(size_t) k0] * (1.0f - fr) + buf[(size_t) (k0 + 1)] * fr;
            }
            else if (kf >= 0.0 && kf < (double) n)
                v = buf[(size_t) (int) kf];

            if (psychoMode != 0)
            {
                double dB = (double) assumedDbFloor
                          + (double) juce::jlimit (0.0f, 1.0f, v) * (0.0 - (double) assumedDbFloor);
                if (psychoMode == 1)
                    dB += (double) aWeighting_dB (f);
                else if (! iso226Db.empty())
                    // index with the SOURCE bin width (the table is per FFT bin) —
                    // indexing with the downsampled width applied the wrong
                    // frequency's correction whenever Max bins < source bins
                    dB += (double) iso226Db[(size_t) juce::jlimit (0, (int) iso226Db.size() - 1,
                                                                   (int) (f / tblBinHz))];
                v = (float) ((dB - (double) assumedDbFloor) / (0.0 - (double) assumedDbFloor));
            }

            return juce::jlimit (0.0f, 1.0f, v);
        };

        // Resample a (downsampled) FFT curve onto the per-pixel points (log-f axis).
        //   measurement: linear interp;  visual: Catmull-Rom.
        // Constant-Q: the log-band MAX aggregate is BLENDED in as the band widens
        // (1 → 3 bins). Narrow bands used to snap straight to the flat MAX of the
        // same bin(s) across many pixels, which drew as piecewise-constant steps
        // ("quantisation levels", most visible in stereo + CQ where the local FFT
        // is small); the blend keeps the curve continuous at every zoom while wide
        // bands still capture every peak. Shared by the L and R channels so both
        // curves read identically.
        auto resampleCurve = [&] (const std::vector<float>& src, std::vector<float>& dst)
        {
            dst.assign ((size_t) points, 0.0f);
            for (int i = 0; i < points; ++i)
            {
                const double u = (points == 1 ? 0.0 : (double) i / (double) (points - 1));
                const double f = fmin * std::pow (10.0, u * logSpan);

                double j = f / binHz - 0.5;
                j = juce::jlimit (0.0, (double) drawBins - 1.000001, j);

                const int   j0   = (int) std::floor (j);
                const int   j1   = juce::jmin (drawBins - 1, j0 + 1);
                const float frac = (float) (j - (double) j0);

                float y;
                if (measurementMode)
                {
                    // Linear interpolácia (žiadny overshoot – SPAN-like)
                    const float p1 = sampleBuf (src, j0);
                    const float p2 = sampleBuf (src, j1);
                    y = p1 + frac * (p2 - p1);
                }
                else
                {
                    // Catmull-Rom (vizuálne plynulé)
                    const float p0 = sampleBuf (src, j0 - 1);
                    const float p1 = sampleBuf (src, j0);
                    const float p2 = sampleBuf (src, j1);
                    const float p3 = sampleBuf (src, j1 + 1);
                    const float t  = frac, t2 = t * t, t3 = t2 * t;
                    const float m1 = 0.5f * (p2 - p0);
                    const float m2 = 0.5f * (p3 - p1);
                    y = (2*t3 - 3*t2 + 1)*p1 + (t3 - 2*t2 + t)*m1
                      + (-2*t3 + 3*t2)*p2 + (t3 - t2)*m2;
                }
                y = juce::jlimit (0.0f, 1.0f, y);

                // (fallback while CQT data not yet arrived) Constant-Q aggregation
                if (constantQ && ! measurementMode)
                {
                    const double uLo = (points == 1) ? 0.0 : ((double) i - 0.5) / (double) (points - 1);
                    const double uHi = (points == 1) ? 1.0 : ((double) i + 0.5) / (double) (points - 1);
                    const double fLo = fmin * std::pow (10.0, juce::jlimit (0.0, 1.0, uLo) * logSpan);
                    const double fHi = fmin * std::pow (10.0, juce::jlimit (0.0, 1.0, uHi) * logSpan);
                    const double bandBins = (fHi - fLo) / binHz;
                    if (bandBins >= 1.0)
                    {
                        const int kLo = juce::jlimit (0, drawBins - 1, (int) std::floor (fLo / binHz));
                        const int kHi = juce::jlimit (kLo, drawBins - 1, (int) std::ceil  (fHi / binHz));
                        float m = 0.0f;
                        for (int k = kLo; k <= kHi; ++k)
                            m = juce::jmax (m, sampleBuf (src, k));
                        const float blend = (float) juce::jlimit (0.0, 1.0, (bandBins - 1.0) * 0.5);
                        y += blend * (juce::jlimit (0.0f, 1.0f, m) - y);
                    }
                }

                dst[(size_t) i] = y;
            }
        };

        if (useCqt)
        {
            // Constant-Q stream: sample the log-spaced CQT bins at each pixel's
            // frequency (incl. the psychoacoustic correction — see sampleCqtAt).
            for (int i = 0; i < points; ++i)
            {
                const double u = (points == 1 ? 0.0 : (double) i / (double) (points - 1));
                line[(size_t) i] = sampleCqtAt (cqtDisp, fmin * std::pow (10.0, u * logSpan));
            }
        }
        else
            resampleCurve (dispDraw, line);

        // --- Peak hold on the DRAWN curve (points domain) → works in EVERY mode,
        //     including Constant-Q. Hold the max of the current curve with a slow fall.
        //     (drawn directly from peakLineHold below — no per-frame copy) ---
        if (peakHoldEnabled)
        {
            if ((int) peakLineHold.size() != points) peakLineHold.assign ((size_t) points, 0.0f);
            for (int i = 0; i < points; ++i)
            {
                const float v = line[(size_t) i];
                peakLineHold[(size_t) i] = (v >= peakLineHold[(size_t) i])
                                             ? v
                                             : juce::jmax (v, peakLineHold[(size_t) i] - peakDecayPerFrame);
            }
        }

        // --- Stereo: R kanál — používa ROVNAKÉ pravidlo ako L kanál:
        //     measurement ON  → power-average downsample + lineárny resample (oba kanály)
        //     measurement OFF → max downsample + Catmull-Rom resample (oba kanály)
        std::vector<float>& lineR = lineRBuf;
        lineR.clear();   // members persist across frames → must be empty when stereo is off
        if (cqStereo)
        {
            // R channel from the TRUE stereo CQT stream — same interpolation and
            // psycho correction as the L curve, full multi-resolution bass detail.
            lineR.assign ((size_t) points, 0.0f);
            for (int i = 0; i < points; ++i)
            {
                const double u = (points == 1 ? 0.0 : (double) i / (double) (points - 1));
                const double f = fmin * std::pow (10.0, u * logSpan);
                lineR[(size_t) i] = sampleCqtAt (cqtDispR, f);
            }
        }
        else if (stereoMode && ! useCqt && (int) dispR.size() == bins)
        {
            std::vector<float>& drawR = drawRBuf;
            if (displayBins > 0 && drawBins < bins && (bins % drawBins) == 0)
            {
                const int group = bins / drawBins;
                drawR.assign ((size_t) drawBins, 0.0f);

                if (measurementMode)
                {
                    const double floor_lin = std::pow (10.0, (double) assumedDbFloor / 10.0);
                    for (int i = 0; i < drawBins; ++i)
                    {
                        double sumP = 0.0;
                        const int start = i * group;
                        for (int k = 0; k < group; ++k)
                        {
                            const double v  = (double) dispR[(size_t) (start + k)];
                            const double dB = (double) assumedDbFloor + v * (0.0 - (double) assumedDbFloor);
                            sumP += std::pow (10.0, dB / 10.0);
                        }
                        const double avgP  = sumP / (double) group;
                        const double avgDb = 10.0 * std::log10 (juce::jmax (avgP, floor_lin));
                        const double nrm   = (avgDb - (double) assumedDbFloor) / (0.0 - (double) assumedDbFloor);
                        drawR[(size_t) i] = (float) juce::jlimit (0.0, 1.0, nrm);
                    }
                }
                else
                {
                    for (int i = 0; i < drawBins; ++i)
                    {
                        float m = 0.0f;
                        const int start = i * group;
                        for (int k = 0; k < group; ++k)
                            m = juce::jmax (m, dispR[(size_t) (start + k)]);
                        drawR[(size_t) i] = m;
                    }
                }
            }
            else
                drawR.assign (dispR.begin(), dispR.begin() + (size_t) drawBins);

            // SAME resampling rule as the L channel (incl. the anti-staircase
            // Constant-Q blend), so both curves read identically.
            resampleCurve (drawR, lineR);
        }

        // (dB / Hz grids are pre-rendered in the bgCache blit at the top)
        const auto& smoothed = line;

        // --- Harmonics detection (overlay bez zmeny hodnôt krivky) ---
        float maxMag = 0.0f;
        for (auto v : dispDraw) maxMag = juce::jmax (maxMag, v);

        std::vector<int>& peakBins = peakBinsBuf;
        peakBins.clear();
        const float peakThreshold = juce::jmax (0.02f, maxMag * 0.28f);
        for (int i = 1; i < drawBins - 1; ++i)
        {
            const float v = dispDraw[(size_t) i];
            if (v >= peakThreshold && v >= dispDraw[(size_t)(i-1)] && v >= dispDraw[(size_t)(i+1)])
                peakBins.push_back (i);
        }
        if (peakBins.empty() && maxMag > 0.0001f)
        {
            int imax = 0;
            for (int i = 0; i < drawBins; ++i)
                if (dispDraw[(size_t)i] > dispDraw[(size_t)imax]) imax = i;
            peakBins.push_back (imax);
        }

        std::vector<char>& mark     = markBuf;
        std::vector<char>& mainPeak = mainPeakBuf;
        std::vector<int>&  mainIndices = mainIndicesBuf;
        mark.assign ((size_t) points, 0);
        mainPeak.assign ((size_t) points, 0);
        mainIndices.clear();

        for (int b : peakBins)
        {
            const double f0 = ((double) b + 0.5) * binHz;
            if (f0 < fmin || f0 > fmax) continue;
            // main peak index
            const double um = std::log10 (f0 / fmin) / logSpan;
            const int pm = (int) std::round (um * (double)(points-1));
            if (pm >= 0 && pm < points) { mainPeak[(size_t)pm] = 1; mainIndices.push_back(pm); }
            // harmonics
            for (int h = 1; h <= 12; ++h)
            {
                const double fh = f0 * (double) h;
                if (fh > fmax) break;
                const double u = std::log10 (fh / fmin) / logSpan;
                const int pi = (int) std::round (u * (double)(points-1));
                if (pi < 0 || pi >= points) continue;
                const int spread = juce::jmax (1, (int)(2 - std::log10((double)h+1.0)));
                for (int s = -spread; s <= spread; ++s)
                {
                    int q = pi + s;
                    if (q >= 0 && q < points) mark[(size_t)q] = 1;
                }
            }
        }

        // --- Fill pod krivkou ---
        {
            juce::Path fillPath;
            bool started = false;
            for (int i = 0; i < points; ++i)
            {
                const float u = (points == 1 ? 0.0f : (float)i / (float)(points-1));
                const float x = xForU (plot, u);
                float y = (float)plot.getBottom() - smoothed[(size_t)i] * (float)H;
                y = juce::jlimit ((float)plot.getY(), (float)plot.getBottom(), y);
                if (!started) { fillPath.startNewSubPath(x,y); started=true; }
                else          fillPath.lineTo(x,y);
            }
            // Close along the bottom, from where the curve ENDED back to where it
            // began. Mirrored, the curve runs right to left, so the two corners
            // trade places or the fill would cross itself.
            fillPath.lineTo (xForU (plot, 1.0), (float) plot.getBottom());
            fillPath.lineTo (xForU (plot, 0.0), (float) plot.getBottom());
            fillPath.closeSubPath();
            juce::ColourGradient grad (lineCol.withAlpha(0.25f), (float)plot.getX(), (float)plot.getY(),
                                       lineCol.withAlpha(0.02f), (float)plot.getX(), (float)plot.getBottom(), false);
            g.setGradientFill (grad);
            g.fillPath (fillPath);
        }

        // --- Stĺpce s harmonics overlay (hodnoty krivky sa NEMENIA) ---
        const float colBaseW  = juce::jmax (1.0f, (float)W / (float)points);
        const juce::Colour baseBgCol = lineCol.withAlpha (0.02f);
        const float lcHue = lineCol.getHue();
        const float lcSat = lineCol.getSaturation();
        const float lcBri = lineCol.getBrightness();
        const int fadeRadius = juce::jmax (2, (int)std::round((double)points * 0.008));

        for (int i = 0; i < points; ++i)
        {
            const float u   = (points==1 ? 0.0f : (float)i / (float)(points-1));
            const float x   = xForU (plot, u);
            const float mag = smoothed[(size_t)i];
            if (mag <= 0.0005f) continue;

            const float hpx = mag * (float)H;
            const float y   = (float)plot.getBottom() - hpx;
            const float bot = (float)plot.getBottom();
            const float alphaTop = juce::jlimit (0.005f, 0.95f, std::pow (mag, 0.6f));
            const float wcol = colBaseW * 1.1f;

            int minDist = INT_MAX;
            for (int pi : mainIndices)
                minDist = juce::jmin (minDist, std::abs(pi - i));

            float fadeWeight = 0.0f;
            if (minDist <= fadeRadius && !mainIndices.empty())
                fadeWeight = std::pow (1.0f - (float)minDist / (float)fadeRadius, 2.0f);

            if (mainPeak[(size_t)i])
            {
                const float thinW = juce::jmax (1.0f, wcol * 0.18f);
                g.setColour (lineCol.withAlpha (juce::jlimit (0.005f, 0.50f, alphaTop * 0.45f)));
                g.fillRect (x - thinW*0.5f, y, thinW, bot - y);
            }
            else if (mark[(size_t)i])
            {
                const float sat = juce::jmin (1.0f, lcSat * 1.30f + fadeWeight * 0.50f);
                const float bri = juce::jmin (1.0f, lcBri * 1.03f + fadeWeight * 0.06f);
                g.setColour (juce::Colour::fromHSV (lcHue, sat, bri, 1.0f)
                                          .withAlpha (juce::jlimit (0.02f, 0.98f, alphaTop * (0.75f + fadeWeight * 0.70f))));
                g.fillRect (x - wcol*0.5f, y, wcol, bot - y);
            }
            else
            {
                const float a = alphaTop * (0.12f + fadeWeight * 0.60f);
                if (a < 0.02f) continue;   // sub-visible: skip the fill (hundreds of
                                           // tiny alpha rects cost real ms per frame)
                g.setColour (baseBgCol.withAlpha (juce::jlimit (0.02f, 0.8f, a)));
                g.fillRect (x - wcol*0.5f, y, wcol, bot - y);
            }
        }

        // --- Stereo: R krivka (komplementárna farba, pod L) ---
        if (! lineR.empty())
        {
            juce::Path pathR;
            bool started = false;
            for (int i = 0; i < points; ++i)
            {
                const float u = (points==1?0.0f:(float)i/(float)(points-1));
                const float x = xForU (plot, u);
                float y = (float)plot.getBottom() - lineR[(size_t)i]*(float)H;
                y = juce::jlimit ((float)plot.getY(), (float)plot.getBottom(), y);
                if (!started) { pathR.startNewSubPath(x,y); started=true; }
                else          pathR.lineTo(x,y);
            }
            // complementary (opposite) or analogous (neighbouring) hue for R
            const float hueShift = (stereoColourMode == 0) ? 0.45f : 0.09f;
            const auto rCol = juce::Colour::fromHSV (std::fmod (lineCol.getHue() + hueShift, 1.0f),
                                                     lineCol.getSaturation(),
                                                     lineCol.getBrightness(), 1.0f);
            g.setColour (rCol.withAlpha (0.9f));
            g.strokePath (pathR, juce::PathStrokeType (1.6f));

            // mini legenda L/R
            g.setFont (fSzInfo);
            g.setColour (lineCol);
            g.drawText ("L", plot.getRight() - 30, plot.getY() + 2, 12, 12, juce::Justification::centred);
            g.setColour (rCol);
            g.drawText ("R", plot.getRight() - 16, plot.getY() + 2, 12, 12, juce::Justification::centred);
        }

        // --- Referenčná krivka (cieľový spektrálny balans žánru) ---
        if (referenceGenre > 0)
        {
            // RBW compensation: the genre silhouettes are calibrated at the
            // default resolution (2048 bins @ 48 kHz → 11.72 Hz/bin). Narrower
            // bins hold LESS broadband energy (−3 dB per halving), so the same
            // track reads lower at higher Max-bins settings. Shifting the target
            // by 10·log10(binHz / 11.72) keeps "distance to the reference"
            // identical at every resolution. (Constant-Q has its own fixed
            // per-band resolution → no shift there.)
            const float rbwComp = useCqt ? 0.0f
                : (float) (10.0 * std::log10 (juce::jmax (0.5, nyq / (double) bins) / 11.71875));
            drawReferenceCurve (g, plot, fmin, logSpan, W, H, nyq / (double) bins, rbwComp);
        }

        // --- Hlavná krivka ---
        {
            juce::Path path;
            bool started = false;
            for (int i = 0; i < points; ++i)
            {
                const float u = (points==1?0.0f:(float)i/(float)(points-1));
                const float x = xForU (plot, u);
                float y = (float)plot.getBottom() - smoothed[(size_t)i]*(float)H;
                y = juce::jlimit ((float)plot.getY(), (float)plot.getBottom(), y);
                if (!started) { path.startNewSubPath(x,y); started=true; }
                else          path.lineTo(x,y);
            }
            g.setColour (lineCol);
            g.strokePath (path, juce::PathStrokeType (2.0f));
        }

        // --- Peak hold krivka (svetlejšia tenká čiara nad hlavnou krivkou) ---
        if (peakHoldEnabled && (int) peakLineHold.size() == points)
        {
            juce::Path pk;
            bool started = false;
            for (int i = 0; i < points; ++i)
            {
                const float u = (points==1?0.0f:(float)i/(float)(points-1));
                const float x = xForU (plot, u);
                float y = (float)plot.getBottom() - peakLineHold[(size_t)i]*(float)H;
                y = juce::jlimit ((float)plot.getY(), (float)plot.getBottom(), y);
                if (!started) { pk.startNewSubPath(x,y); started=true; }
                else          pk.lineTo(x,y);
            }
            g.setColour (lineCol.brighter (0.4f).withAlpha (0.9f));
            g.strokePath (pk, juce::PathStrokeType (1.0f));
        }

        g.restoreState();

        // --- Peak readout pre host overlay label (vpravo hore, vždy citateľné) ---
        if (useCqt)
        {
            int pk = 0; float pv = 0.0f;
            for (int k = 0; k < (int) cqtDisp.size(); ++k)
                if (cqtDisp[(size_t) k] > pv) { pv = cqtDisp[(size_t) k]; pk = k; }
            updatePeakReadout ((double) ConstantQAnalyzer::binFreq (pk),
                               assumedDbFloor + pv * (0.0f - assumedDbFloor));
        }
        else
        {
            int peakBinIdx = 0;
            float peakVal  = 0.0f;
            for (int i = 0; i < drawBins; ++i)
                if (dispDraw[(size_t)i] > peakVal) { peakVal = dispDraw[(size_t)i]; peakBinIdx = i; }

            const double hz = ((double) peakBinIdx + 0.5) * (assumedSampleRateHz * 0.5 / (double) drawBins);
            const float  db = assumedDbFloor + peakVal * (0.0f - assumedDbFloor);
            updatePeakReadout (hz, db);
        }
    }

    // Commit the dominant peak to the overlay readout at a slow, readable rate (~4/s):
    // tracks the loudest peak since the last commit, then holds it long enough to read.
    void updatePeakReadout (double hz, float db)
    {
        if (db > peakDbAccum) { peakDbAccum = db; peakHzAccum = hz; }
        const juce::uint32 now = juce::Time::getMillisecondCounter();
        if (now - peakCommitMs >= 250)
        {
            peakCommitMs = now;
            lastPeakHz = peakHzAccum;
            lastPeakDb = peakDbAccum;
            peakDbAccum = -200.0f;   // reset the window
        }
    }

private:
    IAudioSource& audioSource;

    std::vector<float> cqtDisp;   // EMA-smoothed CQT magnitudes for the display
    std::vector<float> cqtDispR;  // EMA-smoothed RIGHT-channel CQT (stereo CQ overlay)

    // --- Nastavenia ---
    int   displayBins      = 2048;
    bool  constantQ        = false;   // log-band aggregation display
    int   psychoMode       = 0;       // 0=Flat, 1=A-weight, 2=ISO226
    int   phonLevelDb      = 60;
    bool  measurementMode  = false;
    bool  peakHoldEnabled  = false;
    std::vector<float> peakLineHold;  // held maxima of the DRAWN curve (points domain; works in CQT too)
    static constexpr float peakDecayPerFrame = 0.0015f;  // very slow linear fall
    std::atomic<bool> needRebuildTables { true };
    std::atomic<bool> resetPipeline { false };   // message thread -> worker: realloc pipeline
    std::atomic<bool> resetPeak     { false };   // message thread -> worker: clear peak hold
    double assumedSampleRateHz = 48000.0;
    float  assumedDbFloor      = -90.0f;

    // --- EMA koeficienty (štandardizované) ---
    float inputAlpha   = 0.50f;
    float attackAlpha  = 0.70f;
    float releaseAlpha = 0.08f;

    // --- Buffre ---
    std::vector<float> prevInput;
    std::vector<float> disp;
    std::vector<float> prevInputR;   // stereo mode: R channel pipeline
    std::vector<float> dispR;
    std::vector<float> aWeightDb;
    std::vector<float> iso226Db;
    juce::Colour lineColour = juce::Colours::red;

    // Colour-by-tone. The tracker is touched ONLY from renderImage (the async
    // worker), never from the message thread, so the flags below are the only
    // cross-thread state — plain scalars, benign races, same as the rest of the
    // module's settings.
    bool  colourByTone { false };
    bool  toneTwist    { false };
    float toneSmooth   { 0.5f };    // the SLIDER value; the rate curve lives in PitchUtils
    PitchUtils::ToneHueTracker toneHue;

    /** The colour everything is derived from: the note in tone mode, the user's
        pick otherwise. */
    juce::Colour activeLineColour() const noexcept
    {
        return (colourByTone && toneHue.hasTone()) ? toneHue.colour() : lineColour;
    }

    // peak readout (for getPeakText) — committed at a slow, readable rate
    double lastPeakHz = 0.0;
    float  lastPeakDb = -200.0f;
    double peakHzAccum = 0.0;
    float  peakDbAccum = -200.0f;
    juce::uint32 peakCommitMs = 0;

    // --- Reference curve + stereo mode ---
    int  referenceGenre   = 0;   // 0=Off, 1=EDM, 2=Bass, 3=House, 4=Hip-Hop, 5=Pop, 6=Rock
    int  stereoColourMode = 0;   // 0=complementary, 1=analogous
    bool stereoMode       = false;

    // local FFT for stereo mode (packet FFT is mono-mixed)
    std::unique_ptr<juce::dsp::FFT> stereoFft;
    int stereoFftOrder = 0;
    std::vector<float> fftWork, hannWin;

    // --- Per-frame scratch (worker thread only) ---
    // Members so their heap capacity is reused: renderImage does zero
    // steady-state allocation per frame regardless of the bin count.
    std::vector<float> cqRawBuf, cqRawRBuf;
    std::vector<float> inBuf, inRBuf;
    std::vector<float> dispDrawBuf, drawRBuf;
    std::vector<float> lineBuf, lineRBuf;
    std::vector<float> wLBuf, wRBuf;
    std::vector<char>  markBuf, mainPeakBuf;
    std::vector<int>   mainIndicesBuf, peakBinsBuf;

    // static-layer cache (background + grids + labels; see rebuildBgCache)
    juce::Image bgCache;
    unsigned bgCacheTheme { 0xFFFFFFFFu };   // never equals a real generation → first pass rebuilds
    bool  bgCacheInfoHidden { false };       // part of the signature: Hide info changes what is baked
    float bgCacheFloor { 0.0f };
    bool  bgCacheClear { false };   // was the cache baked WITHOUT a background?
    bool  bgCacheMirror{ false };   // ...and which way round was the Hz axis?

    /** High frequencies on the left. See setMirrorFreq. */
    bool  mirrorFreq   { false };

    /** Computes L/R magnitude spectra from the stereo waveform stream.
        Returns the number of bins (0 if no waveform available).
        Output format matches the packet FFT: normalised dB [0..1] over floor..0. */
    int computeStereoFfts (std::vector<float>& outL, std::vector<float>& outR)
    {
        std::vector<float>& wL = wLBuf;   // members: reuse capacity (no per-frame alloc)
        std::vector<float>& wR = wRBuf;
        const int n = audioSource.getLastWaveform (wL, wR);
        if (n < 512) return 0;

        int order = 9;
        while ((1 << (order + 1)) <= n && order < 12) ++order;
        const int size = 1 << order;

        if (stereoFft == nullptr || stereoFftOrder != order)
        {
            stereoFft = std::make_unique<juce::dsp::FFT> (order);
            stereoFftOrder = order;
            hannWin.resize ((size_t) size);
            for (int i = 0; i < size; ++i)
                hannWin[(size_t) i] = 0.5f * (1.0f - std::cos (
                    2.0f * juce::MathConstants<float>::pi * (float) i / (float) (size - 1)));
        }

        auto run = [&] (const std::vector<float>& src, std::vector<float>& out)
        {
            fftWork.assign ((size_t) (2 * size), 0.0f);
            const int offset = n - size;                    // newest samples
            for (int i = 0; i < size; ++i)
                fftWork[(size_t) i] = src[(size_t) (offset + i)] * hannWin[(size_t) i];

            stereoFft->performFrequencyOnlyForwardTransform (fftWork.data());

            const int   bins = size / 2;
            const float norm = 4.0f / (float) size;         // Hann coherent gain × 2/N
            out.resize ((size_t) bins);
            for (int k = 0; k < bins; ++k)
            {
                float dB = 20.0f * std::log10 (juce::jmax (fftWork[(size_t) k] * norm, 1.0e-12f));
                dB = juce::jlimit (assumedDbFloor, 0.0f, dB);
                out[(size_t) k] = (dB - assumedDbFloor) / (0.0f - assumedDbFloor);
            }
        };

        run (wL, outL);
        run (wR, outR);
        return size / 2;
    }

    // ── Static layer cache: background + border + dB/Hz grids + labels ───────
    // Rebuilt only on resize / theme / floor change; renderImage just blits it.
    void rebuildBgCache (int w, int h)
    {
        // CLEARED, not left as raw memory. This used to be allocated uninitialised
        // because paintBackground filled every pixel of it a moment later, so the
        // garbage was always overwritten. As a Fusion layer that fill is skipped
        // on purpose — and the uninitialised block then survived into the cache and
        // was blitted over the whole module, which is where the white rectangle
        // came from.
        bgCache      = juce::Image (juce::Image::ARGB, juce::jmax (2, w), juce::jmax (2, h),
                                    true, juce::SoftwareImageType());
        bgCacheTheme = AlterTheme::themeGeneration;
        bgCacheFloor = assumedDbFloor;
        bgCacheClear = isTransparentBackground();
        bgCacheInfoHidden = AlterTheme::hudInfoHidden.load();
        bgCacheMirror = mirrorFreq;

        juce::Graphics g (bgCache);
        auto bounds = juce::Rectangle<int> (0, 0, w, h);
        auto plot   = bounds;
        const int W = plot.getWidth(), H = plot.getHeight();

        // Transparent means TRANSPARENT: the whole static layer goes, not just the
        // gradient. The border, the dB/Hz grid and its labels are chrome that only
        // makes sense inside a HUD panel — carried into a Fusion they fuse a
        // rectangle and a ladder of lines into the picture, and carried into an
        // alpha export they land on top of whatever footage this was meant to sit
        // over. Either way what is wanted is the curve on its own.
        if (isTransparentBackground())
            return;

        paintModuleBackground (g, bounds.toFloat());
        g.setColour (AlterTheme::navyEdge);
        g.drawRect (bounds, 1);

        const int   shortSide = juce::jmin (w, h);
        const float fSzGrid   = juce::jlimit (6.0f, 10.0f, (float) shortSide * 0.05f);
        const double fmin = 16.0, fmax = 20000.0;
        const double logSpan = std::log10 (fmax / fmin);

        // --- dB Y-os grid ---
        {
            const float dbFloor = assumedDbFloor;
            const float dbGrid[] = { 0.0f, -6.0f, -12.0f, -18.0f, -24.0f, -36.0f, -48.0f, -60.0f, -72.0f, -84.0f };
            const bool showLabels = (H >= 60);
            g.setFont (fSzGrid);
            for (float db : dbGrid)
            {
                if (db < dbFloor) break;
                const float norm = (db - dbFloor) / (0.0f - dbFloor);
                const float yPos = (float) plot.getBottom() - norm * (float) H;
                g.setColour (juce::Colours::grey.withAlpha (db == 0.0f ? 0.5f : 0.2f));
                g.drawHorizontalLine ((int) std::round (yPos), (float) plot.getX(), (float) plot.getRight());
                if (showLabels)
                {
                    g.setColour (juce::Colours::grey.withAlpha (0.7f));
                    juce::String lbl = (db == 0.0f) ? "0" : juce::String ((int) db);
                    g.drawText (lbl, plot.getX() + 2, (int) yPos - (int)fSzGrid - 1,
                                (int)(fSzGrid * 3.5f), (int)(fSzGrid + 2), juce::Justification::centredLeft);
                }
            }
        }

        // --- Hz X-os grid ---
        const double gridHz[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
        g.setColour (juce::Colours::dimgrey.withAlpha (0.55f));
        g.setFont (fSzGrid);
        const bool showHzLabels = (W >= 80);
        for (double hz : gridHz)
        {
            if (hz < fmin || hz > fmax) continue;
            const double u = std::log10 (hz / fmin) / logSpan;
            const float x = xForU (plot, u);
            g.drawVerticalLine ((int) std::round (x), (float) plot.getY(), (float) plot.getBottom());
            if (showHzLabels)
            {
                juce::String txt;
                if (hz >= 1000.0)
                    txt = juce::String (hz / 1000.0, (hz==1000||hz==2000||hz==5000||hz==10000||hz==20000)?0:1) + "k";
                else
                    txt = juce::String ((int) hz);

                // The label sits on the side the scale is GOING, so it never runs
                // off the edge it is nearest: to the right of its line normally,
                // to the left of it when the axis is mirrored.
                const int lblW = (int) (fSzGrid * 4.0f);
                g.drawText (txt,
                            mirrorFreq ? (int) x - 2 - lblW : (int) x + 2,
                            plot.getY() + (int)(fSzGrid * 2.2f),
                            lblW, (int)(fSzGrid + 2),
                            mirrorFreq ? juce::Justification::topRight
                                       : juce::Justification::topLeft);
            }
        }
    }

    // --- Reference tonal-balance curves (long-term average spectra of masters) ---
    //  The curve gets the SAME psychoacoustic correction (A-weight / Fletcher-Munson)
    //  as the live spectrum, so it stays a valid target under any curve setting.
    void drawReferenceCurve (juce::Graphics& g, juce::Rectangle<int> plot,
                             double fmin, double logSpan, int W, int H, double binHz,
                             float rbwCompDb = 0.0f)
    {
        static const float refFreqs[12] = { 25, 40, 63, 100, 160, 250,
                                            500, 1000, 2000, 4000, 8000, 16000 };
        struct RefCurve { const char* name; float db[12]; };
        static const RefCurve kRefs[] = {
            { "EDM",          { -25,-18,-17,-20,-24,-27,-31,-33,-35,-37,-41,-48 } },
            { "Bass music",   { -20,-13,-14,-19,-25,-29,-33,-36,-38,-40,-44,-51 } },
            { "House/Techno", { -27,-20,-18,-21,-24,-27,-30,-32,-34,-37,-42,-50 } },
            { "Hip-Hop",      { -24,-16,-16,-20,-25,-28,-32,-35,-37,-40,-45,-52 } },
            { "Pop",          { -30,-24,-21,-22,-25,-27,-30,-32,-33,-35,-39,-46 } },
            { "Rock",         { -32,-26,-22,-22,-24,-26,-28,-30,-32,-34,-39,-47 } },
        };

        const auto& ref = kRefs[juce::jlimit (0, 5, referenceGenre - 1)];

        // psychoacoustic correction at frequency f (same as applied to the spectrum)
        auto psychoDb = [&] (double f) -> float
        {
            if (psychoMode == 1)
                return aWeighting_dB (f);
            if (psychoMode == 2 && ! iso226Db.empty())
            {
                const int bin = juce::jlimit (0, (int) iso226Db.size() - 1,
                                              (int) (f / binHz));
                return iso226Db[(size_t) bin];
            }
            return 0.0f;
        };

        // build the curve: log-f linear interpolation between control points
        juce::Path path;
        const int steps = juce::jmax (32, W / 4);
        for (int i = 0; i <= steps; ++i)
        {
            const double u = (double) i / (double) steps;
            const double f = fmin * std::pow (10.0, u * logSpan);

            int lo = 0;
            while (lo < 10 && f > refFreqs[lo + 1]) ++lo;
            const double lf0 = std::log10 (refFreqs[lo]), lf1 = std::log10 (refFreqs[lo + 1]);
            const double t = juce::jlimit (0.0, 1.0, (std::log10 (f) - lf0) / (lf1 - lf0));
            const float dB = ref.db[lo] + (float) t * (ref.db[lo + 1] - ref.db[lo])
                           + psychoDb (f) + rbwCompDb;

            const float normV = juce::jlimit (0.0f, 1.0f,
                                              (dB - assumedDbFloor) / (0.0f - assumedDbFloor));
            const float x = xForU (plot, u);
            const float y = (float) plot.getBottom() - normV * (float) H;

            if (i == 0) path.startNewSubPath (x, y);
            else        path.lineTo (x, y);
        }

        // dashed stroke
        juce::Path dashed;
        const float dashes[] = { 6.0f, 4.0f };
        juce::PathStrokeType (1.4f).createDashedStroke (dashed, path, dashes, 2);
        g.setColour (AlterTheme::mintGlow.withAlpha (0.85f));
        g.fillPath (dashed);

        // label
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (juce::String ("REF: ") + ref.name,
                    plot.getX() + 4, plot.getY() + 2, 130, 12,
                    juce::Justification::centredLeft, false);
    }

    // --- A-weighting ---
    void buildAWeightTable (int bins, double sr)
    {
        aWeightDb.resize ((size_t) bins, 0.0f);
        const double binHz = sr * 0.5 / (double) bins;
        for (int i = 0; i < bins; ++i)
            aWeightDb[(size_t)i] = aWeighting_dB ((i + 0.5) * binHz);
    }

    static float aWeighting_dB (double f)
    {
        if (f <= 0.0) return 0.0f;
        const double f2  = f * f;
        const double num = (12200.0*12200.0) * f2 * f2;
        const double den = (f2 + 20.6*20.6) * (f2 + 12200.0*12200.0)
                         * std::sqrt ((f2 + 107.7*107.7) * (f2 + 737.9*737.9));
        return (float) (20.0 * std::log10 (num / den) + 2.0);
    }

    // --- ISO 226:2003 (Fletcher-Munson) equal-loudness ---
    // Vráti korekciu v dB pre dané frekvencie pri zadanom phone leveli.
    // Použijeme štandardné tabuľkové hodnoty ISO 226:2003 pre 11 frekvencií
    // a lineárne interpolujeme v log-f doméne.
    void buildISO226Table (int bins, double sr, int phon)
    {
        iso226Db.resize ((size_t) bins, 0.0f);
        const double binHz = sr * 0.5 / (double) bins;

        // ISO 226:2003 tabuľka: frekvencia [Hz], Lf (SPL pri 0 phon) a Tf (prah)
        // Korekcia = phon - SPL(f) pri 1 kHz normalizovaná
        // Zjednodušená implementácia: equal loudness contour pre dané phon
        // alfa_f a Lu_f z ISO 226:2003 Table 1
        static const double iso_f[]   = { 20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160,
                                           200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
                                           2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500 };
        static const double iso_af[]  = { 0.532,0.506,0.480,0.455,0.432,0.409,0.387,0.367,0.349,0.330,
                                           0.315,0.301,0.288,0.276,0.267,0.259,0.253,0.250,0.246,0.244,
                                           0.243,0.243,0.243,0.242,0.242,0.245,0.254,0.271,0.301 };
        static const double iso_Lu[]  = {-31.6,-27.2,-23.0,-19.1,-15.9,-13.0,-10.3,-8.1,-6.2,-4.5,
                                           -3.1,-2.0,-1.1,-0.4, 0.0, 0.3, 0.5, 0.0,-2.7,-4.1,
                                           -1.0, 1.7, 2.5, 1.2,-2.1,-7.1,-11.2,-10.7,-3.1 };
        static const double iso_Tf[]  = { 78.5,68.7,59.5,51.1,44.0,37.5,31.5,26.5,22.1,17.9,
                                           14.4,11.4, 8.6, 6.2, 4.4, 3.0, 2.2, 2.4, 3.5, 1.7,
                                           -1.3,-4.2,-6.0,-5.4,-1.5, 6.0,12.6,13.9,12.3 };
        const int N = 29;

        // Výpočet SPL pri danom phone pre každú frekvenciu v tabuľke
        // ISO 226: Lp = (10/alpha_f) * log10( (L_N^(alpha_f/10) - 4*10^-10 * Lu_f^(alpha_f/10))^(10/alpha_f) ) + Tf
        // Zjednodušenie: SPL(f,phon) = Tf + ( phon - Lp_1kHz_ref ) / alpha_f * 10 ... pozri normu
        // Priamočiara implementácia podľa ISO 226:2003 eq.4:
        // Af = 4.47e-3 * (10^(0.025*Ln) - 1.15) + (0.4 * 10^( (Tf+Lu)/10 - 9 ))^alpha_f
        // Lp = (10/alpha_f) * log10(Af) - Lu + 94

        auto iso226_spl = [&] (int idx, double ln) -> double
        {
            const double af  = iso_af[idx];
            const double lu  = iso_Lu[idx];
            const double tf  = iso_Tf[idx];
            const double Af  = 4.47e-3 * (std::pow (10.0, 0.025 * ln) - 1.15)
                             + std::pow (0.4 * std::pow (10.0, (tf + lu) / 10.0 - 9.0), af);
            return (10.0 / af) * std::log10 (juce::jmax (Af, 1e-30)) - lu + 94.0;
        };

        // SPL pri 1 kHz (index 17) pre normalizáciu
        const double spl1k = iso226_spl (17, (double) phon);

        // Korekcia na poslednom bode tabuľky (12500 Hz) – potrebná pre fade nad 12.5kHz
        const double corrAtLast = spl1k - iso226_spl (N - 1, (double) phon);
        // Korekcia na prvom bode tabuľky (20 Hz) – pre fade pod 20 Hz
        const double corrAtFirst = spl1k - iso226_spl (0, (double) phon);

        // Fade koniec: nad poslednou hodnotou tabuľky plynule taper do 0 pri 20kHz
        const double fadeEndHz = 20000.0;

        for (int i = 0; i < bins; ++i)
        {
            const double f = (i + 0.5) * binHz;

            if (f <= 0.0)
            {
                iso226Db[(size_t)i] = 0.0f;
                continue;
            }

            // Pod minimálnou frekvenciou tabuľky (20 Hz): taper od 0 na corrAtFirst
            if (f < iso_f[0])
            {
                const double t = f / iso_f[0]; // 0..1
                iso226Db[(size_t)i] = (float) (corrAtFirst * t);
                continue;
            }

            // Nad maximálnou frekvenciou tabuľky (12500 Hz):
            // plynulý cosine fade z corrAtLast na 0 pri 20kHz – žiadny schod
            if (f > iso_f[N - 1])
            {
                if (f >= fadeEndHz)
                {
                    iso226Db[(size_t)i] = 0.0f;
                }
                else
                {
                    // cosine taper: plynší ako lineárny (žiadny "kink" na 12.5kHz)
                    const double tLin = (f - iso_f[N-1]) / (fadeEndHz - iso_f[N-1]); // 0..1
                    const double tCos = 0.5 * (1.0 - std::cos (tLin * juce::MathConstants<double>::pi)); // 0..1 smooth
                    iso226Db[(size_t)i] = (float) (corrAtLast * (1.0 - tCos));
                }
                continue;
            }

            // Normálny prípad: lineárna interpolácia v log-f doméne
            int lo = 0;
            for (int k = 0; k < N - 1; ++k)
                if (iso_f[k] <= f && f <= iso_f[k+1]) { lo = k; break; }

            const double logF  = std::log10 (f);
            const double logF0 = std::log10 (iso_f[lo]);
            const double logF1 = std::log10 (iso_f[lo+1]);
            const double t     = (logF - logF0) / (logF1 - logF0);

            const double spl0 = iso226_spl (lo,   (double) phon);
            const double spl1 = iso226_spl (lo+1, (double) phon);
            const double spl  = spl0 + t * (spl1 - spl0);

            // Korekcia = spl1k - spl:
            // záporná tam kde sluch nie je citlivý (nízke + vysoké f) → potlačenie
            // kladná tam kde sluch je citlivejší (okolo 3-4kHz) → zvýraznenie
            iso226Db[(size_t)i] = (float) (spl1k - spl);
        }
    }
};
