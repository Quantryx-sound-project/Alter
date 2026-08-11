/*
  ==============================================================================

    VisualOscilator.h
    Created: 3 Mar 2026 1:38:15pm
    Author:  Martin

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include "AudioSourceInterface.h"
#include "AlterTheme.h"
#include "PitchUtils.h"

// VisualOscilator: three view modes ("Term"):
//
// 0 SHORT TERM:    direct waveform, 0.1-85 ms window with zero-crossing
//                  trigger; Mono / Stereo (L up, R down) / Mirror display.
// 1 LEVEL HISTORY: DAW-style scrolling level envelope (peak silhouette +
//                  RMS core, 60 Hz) over 0.1-30 s.
// 2 LONG WAVE:     the RAW waveform itself, stored continuously and drawn
//                  over 0.02-30 s (min/max per pixel; at short windows the
//                  actual sample curve) - short term stretched in time, but at
//                  TRUE amplitude: unlike short term it is not peak-normalised,
//                  so it carries a real dB scale.
//
// Mouse wheel over the module zooms the window in modes 1 & 2.

class VisualOscilator  : public ThemedBackground,
                         public juce::Component,
                         private juce::Timer
{
public:
    // Mono        : single mono mix
    // Stereo      : L and R overlaid on ONE baseline (compare L vs R directly)
    // Mirror      : single channel mirrored around the centre
    // MirrorStereo: L on top half, R on bottom half inverted (mirror-style stereo)
    enum class DisplayMode { Mono, Stereo, Mirror, MirrorStereo };

    static constexpr int kEnvelopeRate = 60;            // capture ticks per second
    static constexpr int kMaxWindowSec = 30;

    explicit VisualOscilator (IAudioSource& receiver)
        : audioSource (receiver)
    {
        setOpaque (true);
        lineColour = juce::Colours::violet;
        setLongTermWindow (10.0f);   // also sizes the dynamic raw + envelope buffers

        startTimerHz (kEnvelopeRate);
    }

    ~VisualOscilator() override { stopTimer(); }

    /** Notified when the user changes the window with the mouse wheel. */
    std::function<void (float)> onLongTermWindowChanged;

    /** View mode: 0 = Short-Term Scope, 2 = Long Waveform.
        (1 was Level history, now moved to the Audio Meter module.) */
    void setTermMode (int mode) noexcept { termMode = juce::jlimit (0, 2, mode); repaint(); }

    /** Shortest Long Waveform window, seconds.

        NOT lower than this, and the limit is the capture rate, not taste. The raw
        ring is filled from a 60 Hz timer, so roughly 800 samples land per frame at
        48 kHz — about 17 ms of audio. Once the window is shorter than one frame's
        worth, every frame replaces the whole picture with a fresh slice starting at
        an arbitrary phase, and since this view has no trigger (unlike the Short-Term
        Scope) the wave slides instead of standing still. 20 ms keeps a full frame
        inside the window with a little to spare, which is the point at which the
        trace still holds together on its own. */
    static constexpr float kMinWindowSec = 0.02f;

    /** Visible window for the Long Waveform view, seconds (0.02-30).
        The raw ring is sized to the window – nothing extra is stored. */
    void setLongTermWindow (float seconds)
    {
        ltWindowSec = juce::jlimit (kMinWindowSec, (float) kMaxWindowSec, seconds);
        resizeRawBuffer();   // raw waveform ring, dynamic to the window
        repaint();
    }

    /** Mouse wheel zooms the window in the Long Waveform mode. */
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        if (termMode == 0) return;
        const float factor = (wheel.deltaY > 0) ? 0.8f : 1.25f;
        setLongTermWindow (ltWindowSec * factor);
        if (onLongTermWindowChanged)
            onLongTermWindowChanged (ltWindowSec);
    }

    void setFill (bool b) noexcept { showFill = b; repaint(); }
    void setColour (juce::Colour c) noexcept { lineColour = c; repaint(); }
    void setLineColour (juce::Colour c) noexcept { setColour (c); }

    // ── Colour by tone ────────────────────────────────────────────────────────
    //  Every stroke, fill, crosshair and the rotated-hue R channel all derive from
    //  one colour, so this covers BOTH terms (Short-Term Scope and Long Waveform)
    //  and all four display modes without any per-mode branching.
    void setColourByTone (bool b) noexcept
    {
        if (colourByTone == b) return;
        colourByTone = b;
        // The held hue is as old as the last tone-mode frame; snap rather than slide
        // across the wheel from a note that stopped sounding minutes ago.
        if (b) toneHue.reset();
        repaint();
    }

    /** 'Mirror tone color' — reverses the tone→hue wheel (see PitchUtils). */
    void setToneTwist (bool b) noexcept { toneTwist = b; repaint(); }

    /** 'Tone smooth' 0..1: HIGH = heavier smoothing (slow colour reaction), LOW =
        fast. Stored RAW — PitchUtils::toneSmoothToRate turns it into a chase rate,
        so the curve is defined once instead of once per module. */
    void setToneSmooth (float s01) noexcept { toneSmooth = juce::jlimit (0.0f, 1.0f, s01); }

    void setSmoothAmount (float s01) noexcept
    {
        s01 = juce::jlimit (0.0f, 1.0f, s01);
        smooth01 = s01;
        if (s01 <= 0.000001f)
        {
            smoothAlpha = 0.0f;
            return;
        }
        const float minAlpha = 0.08f, maxAlpha = 0.60f;
        float a = juce::jmap (s01, 1.0f, 0.0f, minAlpha, maxAlpha);
        smoothAlpha = juce::jlimit (0.001f, 0.95f, a);
    }

    void setDisplayMode (DisplayMode m) noexcept { displayMode = m; repaint(); }
    void setZoom (float z) noexcept { zoom01 = juce::jlimit (0.0f, 1.0f, z); repaint(); }
    void setAssumedSampleRate (double sr) noexcept { assumedSampleRateHz = (sr > 0.0 ? sr : 48000.0); }

    /** Overlaid Stereo display: the R channel's hue relative to L.
        0 = complementary (the far side of the wheel — maximum separation, what this
        module has always drawn), 1 = analogous (a neighbouring hue — the two
        channels read as one instrument in two shades rather than as two signals).

        Same choice, same two hue distances and the same wording as the Spectrum's
        L/R colour, so a HUD carrying both can be set to match. It applies in BOTH
        terms, because the Short-Term Scope and the Long Waveform draw the R channel
        from the same offset. */
    void setStereoColourMode (int mode) noexcept
    {
        const bool a = (mode == 1);
        if (stereoAnalogous != a) { stereoAnalogous = a; repaint(); }
    }

    /** Long Waveform: draw the 0 dBFS boundary, the right-edge dB ruler, and a red
        mark over every column that reached full scale.

        SHORT TERM DELIBERATELY DOES NOT GET IT. That view normalises the trace to
        its own peak so a quiet passage still fills the window — which means a given
        height there is a fraction of whatever the loudest sample happened to be, not
        a dB value. A "0 dB" line drawn on it would sit at the top of the picture at
        every level, i.e. a scale whose numbers change meaning every frame. The Long
        Waveform draws raw amplitude, so there the scale is real. */
    void setClipZone (bool b) noexcept { if (showClipZone != b) { showClipZone = b; repaint(); } }

    DisplayMode getDisplayMode() const { return displayMode; }

    /** Readout for the pointer at `p` (module-local), or empty when there is
        nothing to say. The host draws it — this view gets no mouse events itself.

        Reports what the POSITION means: how far into the window that column is, and
        what level that height stands for. In the Long Waveform the level is absolute
        dBFS; in the Short-Term Scope the trace is peak-normalised, so it is marked
        "rel" rather than printed as a dBFS number it is not. */
    juce::String cursorText (juce::Point<float> p) const
    {
        auto area = getLocalBounds().toFloat();
        if (! area.contains (p) || area.getWidth() < 4.0f || area.getHeight() < 4.0f) return {};

        const bool longWave = (termMode == 2);

        // MirrorStereo stacks two independent pictures; the pointer measures against
        // the centre line of the half it is actually in.
        if (displayMode == DisplayMode::MirrorStereo)
        {
            auto top = area.withHeight (area.getHeight() * 0.5f);
            area = (p.y < top.getBottom()) ? top : top.translated (0.0f, top.getHeight());
        }

        const float u      = juce::jlimit (0.0f, 1.0f, (p.x - area.getX()) / area.getWidth());
        const float vScale = area.getHeight() * (longWave ? kLongVScale : kShortVScale);
        const float amp    = std::abs (p.y - area.getCentreY()) / juce::jmax (1.0f, vScale);

        juce::String t;

        if (longWave)
        {
            const float secsAgo = (1.0f - u) * ltWindowSec;
            t << (secsAgo < 0.05f ? juce::String ("now")
                                  : "-" + juce::String (secsAgo, secsAgo < 10.0f ? 2 : 1) + " s");
        }
        else
        {
            const double ms = shortTermWindowSec() * 1000.0 * (double) u;
            t << (ms >= 1.0 ? juce::String (ms, 2) + " ms"
                            : juce::String (ms * 1000.0, 0) + juce::String::charToString (0x00B5) + "s");
        }

        t << "  |  "
          << (amp < 0.0011f ? juce::String ("-inf dB")
                            : juce::String (20.0f * std::log10 (amp), 1) + " dB")
          << (longWave ? "" : " rel");
        return t;
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        paintModuleBackground (g, bounds);
        // Kept in a transparent export like every other readable detail; "Hide
        // info" is the one switch that strips the module back to its picture.
        if (! AlterTheme::hudInfoHidden.load())
        {
            g.setColour (AlterTheme::navyEdge);
            g.drawRect (bounds, 1.0f);
        }

        if (termMode == 2) { paintLongWave (g, bounds); return; }   // Long Waveform

        // Get stereo waveform from audio source
        std::vector<float> waveL, waveR;
        const int count = audioSource.getLastWaveform (waveL, waveR);

        if (count < 4)
            return;

        const int W = juce::jmax (64, getWidth());

        // --- Time window from zoom ---
        const double timeWindowSec = shortTermWindowSec();
        const int displaySamples = juce::jlimit (2, count - 1,
            (int) (assumedSampleRateHz * timeWindowSec));

        // Trigger: search backwards for a positive zero crossing
        // Because displaySamples matches the fundamental period, successive
        // trigger-to-trigger jumps are exactly one period → phase stays locked.
        const int latestStart = juce::jmax (0, count - displaySamples);
        int triggerIdx = latestStart;
        {
            const int searchMin = juce::jmax (1, latestStart - displaySamples);
            for (int i = latestStart; i >= searchMin; --i)
            {
                if (waveL[(size_t) (i - 1)] <= 0.0f && waveL[(size_t) i] > 0.0f)
                {
                    triggerIdx = i;
                    break;
                }
            }
        }

        // Extract display region
        const int dispLen = juce::jmin (displaySamples, count - triggerIdx);
        if (dispLen < 2) return;

        // Resample to W pixels using Catmull-Rom cubic interpolation
        std::vector<float> dispL ((size_t) W, 0.0f);
        std::vector<float> dispR ((size_t) W, 0.0f);

        auto catmullRom = [] (float p0, float p1, float p2, float p3, float t) -> float
        {
            return 0.5f * ((2.0f * p1)
                + (-p0 + p2) * t
                + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t
                + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
        };

        for (int x = 0; x < W; ++x)
        {
            const float fIdx = (float) x / (float) (W - 1) * (float) (dispLen - 1);
            const int i1 = triggerIdx + (int) fIdx;
            const float frac = fIdx - (float) (int) fIdx;

            const int i0 = juce::jmax (0, i1 - 1);
            const int i2 = juce::jmin (i1 + 1, count - 1);
            const int i3 = juce::jmin (i1 + 2, count - 1);

            if (i1 >= 0 && i1 < count)
            {
                dispL[(size_t) x] = catmullRom (waveL[(size_t) i0], waveL[(size_t) i1],
                                                 waveL[(size_t) i2], waveL[(size_t) i3], frac);
                dispR[(size_t) x] = catmullRom (waveR[(size_t) i0], waveR[(size_t) i1],
                                                 waveR[(size_t) i2], waveR[(size_t) i3], frac);
            }
        }

        // Apply temporal smoothing (EMA between frames)
        if (smoothAlpha > 0.000001f)
        {
            if ((int) prevL.size() != W) prevL.assign ((size_t) W, 0.0f);
            if ((int) prevR.size() != W) prevR.assign ((size_t) W, 0.0f);

            for (int i = 0; i < W; ++i)
            {
                prevL[(size_t) i] += smoothAlpha * (dispL[(size_t) i] - prevL[(size_t) i]);
                prevR[(size_t) i] += smoothAlpha * (dispR[(size_t) i] - prevR[(size_t) i]);
                dispL[(size_t) i] = prevL[(size_t) i];
                dispR[(size_t) i] = prevR[(size_t) i];
            }
        }
        else
        {
            prevL.clear();
            prevR.clear();
        }

        // Normalize
        float maxAbs = 1e-9f;
        for (int i = 0; i < W; ++i)
        {
            maxAbs = juce::jmax (maxAbs, std::abs (dispL[(size_t) i]));
            maxAbs = juce::jmax (maxAbs, std::abs (dispR[(size_t) i]));
        }
        if (maxAbs > 0.0f)
        {
            for (int i = 0; i < W; ++i)
            {
                dispL[(size_t) i] /= maxAbs;
                dispR[(size_t) i] /= maxAbs;
            }
        }

        // Draw crosshair (always visible)
        {
            const float cy = bounds.getCentreY();
            g.setColour (activeColour().withAlpha (0.30f));
            g.fillRect (bounds.getX(), cy - 0.5f, bounds.getWidth(), 1.0f);
        }

        // Build paths based on display mode
        const float vScale = bounds.getHeight() * kShortVScale;
        juce::Path pMain;
        juce::Path pSecondary;

        for (int x = 0; x < W; ++x)
        {
            const float u = (float) x / (float) (W - 1);
            const float px = bounds.getX() + u * bounds.getWidth();

            float mainY, secY;

            switch (displayMode)
            {
                case DisplayMode::Mono:
                    mainY = bounds.getCentreY() - dispL[(size_t) x] * vScale;
                    if (x == 0) pMain.startNewSubPath (px, mainY);
                    else        pMain.lineTo (px, mainY);
                    break;

                case DisplayMode::Stereo:
                    // L and R overlaid on the SAME baseline (see the L/R difference)
                    mainY = bounds.getCentreY() - dispL[(size_t) x] * vScale;
                    secY  = bounds.getCentreY() - dispR[(size_t) x] * vScale;
                    if (x == 0)
                    {
                        pMain.startNewSubPath (px, mainY);
                        pSecondary.startNewSubPath (px, secY);
                    }
                    else
                    {
                        pMain.lineTo (px, mainY);
                        pSecondary.lineTo (px, secY);
                    }
                    break;

                case DisplayMode::MirrorStereo:
                    // L channel on top half, R channel on bottom half (inverted)
                    mainY = bounds.getCentreY() - dispL[(size_t) x] * vScale;
                    secY  = bounds.getCentreY() + dispR[(size_t) x] * vScale;
                    if (x == 0)
                    {
                        pMain.startNewSubPath (px, mainY);
                        pSecondary.startNewSubPath (px, secY);
                    }
                    else
                    {
                        pMain.lineTo (px, mainY);
                        pSecondary.lineTo (px, secY);
                    }
                    break;

                case DisplayMode::Mirror:
                    mainY = bounds.getCentreY() - dispL[(size_t) x] * vScale;
                    secY  = bounds.getCentreY() + dispL[(size_t) x] * vScale;
                    if (x == 0)
                    {
                        pMain.startNewSubPath (px, mainY);
                        pSecondary.startNewSubPath (px, secY);
                    }
                    else
                    {
                        pMain.lineTo (px, mainY);
                        pSecondary.lineTo (px, secY);
                    }
                    break;
            }
        }

        // Fill between waveform and centre line (only when fill enabled)
        if (showFill)
        {
            const float cy = bounds.getCentreY();

            juce::Path fillPath (pMain);
            fillPath.lineTo (bounds.getRight(), cy);
            fillPath.lineTo (bounds.getX(), cy);
            fillPath.closeSubPath();

            g.setColour (activeColour().withAlpha (0.25f));
            g.fillPath (fillPath);
        }

        // Stroke paths
        g.setColour (activeColour());
        g.strokePath (pMain, juce::PathStrokeType (2.0f));
        if (! pSecondary.isEmpty())
        {
            // In overlaid Stereo the R channel gets a distinct hue so the two
            // channels can be told apart; mirror modes keep the faint same colour.
            if (displayMode == DisplayMode::Stereo)
                g.setColour (stereoRColour().withAlpha (0.9f));
            else
                g.setColour (activeColour().withAlpha (0.5f));
            g.strokePath (pSecondary, juce::PathStrokeType (1.4f));
        }

        // Time labels (bottom edge: "0" left, duration right)
        {
            const double twMs = timeWindowSec * 1000.0;
            juce::String endLabel;
            if (twMs >= 1.0)
                endLabel = juce::String (twMs, 1) + " ms";
            else
                endLabel = juce::String (twMs * 1000.0, 0) + juce::String::charToString (0x00B5) + "s";

            g.setColour (juce::Colours::lightgrey.withAlpha (0.7f));
            g.setFont (11.0f);
            const int lblH = 14;
            const int lblY = (int) bounds.getBottom() - lblH - 1;
            g.drawText ("0", (int) bounds.getX() + 3, lblY, 24, lblH, juce::Justification::centredLeft);
            g.drawText (endLabel, (int) bounds.getRight() - 64, lblY, 60, lblH, juce::Justification::centredRight);
        }
    }

    void resized() override {}

private:
    IAudioSource& audioSource;
    juce::Colour lineColour;

    // Colour by tone. Updated once per timer tick; read by paint().
    bool  colourByTone { false };
    bool  toneTwist    { false };
    float toneSmooth   { 0.5f };    // the SLIDER value; the rate curve lives in PitchUtils
    PitchUtils::ToneHueTracker toneHue;

    /** The colour everything is derived from: the note in tone mode, the user's
        pick otherwise. */
    juce::Colour activeColour() const noexcept
    {
        return (colourByTone && toneHue.hasTone()) ? toneHue.colour() : lineColour;
    }

    bool showFill { false };
    double assumedSampleRateHz { 48000.0 };

    // Pixel columns that reached full scale this frame (1 = over). See paintLongWave.
    std::vector<char> overCols;

    /** Lay a red bar along both boundaries wherever the audio reached full scale.
        Drawn as RUNS, not per pixel: a fill call per column is 1000+ tiny rects on a
        wide module, and consecutive over-columns are the normal case. */
    void drawOverMarks (juce::Graphics& g, juce::Rectangle<float> area,
                        float cy, float vScale) const
    {
        const int W = (int) overCols.size();
        if (W < 2 || area.getWidth() < 2.0f) return;

        const float pxW = area.getWidth() / (float) (W - 1);
        g.setColour (juce::Colours::red.withAlpha (0.85f));

        for (int x = 0; x < W; )
        {
            if (! overCols[(size_t) x]) { ++x; continue; }
            const int start = x;
            while (x < W && overCols[(size_t) x]) ++x;

            const float x0 = area.getX() + (float) start * pxW;
            const float w  = juce::jmax (1.5f, (float) (x - start) * pxW);
            g.fillRect (x0, cy - vScale - 1.0f, w, 2.5f);
            g.fillRect (x0, cy + vScale - 1.5f, w, 2.5f);
        }
    }

    bool stereoAnalogous { false };   // false = complementary R hue (the original look)
    bool showClipZone   { true };    // Long Waveform: 0 dB line pair + right-edge dB ruler

    /** Half-height each view draws its trace against, as a fraction of the area.
        Two different numbers because the two views always used two different ones;
        naming them keeps the cursor readout and the clip lines measuring against
        the same scale the drawing does instead of a copied literal that can drift. */
    static constexpr float kShortVScale = 0.42f;
    static constexpr float kLongVScale  = 0.48f;

    /** Short-Term Scope visible window, seconds. zoom01 maps logarithmically:
        0.0 = 85 ms (~12 Hz), 1.0 = 0.1 ms (10 kHz). */
    double shortTermWindowSec() const noexcept
    {
        return 0.085 * std::pow (0.001176, (double) zoom01);
    }

    /** The R channel's colour in the overlaid Stereo display. */
    juce::Colour stereoRColour() const noexcept
    {
        return activeColour().withRotatedHue (stereoAnalogous ? 0.12f : 0.45f);
    }

    DisplayMode displayMode { DisplayMode::Mono };
    float zoom01 { 0.420f };

    float smooth01 { 0.0f };
    float smoothAlpha { 0.0f };
    std::vector<float> prevL, prevR;

    int  termMode  { 0 };          // 0 = Short-Term Scope, 2 = Long Waveform
    float ltWindowSec { 0.0f };
    std::vector<float> ltWaveL, ltWaveR;   // reused capture buffers

    // raw waveform ring for the LONG WAVE view (full sample rate). The buffer is
    // sized dynamically to exactly one window; rawFilled tracks valid samples.
    static constexpr int kRawRate = 48000;
    std::vector<float> rawL, rawR;
    int rawWrite  { 0 };
    int rawFilled { 0 };
    long long rawTotal { 0 };   // running count of all samples ever captured (absolute clock)

    void captureRaw (int count)
    {
        if (count <= 0) return;
        const int total = (int) rawL.size();
        if (total <= 0) return;
        const int n = juce::jmin (count, (int) (assumedSampleRateHz / kEnvelopeRate) + 16);

        for (int i = count - n; i < count; ++i)
        {
            rawL[(size_t) rawWrite] = ltWaveL[(size_t) i];
            rawR[(size_t) rawWrite] = ltWaveR[(size_t) i];
            rawWrite  = (rawWrite + 1) % total;
            rawFilled = juce::jmin (rawFilled + 1, total);
            ++rawTotal;
        }
    }

    // Resize the raw ring to match the current window, preserving the newest
    // samples at [0..keep) (oldest→newest); the rest fills in over time.
    void resizeRawBuffer()
    {
        const int maxCap = kRawRate * kMaxWindowSec;
        const int wanted  = juce::jlimit (8, maxCap, (int) (ltWindowSec * (float) kRawRate));
        const int oldSize = (int) rawL.size();
        if (wanted == oldSize) return;

        std::vector<float> freshL ((size_t) wanted, 0.0f);
        std::vector<float> freshR ((size_t) wanted, 0.0f);

        const int keep = juce::jmin (juce::jmin (wanted, rawFilled), oldSize);
        for (int i = 0; i < keep; ++i)
        {
            const int src = ((rawWrite - keep + i) % oldSize + oldSize) % oldSize;
            freshL[(size_t) i] = rawL[(size_t) src];
            freshR[(size_t) i] = rawR[(size_t) src];
        }
        rawL.swap (freshL);
        rawR.swap (freshR);
        rawFilled = keep;
        rawWrite  = keep % wanted;
    }

    // Light box smoothing over the valid (has) region — removes the moiré
    // shimmer of the min/max band when the window is zoomed out.
    static void smoothEdges (std::vector<float>& a, const std::vector<char>& has, int r)
    {
        if (r < 1) return;
        const int n = (int) a.size();
        std::vector<float> t (a);
        for (int x = 0; x < n; ++x)
        {
            if (! has[(size_t) x]) continue;
            float sum = 0.0f; int cnt = 0;
            for (int k = -r; k <= r; ++k)
            {
                const int j = x + k;
                if (j >= 0 && j < n && has[(size_t) j]) { sum += a[(size_t) j]; ++cnt; }
            }
            if (cnt > 0) t[(size_t) x] = sum / (float) cnt;
        }
        a.swap (t);
    }

    void drawTimeLabels (juce::Graphics& g, juce::Rectangle<float> bounds) const
    {
        g.setColour (juce::Colours::lightgrey.withAlpha (0.7f));
        g.setFont (11.0f);
        const int lblH = 14;
        const int lblY = (int) bounds.getBottom() - lblH - 1;
        // Below a second the window is now short enough that "-0.0 s" is all a
        // one-decimal seconds label can say — print milliseconds there instead.
        const juce::String startLbl = (ltWindowSec < 1.0f)
            ? "-" + juce::String (ltWindowSec * 1000.0f, 0) + " ms"
            : "-" + juce::String (ltWindowSec, ltWindowSec < 10.0f ? 1 : 0) + " s";
        g.drawText (startLbl,
                    (int) bounds.getX() + 3, lblY, 60, lblH, juce::Justification::centredLeft);
        g.drawText ("now", (int) bounds.getRight() - 44, lblY, 40, lblH,
                    juce::Justification::centredRight);
    }

    // ── Term 2: LONG WAVE (the raw waveform over a long window) ──────────────
    void paintLongWave (juce::Graphics& g, juce::Rectangle<float> bounds)
    {
        const int W       = juce::jmax (8, getWidth());
        const int total   = (int) rawL.size();              // capacity == window samples
        const int filledN = juce::jlimit (0, total, rawFilled);

        // Which pixel columns contained a sample at or beyond full scale. Filled by
        // drawWave (which is where the per-column values already exist — a second
        // pass over the ring to find the same thing would be pure duplication) and
        // consumed by drawClipZone below. Cleared per frame, ORed across channels:
        // in the overlaid modes both channels share one picture, so "something hit
        // full scale in this column" is the honest claim to mark.
        overCols.assign ((size_t) W, 0);

        if (filledN < 2 || total < 2)
        {
            g.setColour (activeColour().withAlpha (0.25f));
            g.fillRect (bounds.getX(), bounds.getCentreY() - 0.5f, bounds.getWidth(), 1.0f);
            drawTimeLabels (g, bounds);
            return;
        }

        // q is the oldest-first time coordinate over the full window [0,total);
        // q == total-1 is the newest sample (right edge). Columns whose q falls
        // before firstQ have no data yet (window was just enlarged) and are skipped,
        // so partially-filled buffers draw right-aligned and fill in over time.
        const int firstQ = total - filledN;

        auto valAt = [&] (int q, int ch) -> float
        {
            const int idx = (rawWrite + q) % total;
            const float l = rawL[(size_t) idx], r = rawR[(size_t) idx];
            return ch == 0 ? l : ch == 1 ? r : 0.5f * (l + r);
        };

        auto catmull = [] (float p0, float p1, float p2, float p3, float t)
        {
            return 0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t
                           + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
        };

        auto drawWave = [&] (juce::Rectangle<float> area, int ch, float alpha, juce::Colour col, bool mirror = false)
        {
            const float cy           = area.getCentreY();
            const float vScale       = area.getHeight() * kLongVScale;
            const float samplesPerPx = (float) total / (float) W;
            auto xAtPixel = [&] (int x) { return area.getX() + (float) x / (float) (W - 1) * area.getWidth(); };

            if (samplesPerPx >= 4.0f)
            {
                // DAW-style min/max band. Bins are anchored to ABSOLUTE sample
                // positions (not the moving write head), so a completed bin's
                // min/max never changes from frame to frame — the peaks stay rock
                // steady and only scroll, instead of vibrating. A light horizontal
                // smoothing then removes any residual moiré.
                const int       binSize        = juce::jmax (1, (int) std::lround (samplesPerPx));
                const long long newestBin      = (rawTotal - 1) / binSize;
                const long long oldestValidAbs = rawTotal - (long long) filledN;

                std::vector<float> mn ((size_t) W, 0.0f), mx ((size_t) W, 0.0f);
                std::vector<char>  has ((size_t) W, 0);

                for (int x = 0; x < W; ++x)
                {
                    const long long bin = newestBin - (long long) (W - 1 - x);
                    if (bin < 0) continue;
                    long long s0 = bin * (long long) binSize;
                    long long s1 = s0 + (long long) binSize;
                    s0 = juce::jmax (s0, oldestValidAbs);
                    s1 = juce::jmin (s1, rawTotal);
                    if (s0 >= s1) continue;              // bin not yet filled (right) or scrolled out (left)

                    float lo = 1.0e9f, hi = -1.0e9f;
                    for (long long s = s0; s < s1; ++s)
                    {
                        const long long delta = rawTotal - s;        // 1 .. filledN
                        const int idx = (int) (((rawWrite - delta) % total + total) % total);
                        const float v = (ch == 0 ? rawL[(size_t) idx]
                                       : ch == 1 ? rawR[(size_t) idx]
                                                 : 0.5f * (rawL[(size_t) idx] + rawR[(size_t) idx]));
                        lo = juce::jmin (lo, v);
                        hi = juce::jmax (hi, v);
                    }
                    mn[(size_t) x] = lo; mx[(size_t) x] = hi; has[(size_t) x] = 1;
                    // Before the jlimit below throws the overshoot away.
                    if (hi >= 1.0f || lo <= -1.0f) overCols[(size_t) x] = 1;
                }

                // smoothing strength scales with how zoomed-out we are
                const int r = juce::jlimit (1, 3, (int) std::round (std::log10 (samplesPerPx)));
                smoothEdges (mn, has, r);
                smoothEdges (mx, has, r);

                juce::Path band;
                bool started = false;
                for (int x = 0; x < W; ++x)
                {
                    if (! has[(size_t) x]) continue;
                    const float py = cy - juce::jlimit (-1.0f, 1.0f, mx[(size_t) x]) * vScale;
                    if (! started) { band.startNewSubPath (xAtPixel (x), py); started = true; }
                    else             band.lineTo (xAtPixel (x), py);
                }
                for (int x = W - 1; x >= 0; --x)
                {
                    if (! has[(size_t) x]) continue;
                    band.lineTo (xAtPixel (x), cy - juce::jlimit (-1.0f, 1.0f, mn[(size_t) x]) * vScale);
                }
                if (! started) return;
                band.closeSubPath();

                g.setColour (col.withAlpha ((showFill ? 0.55f : 0.40f) * alpha));
                g.fillPath (band);
                g.setColour (col.withAlpha (0.9f * alpha));
                g.strokePath (band, juce::PathStrokeType (1.0f));

                if (mirror)   // reflect the band about the centre line (symmetric look)
                {
                    juce::Path fb (band);
                    fb.applyTransform (juce::AffineTransform::scale (1.0f, -1.0f, 0.0f, cy));
                    g.setColour (col.withAlpha ((showFill ? 0.55f : 0.40f) * alpha));
                    g.fillPath (fb);
                    g.setColour (col.withAlpha (0.9f * alpha));
                    g.strokePath (fb, juce::PathStrokeType (1.0f));
                }
            }
            else
            {
                // zoomed in: smooth Catmull-Rom intersample curve (no stair-stepping)
                juce::Path p;
                bool started = false;
                for (int x = 0; x < W; ++x)
                {
                    const float fq = (float) x * samplesPerPx;
                    if (fq < (float) firstQ) continue;
                    const int   q1 = (int) fq;
                    const float t  = fq - (float) q1;
                    const int   q0 = juce::jmax (firstQ,    q1 - 1);
                    const int   q2 = juce::jmin (total - 1, q1 + 1);
                    const int   q3 = juce::jmin (total - 1, q1 + 2);
                    const float v  = catmull (valAt (q0, ch), valAt (q1, ch),
                                              valAt (q2, ch), valAt (q3, ch), t);
                    if (std::abs (v) >= 1.0f) overCols[(size_t) x] = 1;
                    const float px = xAtPixel (x);
                    const float py = cy - juce::jlimit (-1.0f, 1.0f, v) * vScale;
                    if (! started) { p.startNewSubPath (px, py); started = true; }
                    else             p.lineTo (px, py);
                }
                if (! started) return;

                if (showFill)
                {
                    juce::Path fillPath (p);
                    fillPath.lineTo (area.getRight(), cy);
                    fillPath.lineTo (area.getX(), cy);
                    fillPath.closeSubPath();
                    g.setColour (col.withAlpha (0.25f * alpha));
                    g.fillPath (fillPath);
                }
                g.setColour (col.withAlpha (alpha));
                g.strokePath (p, juce::PathStrokeType (2.0f));

                if (mirror)   // reflect the curve about the centre line (symmetric look)
                {
                    juce::Path fp (p);
                    fp.applyTransform (juce::AffineTransform::scale (1.0f, -1.0f, 0.0f, cy));
                    g.setColour (col.withAlpha (alpha));
                    g.strokePath (fp, juce::PathStrokeType (2.0f));
                }
            }
        };

        if (displayMode == DisplayMode::MirrorStereo)
        {
            auto top    = bounds.withHeight (bounds.getHeight() * 0.5f);
            auto bottom = top.translated (0.0f, top.getHeight());
            drawWave (top,    0, 1.0f,  activeColour());
            drawWave (bottom, 1, 0.75f, activeColour());
            // Each half is its own picture with its own centre line, so the clip
            // boundary is drawn per half — one pair across the whole module would
            // sit at ±0 dBFS of a scale neither channel is drawn against.
            drawClipZone (g, top);
            drawClipZone (g, bottom);
        }
        else if (displayMode == DisplayMode::Stereo)
        {
            // L and R overlaid on the same area, R in a distinct hue
            drawWave (bounds, 0, 1.0f,  activeColour());
            drawWave (bounds, 1, 0.85f, stereoRColour());
            drawClipZone (g, bounds);
        }
        else
        {
            // Mono, or Mirror (mono mix reflected about the centre line)
            drawWave (bounds, 2, 1.0f, activeColour(), displayMode == DisplayMode::Mirror);
            drawClipZone (g, bounds);
        }

        g.setColour (activeColour().withAlpha (0.25f));
        g.fillRect (bounds.getX(), bounds.getCentreY() - 0.5f, bounds.getWidth(), 1.0f);
        drawTimeLabels (g, bounds);
    }

    // ── CLIPPING ZONE (Long Waveform only) ───────────────────────────────────
    //
    //  This view draws raw amplitude against a fixed half-height, so 0 dBFS — full
    //  scale, the ceiling — is not an estimate: it is exactly ±kLongVScale from the
    //  centre line. Without it the view has no absolute reference at all, and a hot
    //  master and a quiet stem draw the same silhouette.    //
    //  THE TRACE IS CLAMPED TO FULL SCALE, ON PURPOSE. That is what a DAW does with a
    //  clip overview, and the flat plateau it produces is how an engineer reads "this
    //  is slammed" at a glance. Letting the trace run past the boundary would buy one
    //  number — HOW far over — at the cost of shrinking every waveform to leave room
    //  for the overshoot, and that number belongs on a meter: True Peak reports it in
    //  dBTP and also catches inter-sample peaks, which a sample-domain view cannot see
    //  at all.
    //
    //  What the clamp costs is that a column 6 dB over looks identical to one that
    //  merely touched. So the over-scale columns are MARKED instead — see
    //  drawOverMarks. Same information, no cost to the picture.
    //
    //  The sparse ruler down the right (0 / -6 / -12 / -20 = 1, ½, ¼, 1⁄10 of full
    //  scale) answers the other question the boundary raises: where is everything else.
    void drawClipZone (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        if (! showClipZone) return;
        if (area.getHeight() < 40.0f || area.getWidth() < 60.0f) return;

        const float cy     = area.getCentreY();
        const float vScale = area.getHeight() * kLongVScale;

        // THE MODULE'S OWN COLOUR, not a fixed red. The line is a SCALE MARK, not an
        // alarm — it says where 0 dBFS is whether or not anything is near it, and a
        // red rule burning across a violet waveform reads as a warning that is
        // permanently on. Drawn from the same colour as the trace (so it follows the
        // note in tone mode too), brightened and dashed so it stays legible against
        // the waveform it crosses without pretending to be part of it.
        const auto clipCol = activeColour().brighter (0.4f).withAlpha (0.75f);
        g.setColour (clipCol);
        for (int sign = -1; sign <= 1; sign += 2)
        {
            const float y = cy - (float) sign * vScale;
            // Dashed, so it never reads as part of the waveform it crosses.
            const float dash[] = { 5.0f, 4.0f };
            g.drawDashedLine ({ area.getX(), y, area.getRight(), y }, dash, 2, 1.0f);
        }

        // ── OVER-SCALE MARKS ────────────────────────────────────────────────
        //  The trace is clamped to full scale, so a column that went over draws as a
        //  flat top ON the boundary and looks identical to one that just touched it.
        //  This is what tells them apart: a solid red bar laid along the boundary
        //  exactly where the audio reached it, i.e. the clip overlay a DAW puts on a
        //  hot region. Red HERE and nowhere else in this module — the boundary line
        //  itself is a scale mark and stays the module's colour, so red never appears
        //  unless something actually hit the ceiling.
        drawOverMarks (g, area, cy, vScale);

        // Only the marks that have room: below ~46 px of half-height the labels
        // would touch, and four overlapping numbers say less than one clean one.
        static const float marks[] = { 0.0f, -6.0f, -12.0f, -20.0f };
        const int          nMarks  = (vScale > 46.0f) ? 4 : 1;
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        for (int i = 0; i < nMarks; ++i)
        {
            const float amp = std::pow (10.0f, marks[i] / 20.0f);   // dBFS → linear
            const float y   = cy - amp * vScale;
            const bool  isZero = (i == 0);
            g.setColour (isZero ? clipCol : juce::Colours::lightgrey.withAlpha (0.45f));
            if (! isZero)
                g.drawHorizontalLine ((int) std::round (y), area.getRight() - 6.0f, area.getRight());
            g.drawText (isZero ? juce::String ("0 dB") : juce::String ((int) marks[i]),
                        (int) area.getRight() - 42, (int) y + 1, 38, 11,
                        juce::Justification::centredRight, false);
        }
    }

    void timerCallback() override
    {
        if (AlterTheme::hudFrozen.load()) return;   // HUD "Hold": keep last frame

        // Tone tracking belongs HERE, not in paint(). paint() runs on repaints the
        // module did not ask for — a resize, an overlapping window, a Fusion pulling
        // a frame — and advancing the hue smoother on those would make the colour
        // chase the note at a rate that depends on what else is on screen. The timer
        // is the module's clock, so one update per tick is one update per frame.
        if (colourByTone)
            toneHue.update (audioSource, toneTwist, toneSmooth);

        // Capture the raw ring for the Long-wave view. (Level history moved to the
        // Audio Meter module, so the envelope ring is no longer captured here.)
        const int count = audioSource.getLastWaveform (ltWaveL, ltWaveR);
        captureRaw (count);
        repaint();
    }
};
