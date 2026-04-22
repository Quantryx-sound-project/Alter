#pragma once
#include <JuceHeader.h>
#include "AudioSourceInterface.h"

// Professional audio meter with dB scale (like in DAW)
// Supports multiple modes: RMS, True Peak, LUFS
// Renamed from VisualRmsBar -> VisualAudioMeter to better reflect functionality
class VisualAudioMeter : public juce::Component, private juce::Timer
{
public:
    enum class MeterMode
    {
        RMS,       // Root Mean Square (average energy)
        TruePeak,  // True Peak (maximum sample value)
        LUFS       // Loudness Units Full Scale (perceptual loudness, ITU-R BS.1770)
    };

    enum class ColorMode
    {
        Standard,       // Fixed DAW colors (green, yellow, orange, red)
        CustomGradient, // Gradient from user color (100%, 70%, 50%, 30%)
        CustomSpectrum  // Gradient from user color to adjacent color on color wheel
    };

    explicit VisualAudioMeter (IAudioSource& r) : audioSource (r)
    {
        setOpaque (true);
        startTimerHz (30);  // Update peak hold
    }

    ~VisualAudioMeter() override
    {
        // CRITICAL: Stop timer before destruction to avoid crash!
        stopTimer();
    }

    void setSmoothAmount (float s01)
    {
        smooth01 = juce::jlimit (0.0f, 1.0f, s01);

        if (meterMode == MeterMode::RMS)
        {
            alpha = juce::jmap (smooth01, 0.0f, 1.0f, 0.0f, 0.97f);
        }
        else if (meterMode == MeterMode::TruePeak)
        {
            releaseCoeff = juce::jmap (smooth01, 0.0f, 1.0f, 0.95f, 0.995f);
        }
        else // LUFS
        {
            alpha = juce::jmap (smooth01, 0.0f, 1.0f, 0.85f, 0.98f);
        }

        // Trend EMA: higher smooth01 = slower/smoother curve
        // Range: alpha 0.30 (smooth01=0, very reactive) .. 0.03 (smooth01=1, very smooth)
        trendAlpha = juce::jmap (smooth01, 0.0f, 1.0f, 0.30f, 0.03f);
    }

    // Set meter mode (RMS, True Peak, LUFS)
    void setMeterMode (MeterMode mode)
    {
        if (meterMode != mode)
        {
            meterMode = mode;
            smoothed = (mode == MeterMode::LUFS) ? -100.0f : 0.0f;
            peakHold = (mode == MeterMode::LUFS) ? -100.0f : 0.0f;
            setSmoothAmount (smooth01);
            repaint();
        }
    }

    // Set display view: false = Momentary (bar), true = Trend (waveform over time)
    void setTrendMode (bool enabled)
    {
        if (trendMode != enabled)
        {
            trendMode = enabled;
            if (trendMode)
                measureSmoothed = -60.0f;
            repaint();
        }
    }

    bool getTrendMode() const noexcept { return trendMode; }

    MeterMode getMeterMode() const { return meterMode; }

    // ── Integrated measurement control ──────────────────────────────────
    // Start a new measurement: clears buffer, reserves memory, begins capturing.
    void startMeasurement()
    {
        measurementBuffer.clear();
        measurementBuffer.reserve ((size_t) kMaxMeasurePoints);
        measureSmoothed = -60.0f;
        isMeasuring = true;
        repaint();
    }

    // Stop measurement: halts capturing but keeps buffer intact for display.
    void stopMeasurement()
    {
        isMeasuring = false;
        repaint();
    }

    bool isMeasurementActive()    const noexcept { return isMeasuring; }
    int  getMeasurementPointCount() const noexcept { return (int) measurementBuffer.size(); }
    // Returns elapsed seconds (based on 30 Hz timer)
    float getMeasurementElapsedSec() const noexcept { return (float) measurementBuffer.size() / 30.0f; }

    // Public getter for info display
    float getCurrentValue() const
    {
        if (meterMode == MeterMode::TruePeak)
            return audioSource.getLastPeak();
        if (meterMode == MeterMode::LUFS)
        {
            const float lufs = audioSource.getLastLufs();
            return (lufs <= -60.0f) ? 0.0f : std::pow (10.0f, lufs / 20.0f);
        }
        return audioSource.getLastRms();
    }

    // Get the smoothed value (matches what the bar actually displays)
    float getSmoothedValue() const noexcept { return smoothed; }

    // Set custom color (from controller)
    void setBarColour (juce::Colour c) { barColour = c; repaint(); }

    // Set custom color mode (0=Standard, 1=Gradient, 2=Spectrum)
    void setCustomColorMode (bool useCustom) { colorMode = useCustom ? ColorMode::CustomGradient : ColorMode::Standard; repaint(); }

    // Set color mode (Standard / CustomGradient / CustomSpectrum)
    void setColorMode (ColorMode mode) { colorMode = mode; repaint(); }

    // Tell the meter what rotation the host is applying (so scale text can be skipped when rotated)
    void setHostRotation (int r) { hostRotation = juce::jlimit (0, 3, r); repaint(); }
    int  getHostRotation() const noexcept { return hostRotation; }

    // Expose scale data so PanelHost can draw labels in host space (always horizontal)
    struct ScaleInfo
    {
        float minDb, maxDb;
        std::vector<float> checkpoints;
    };

    ScaleInfo getScaleInfo() const
    {
        ScaleInfo info;
        info.minDb = -60.0f;

        if (meterMode == MeterMode::RMS)
        {
            info.maxDb = 0.0f;
            info.checkpoints = { -60.0f, -48.0f, -36.0f, -24.0f, -18.0f, -12.0f, -9.0f, -6.0f, -3.0f, 0.0f };
        }
        else if (meterMode == MeterMode::TruePeak)
        {
            info.maxDb = +6.0f;
            info.checkpoints = { -60.0f, -24.0f, -18.0f, -12.0f, -9.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f };
        }
        else // LUFS
        {
            info.maxDb = +6.0f;
            info.checkpoints = { -60.0f, -36.0f, -23.0f, -18.0f, -14.0f, -9.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f };
        }
        return info;
    }

    // Getters pre PanelHost strip
    float getSmoothedDb()   const noexcept { return smoothedDbCache; }
    float getSessionMaxDb() const noexcept { return sessionMaxDb; }

    juce::String getStripText() const noexcept { return stripText; }

    float getMinDb() const noexcept { return -60.0f; }
    float getMaxDb() const noexcept { return (meterMode == MeterMode::RMS) ? 0.0f : 6.0f; }

    std::vector<float> getCheckpoints() const
    {
        if (meterMode == MeterMode::RMS)
            return { -60.0f, -48.0f, -36.0f, -24.0f, -18.0f, -12.0f, -9.0f, -6.0f, -3.0f, 0.0f };
        else if (meterMode == MeterMode::TruePeak)
            return { -60.0f, -24.0f, -18.0f, -12.0f, -9.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f };
        else // LUFS
            return { -60.0f, -36.0f, -23.0f, -18.0f, -14.0f, -9.0f, -6.0f, -38.0f, -14.0f, -9.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f };
    }

    void timerCallback() override
    {
        // --- Read input ---
        float incoming;
        bool incomingIsDb = false;
        if (meterMode == MeterMode::TruePeak)
            incoming = audioSource.getLastPeak();
        else if (meterMode == MeterMode::LUFS)
        { incoming = audioSource.getLastLufs(); incomingIsDb = true; }
        else
            incoming = audioSource.getLastRms();

        const float safeIncoming = incomingIsDb
            ? juce::jlimit (-100.0f, 6.0f, incoming)
            : juce::jlimit (0.0f, 10.0f, incoming);

        // Smoothing (for bar animation only)
        if (meterMode == MeterMode::RMS)
            smoothed = smoothed * alpha + safeIncoming * (1.0f - alpha);
        else if (meterMode == MeterMode::TruePeak)
        {
            if (safeIncoming > smoothed) smoothed = safeIncoming;
            else smoothed = smoothed * releaseCoeff + safeIncoming * (1.0f - releaseCoeff);
        }
        else
            smoothed = smoothed * alpha + safeIncoming * (1.0f - alpha);

        // Peak hold (all modes)
        if (smoothed > peakHold) { peakHold = smoothed; peakHoldTime = 0.0f; }

        float holdTime = (meterMode == MeterMode::RMS) ? 2.0f : 3.0f;
        peakHoldTime += 1.0f / 30.0f;
        if (peakHoldTime > holdTime)
        {
            float decayFactor = 0.98f;
            if (meterMode == MeterMode::TruePeak) decayFactor = 0.985f;
            else if (meterMode == MeterMode::LUFS) decayFactor = 0.99f;
            peakHold *= decayFactor;
            if (peakHold < 0.001f) peakHold = 0.0f;
        }

        // Cache dB value for strip
        smoothedDbCache = incomingIsDb ? smoothed : linearToDb (smoothed);
        if (smoothedDbCache > sessionMaxDb)
            sessionMaxDb = smoothedDbCache;

        // Update strip text
        bool isLufs = (meterMode == MeterMode::LUFS);
        if (meterMode == MeterMode::TruePeak)
        {
            if (smoothedDbCache <= -59.0f)
                stripText = juce::String ("-inf dBTP");
            else
                stripText = juce::String (smoothedDbCache, 1) + " dBTP";
        }
        else if (smoothedDbCache <= -59.0f)
            stripText = isLufs ? juce::String ("-inf LUFS") : juce::String ("-inf dBFS");
        else
            stripText = juce::String (smoothedDbCache, 1) + (isLufs ? " LUFS" : " dBFS");

        // ── Trend capture (works for any MeterMode – records smoothedDbCache over time) ──
        if (trendMode && isMeasuring)
        {
            measureSmoothed = measureSmoothed * (1.0f - trendAlpha)
                            + smoothedDbCache * trendAlpha;
            measurementBuffer.push_back (measureSmoothed);

            const int n = (int) measurementBuffer.size();
            const float elapsed = (float) n / 30.0f;
            const int mins  = (int) (elapsed / 60.0f);
            const int secs  = (int) elapsed % 60;
            char buf[40];
            std::snprintf (buf, sizeof (buf), "REC %02d:%02d  %d pts", mins, secs, n);
            stripText = juce::String (buf);

            if (n >= kMaxMeasurePoints)
            {
                isMeasuring = false;  // auto-stop at 30 min
                char buf2[32];
                std::snprintf (buf2, sizeof (buf2), "DONE  %d pts", n);
                stripText = juce::String (buf2);
            }
        }
        else if (trendMode && !isMeasuring)
        {
            const int n = (int) measurementBuffer.size();
            if (n > 0)
            {
                const float elapsed = (float) n / 30.0f;
                const int mins = (int) (elapsed / 60.0f);
                const int secs = (int) elapsed % 60;
                char buf[40];
                std::snprintf (buf, sizeof (buf), "STOP  %02d:%02d  %d pts", mins, secs, n);
                stripText = juce::String (buf);
            }
            else
            {
                stripText = "Press Start";
            }
        }

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        g.fillAll (juce::Colours::black);

        const int W = bounds.getWidth();
        const int H = bounds.getHeight();
        if (W <= 0 || H <= 0) return;

        const float fontSize = juce::jlimit (8.0f, 13.0f, (float) juce::jmin (W, H) * 0.06f);
        const float scaleFontSz = juce::jlimit (7.0f, 10.0f, fontSize * 0.85f);

        // --- BOTTOM STRIP: live value text ---
        const int stripH = juce::jlimit (14, 20, (int)(fontSize * 1.6f));
        auto stripRect = bounds.removeFromBottom (stripH);

        g.setColour (juce::Colour (0xFF111111));
        g.fillRect (stripRect);
        g.setColour (juce::Colour (0xFF333333));
        g.drawLine ((float) stripRect.getX(), (float) stripRect.getY(),
                    (float) stripRect.getRight(), (float) stripRect.getY(), 1.0f);

        g.setFont (juce::Font (juce::FontOptions (fontSize).withStyle ("Bold")));
        g.setColour (juce::Colours::white);
        g.drawText (stripText, stripRect.reduced (4, 0),
                    juce::Justification::centred, false);

        // --- BAR AREA: vertical, bottom = minDb, top = maxDb ---
        auto barArea = bounds.reduced (2);
        if (barArea.isEmpty()) return;

        const bool incomingIsDb = (meterMode == MeterMode::LUFS);
        const float smoothedDb = incomingIsDb ? smoothed : linearToDb (smoothed);

        const float minDb = getMinDb();
        const float maxDb = getMaxDb();

        auto normalizeDb = [minDb, maxDb](float db) -> float {
            return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
        };

        // Vertical bar: level fills from bottom upward
        const int barH = barArea.getHeight();

        g.saveState();
        g.reduceClipRegion (barArea);

        // Background (unfilled portion)
        g.setColour (juce::Colour (0xFF0A0A0A));
        g.fillRect (barArea);

        if (trendMode)
        {
            renderTrendVertical (g, barArea, normalizeDb);
        }
        else
        {
            if (meterMode == MeterMode::RMS)
                renderRmsVertical (g, barArea, smoothedDb, normalizeDb);
            else if (meterMode == MeterMode::TruePeak)
                renderPeakVertical (g, barArea, smoothedDb, normalizeDb);
            else
                renderLufsVertical (g, barArea, smoothedDb, normalizeDb);
        }

        // --- PEAK HOLD marker (horizontal line showing max peak) ---
        // Skip in Trend view (irrelevant)
        if (!trendMode)
        {
            const float phDb = (meterMode == MeterMode::LUFS) ? peakHold : linearToDb (peakHold);
            const float phNorm = normalizeDb (phDb);
            if (phNorm > 0.001f && phNorm < 1.0f)
            {
                const float phY = (float) barArea.getBottom() - phNorm * (float) barH;
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawHorizontalLine ((int) std::round (phY),
                                      (float) barArea.getX(),
                                      (float) barArea.getRight());
            }
        }

        // --- SCALE: horizontal tick lines + labels inside bar (responsive) ---
        const auto checkpoints = getCheckpoints();
        const float lblH = scaleFontSz + 2.0f;
        g.setFont (juce::Font (juce::FontOptions (scaleFontSz)));

        // Pre-compute Y positions for all checkpoints
        struct LabelInfo { float db; float yPos; };
        std::vector<LabelInfo> labels;
        for (float db : checkpoints)
        {
            if (db < minDb || db > maxDb) continue;
            float norm = normalizeDb (db);
            float yPos = (float) barArea.getBottom() - norm * (float) barH;
            labels.push_back ({ db, yPos });
        }

        // Draw tick lines always
        for (auto& li : labels)
        {
            g.setColour (juce::Colours::white.withAlpha (li.db == 0.0f ? 0.4f : 0.15f));
            g.drawHorizontalLine ((int) std::round (li.yPos),
                                  (float) barArea.getX(),
                                  (float) barArea.getRight());
        }

        // Draw labels only where they don't overlap (skip if too close to neighbors)
        // Two passes: 1) mark which labels fit, 2) draw them
        // Priority: 0 dBFS always drawn first, then outward
        {
            const int n = (int) labels.size();
            std::vector<bool> draw (n, false);

            // First pass: mark 0 dBFS (or closest to it) as always drawn
            int zeroIdx = -1;
            for (int i = 0; i < n; ++i)
                if (std::abs (labels[i].db) < 0.01f) { zeroIdx = i; draw[i] = true; break; }

            // Second pass: greedily add labels that don't overlap with already-drawn ones
            auto canFit = [&](int idx) -> bool {
                for (int j = 0; j < n; ++j)
                    if (draw[j] && std::abs (labels[idx].yPos - labels[j].yPos) < lblH * 0.7f)
                        return false;
                return true;
            };

            // Priority: musically important thresholds first
            std::vector<float> priority = { 0.0f, -3.0f, -6.0f, -9.0f, -12.0f,
                                            3.0f, 6.0f, -14.0f, -18.0f, -23.0f,
                                            -24.0f, -36.0f, -48.0f, -60.0f };
            for (float pDb : priority)
            {
                for (int i = 0; i < n; ++i)
                {
                    if (!draw[i] && std::abs (labels[i].db - pDb) < 0.01f && canFit (i))
                    {
                        draw[i] = true;
                        break;
                    }
                }
            }

            for (int i = 0; i < n; ++i)
            {
                if (!draw[i]) continue;

                juce::String lbl;
                if      (labels[i].db == 0.0f) lbl = "0";
                else if (labels[i].db == 3.0f) lbl = "+3";
                else if (labels[i].db == 6.0f) lbl = "+6";
                else                           lbl = juce::String ((int) labels[i].db);

                g.setColour (juce::Colours::white.withAlpha (0.7f));
                g.drawText (lbl,
                            barArea.getX() + 2, (int) std::round (labels[i].yPos) - (int)(lblH / 2),
                            (int)(scaleFontSz * 4.5f), (int) lblH,
                            juce::Justification::centredLeft, false);
            }
        }

        g.restoreState();

        // Border
        g.setColour (juce::Colours::darkgrey);
        g.drawRect (getLocalBounds(), 1);
    }

private:
    void renderRmsMode (juce::Graphics& g, juce::Rectangle<int> r, int levelWidth, int zeroWidth)
    {
        // RMS EDM zones: Green(-60 to -12), Yellow(-12 to -6), Orange(-6 to 0)
        // No CLIP indicator in RMS mode

        const int barWidth  = r.getWidth();
        const float minDb   = -60.0f;
        const float maxDb   =   0.0f;

        auto dbToX = [&](float db) -> int
        {
            return (int) juce::jlimit (0.0f, (float) barWidth,
                ((db - minDb) / (maxDb - minDb)) * (float) barWidth);
        };

        const int x_m12 = dbToX (-12.0f);
        const int x_m6  = dbToX (-6.0f);

        if (levelWidth > 0)
        {
            if (colorMode != ColorMode::Standard)
            {
                const int safeW = juce::jmin (levelWidth, x_m6);
                if (safeW > 0) { g.setColour (barColour); g.fillRect (r.withWidth (safeW)); }
                if (levelWidth > x_m6)
                {
                    const int ow = levelWidth - x_m6;
                    g.setColour (colorMode == ColorMode::CustomGradient
                                 ? barColour.withBrightness (0.45f)
                                 : getAdjacentColor (barColour, 0.4f));
                    g.fillRect (r.withX (r.getX() + x_m6).withWidth (ow));
                }
            }
            else
            {
                // Zone 1: Green (0 .. -12)
                {
                    const int w = juce::jmin (levelWidth, x_m12);
                    if (w > 0) { g.setColour (juce::Colours::limegreen); g.fillRect (r.withWidth (w)); }
                }
                // Zone 2: Yellow (-12 .. -6)
                if (levelWidth > x_m12)
                {
                    const int w = juce::jmin (levelWidth - x_m12, x_m6 - x_m12);
                    if (w > 0) { g.setColour (juce::Colour (0xFFFFDD00)); g.fillRect (r.withX (r.getX() + x_m12).withWidth (w)); }
                }
                // Zone 3: Orange (-6 .. 0)
                if (levelWidth > x_m6)
                {
                    const int w = levelWidth - x_m6;
                    if (w > 0) { g.setColour (juce::Colours::orange); g.fillRect (r.withX (r.getX() + x_m6).withWidth (w)); }
                }
            }
        }
    }

    void renderPeakMode (juce::Graphics& g, juce::Rectangle<int> r, int levelWidth, int barWidth,
                        std::function<float(float)> normalizeDb)
    {
        // Peak mode: multi-zone coloring based on color mode
        const float minus3Db = -3.0f;
        const float normalizedMinus3 = normalizeDb (minus3Db);
        const int yellowStart = (int) (normalizedMinus3 * barWidth);

        if (levelWidth > 0)
        {
            // Part 1: Below -3 dBFS (safe zone - full brightness)
            const int greenWidth = juce::jmin (levelWidth, yellowStart);
            if (greenWidth > 0)
            {
                auto greenFill = r.withWidth (greenWidth);
                g.setColour (barColour);
                g.fillRect (greenFill);
            }

            // Part 2: -3 dBFS to 0 dBFS (warning zone)
            if (levelWidth > yellowStart)
            {
                const float normalizedZero = normalizeDb (0.0f);
                const int zeroStart = (int) (normalizedZero * barWidth);
                const int yellowWidth = juce::jmin (levelWidth - yellowStart, zeroStart - yellowStart);

                if (yellowWidth > 0)
                {
                    auto yellowFill = r.withX (r.getX() + yellowStart).withWidth (yellowWidth);

                    juce::Colour warningColour;
                    if (colorMode == ColorMode::Standard)
                        warningColour = juce::Colours::yellow.withAlpha (0.9f);
                    else if (colorMode == ColorMode::CustomGradient)
                        warningColour = barColour.withBrightness (0.7f);
                    else // CustomSpectrum
                        warningColour = getAdjacentColor (barColour, 0.7f);

                    g.setColour (warningColour);
                    g.fillRect (yellowFill);
                }

                // Part 3: Above 0 dBFS (CLIPPING)
                if (levelWidth > zeroStart)
                {
                    const int clipWidth = levelWidth - zeroStart;
                    auto clipFill = r.withX (r.getX() + zeroStart).withWidth (clipWidth);

                    juce::Colour clipColour;
                    if (colorMode == ColorMode::Standard)
                        clipColour = juce::Colours::red.withAlpha (0.95f);
                    else if (colorMode == ColorMode::CustomGradient)
                        clipColour = barColour.withBrightness (0.3f);
                    else // CustomSpectrum
                        clipColour = getAdjacentColor (barColour, 0.3f);

                    g.setColour (clipColour);
                    g.fillRect (clipFill);
                }
            }
        }
    }

    void renderLufsMode (juce::Graphics& g, juce::Rectangle<int> r, int levelWidth, int barWidth,
                        std::function<float(float)> normalizeDb)
    {
        // LUFS mode: 4-zone coloring based on color mode
        const float target23 = -23.0f;  // EBU R128 broadcast target
        const float spotify14 = -14.0f; // Spotify normalization target
        const float loud9 = -9.0f;      // Very loud (loudness war territory)

        const float normalized23 = normalizeDb (target23);
        const float normalized14 = normalizeDb (spotify14);
        const float normalized9 = normalizeDb (loud9);

        const int target23Start = (int) (normalized23 * barWidth);
        const int spotify14Start = (int) (normalized14 * barWidth);
        const int loud9Start = (int) (normalized9 * barWidth);

        if (levelWidth > 0)
        {
            // Part 1: Below -23 LUFS (safe zone)
            const int zone1Width = juce::jmin (levelWidth, target23Start);
            if (zone1Width > 0)
            {
                auto fill = r.withWidth (zone1Width);
                g.setColour (barColour);  // Full brightness
                g.fillRect (fill);
            }

            // Part 2: -23 to -14 LUFS (streaming zone)
            if (levelWidth > target23Start)
            {
                const int zone2Width = juce::jmin (levelWidth - target23Start, spotify14Start - target23Start);
                if (zone2Width > 0)
                {
                    auto fill = r.withX (r.getX() + target23Start).withWidth (zone2Width);

                    juce::Colour colour;
                    if (colorMode == ColorMode::Standard)
                        colour = juce::Colours::yellow.withAlpha (0.9f);
                    else if (colorMode == ColorMode::CustomGradient)
                        colour = barColour.withBrightness (0.7f);
                    else // CustomSpectrum
                        colour = getAdjacentColor (barColour, 0.7f);

                    g.setColour (colour);
                    g.fillRect (fill);
                }

                // Part 3: -14 to -9 LUFS (hot zone)
                if (levelWidth > spotify14Start)
                {
                    const int zone3Width = juce::jmin (levelWidth - spotify14Start, loud9Start - spotify14Start);
                    if (zone3Width > 0)
                    {
                        auto fill = r.withX (r.getX() + spotify14Start).withWidth (zone3Width);

                        juce::Colour colour;
                        if (colorMode == ColorMode::Standard)
                            colour = juce::Colours::orange.withAlpha (0.9f);
                        else if (colorMode == ColorMode::CustomGradient)
                            colour = barColour.withBrightness (0.5f);
                        else // CustomSpectrum
                            colour = getAdjacentColor (barColour, 0.5f);

                        g.setColour (colour);
                        g.fillRect (fill);
                    }

                    // Part 4: Above -9 LUFS (loudness war)
                    if (levelWidth > loud9Start)
                    {
                        const int zone4Width = levelWidth - loud9Start;
                        auto fill = r.withX (r.getX() + loud9Start).withWidth (zone4Width);

                        juce::Colour colour;
                        if (colorMode == ColorMode::Standard)
                            colour = juce::Colours::red.withAlpha (0.95f);
                        else if (colorMode == ColorMode::CustomGradient)
                            colour = barColour.withBrightness (0.3f);
                        else // CustomSpectrum
                            colour = getAdjacentColor (barColour, 0.3f);

                        g.setColour (colour);
                        g.fillRect (fill);
                    }
                }
            }
        }
    }

    // Helper: Get adjacent color on color wheel (for CustomSpectrum mode)
    juce::Colour getAdjacentColor (juce::Colour baseColour, float position)
    {
        float hue = baseColour.getHue();
        const float sat = baseColour.getSaturation();
        const float brightness = baseColour.getBrightness();
        const float targetHue = std::fmod (hue + 0.166f, 1.0f);
        float interpolatedHue = hue + (targetHue - hue) * (1.0f - position);
        if (interpolatedHue < 0.0f) interpolatedHue += 1.0f;
        if (interpolatedHue > 1.0f) interpolatedHue -= 1.0f;
        return juce::Colour::fromHSV (interpolatedHue, sat, brightness, baseColour.getAlpha());
    }

    // ── Vertical render helpers (bottom-to-top) ────────────────────────

    void renderRmsVertical (juce::Graphics& g, juce::Rectangle<int> r,
                            float smoothedDb, std::function<float(float)> normalizeDb)
    {
        const int barH = r.getHeight();

        auto dbToY = [&](float db) -> int {
            float norm = normalizeDb (db);
            return r.getBottom() - (int)(norm * barH);
        };

        const int y_m12 = dbToY (-12.0f);
        const int y_m6  = dbToY (-6.0f);
        const int y_0   = dbToY (0.0f);

        const float levelNorm = normalizeDb (smoothedDb);
        const int fillBottom = r.getBottom();
        const int fillTop = r.getBottom() - (int)(levelNorm * barH);

        if (fillTop < fillBottom)
        {
            if (colorMode != ColorMode::Standard)
            {
                const int safeY = dbToY (-6.0f);
                int belowY = juce::jmax (fillTop, safeY);
                if (belowY < fillBottom)
                {
                    g.setColour (barColour);
                    g.fillRect (r.getX(), belowY, r.getWidth(), fillBottom - belowY);
                }
                if (fillTop < safeY)
                {
                    g.setColour (colorMode == ColorMode::CustomGradient
                                 ? barColour.withBrightness (0.45f)
                                 : getAdjacentColor (barColour, 0.4f));
                    g.fillRect (r.getX(), fillTop, r.getWidth(), safeY - fillTop);
                }
            }
            else
            {
                // Green zone: -60 to -12
                {
                    int top = juce::jmax (fillTop, y_m12);
                    if (top < fillBottom)
                    {
                        g.setColour (juce::Colours::limegreen);
                        g.fillRect (r.getX(), top, r.getWidth(), fillBottom - top);
                    }
                }
                // Yellow zone: -12 to -6
                if (fillTop < y_m12)
                {
                    int top = juce::jmax (fillTop, y_m6);
                    int bot = y_m12;
                    if (top < bot)
                    {
                        g.setColour (juce::Colour (0xFFFFDD00));
                        g.fillRect (r.getX(), top, r.getWidth(), bot - top);
                    }
                }
                // Orange zone: -6 to 0
                if (fillTop < y_m6)
                {
                    int top = juce::jmax (fillTop, y_0);
                    int bot = y_m6;
                    if (top < bot)
                    {
                        g.setColour (juce::Colours::orange);
                        g.fillRect (r.getX(), top, r.getWidth(), bot - top);
                    }
                }
            }
        }
    }

    void renderPeakVertical (juce::Graphics& g, juce::Rectangle<int> r,
                             float smoothedDb, std::function<float(float)> normalizeDb)
    {
        const int barH = r.getHeight();
        const float levelNorm = normalizeDb (smoothedDb);
        const int fillTop = r.getBottom() - (int)(levelNorm * barH);

        const float m3norm = normalizeDb (-3.0f);
        const int y_m3 = r.getBottom() - (int)(m3norm * barH);
        const float zeroNorm = normalizeDb (0.0f);
        const int y_0 = r.getBottom() - (int)(zeroNorm * barH);

        if (fillTop < r.getBottom())
        {
            // Below -3: safe
            int safeTop = juce::jmax (fillTop, y_m3);
            if (safeTop < r.getBottom())
            {
                g.setColour (barColour);
                g.fillRect (r.getX(), safeTop, r.getWidth(), r.getBottom() - safeTop);
            }
            // -3 to 0: warning
            if (fillTop < y_m3)
            {
                int wTop = juce::jmax (fillTop, y_0);
                if (wTop < y_m3)
                {
                    juce::Colour wc = (colorMode == ColorMode::Standard) ? juce::Colours::yellow.withAlpha (0.9f)
                                     : barColour.withBrightness (0.7f);
                    g.setColour (wc);
                    g.fillRect (r.getX(), wTop, r.getWidth(), y_m3 - wTop);
                }
            }
            // Above 0: clip
            if (fillTop < y_0)
            {
                juce::Colour cc = (colorMode == ColorMode::Standard) ? juce::Colours::red.withAlpha (0.95f)
                                 : barColour.withBrightness (0.3f);
                g.setColour (cc);
                g.fillRect (r.getX(), fillTop, r.getWidth(), y_0 - fillTop);
            }
        }
    }

    void renderLufsVertical (juce::Graphics& g, juce::Rectangle<int> r,
                             float smoothedDb, std::function<float(float)> normalizeDb)    {
        const int barH = r.getHeight();
        const float levelNorm = normalizeDb (smoothedDb);
        const int fillTop = r.getBottom() - (int)(levelNorm * barH);

        const int y_23 = r.getBottom() - (int)(normalizeDb (-23.0f) * barH);
        const int y_14 = r.getBottom() - (int)(normalizeDb (-14.0f) * barH);
        const int y_9  = r.getBottom() - (int)(normalizeDb (-9.0f)  * barH);

        if (fillTop < r.getBottom())
        {
            // Below -23: safe
            int top = juce::jmax (fillTop, y_23);
            if (top < r.getBottom())
            {
                g.setColour (barColour);
                g.fillRect (r.getX(), top, r.getWidth(), r.getBottom() - top);
            }
            // -23 to -14
            if (fillTop < y_23)
            {
                int t = juce::jmax (fillTop, y_14);
                if (t < y_23)
                {
                    juce::Colour c = (colorMode == ColorMode::Standard) ? juce::Colours::yellow.withAlpha (0.9f)
                                   : barColour.withBrightness (0.7f);
                    g.setColour (c);
                    g.fillRect (r.getX(), t, r.getWidth(), y_23 - t);
                }
            }
            // -14 to -9
            if (fillTop < y_14)
            {
                int t = juce::jmax (fillTop, y_9);
                if (t < y_14)
                {
                    juce::Colour c = (colorMode == ColorMode::Standard) ? juce::Colours::orange.withAlpha (0.9f)
                                   : barColour.withBrightness (0.5f);
                    g.setColour (c);
                    g.fillRect (r.getX(), t, r.getWidth(), y_14 - t);
                }
            }
            // Above -9
            if (fillTop < y_9)
            {
                juce::Colour c = (colorMode == ColorMode::Standard) ? juce::Colours::red.withAlpha (0.95f)
                               : barColour.withBrightness (0.3f);
                g.setColour (c);
                g.fillRect (r.getX(), fillTop, r.getWidth(), y_9 - fillTop);
            }
        }
    }

    // ── Trend view: waveform of dB over time, zone-colored ─────────────
    void renderTrendVertical (juce::Graphics& g, juce::Rectangle<int> r,
                              std::function<float(float)> normalizeDb)
    {
        const int W = r.getWidth();
        const int H = r.getHeight();
        const int n = (int) measurementBuffer.size();

        if (n == 0)
        {
            g.setColour (juce::Colours::grey.withAlpha (0.5f));
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText ("Press Start", r, juce::Justification::centred, false);
            return;
        }

        // Helper: pick zone colour for a given dB value (mirrors bar renderers)
        auto zoneColour = [this](float db) -> juce::Colour
        {
            if (meterMode == MeterMode::RMS)
            {
                if (colorMode != ColorMode::Standard)
                    return db > -6.0f ? barColour.withBrightness (0.45f) : barColour;
                if (db > -6.0f)  return juce::Colours::orange;
                if (db > -12.0f) return juce::Colour (0xFFFFDD00);
                return juce::Colours::limegreen;
            }
            else if (meterMode == MeterMode::TruePeak)
            {
                if (colorMode != ColorMode::Standard)
                {
                    if (db > 0.0f)  return barColour.withBrightness (0.3f);
                    if (db > -3.0f) return barColour.withBrightness (0.7f);
                    return barColour;
                }
                if (db > 0.0f)  return juce::Colours::red;
                if (db > -3.0f) return juce::Colours::yellow;
                return barColour;
            }
            else // LUFS
            {
                if (colorMode != ColorMode::Standard)
                {
                    if (db > -9.0f)  return barColour.withBrightness (0.3f);
                    if (db > -14.0f) return barColour.withBrightness (0.5f);
                    if (db > -23.0f) return barColour.withBrightness (0.7f);
                    return barColour;
                }
                if (db > -9.0f)  return juce::Colours::red;
                if (db > -14.0f) return juce::Colours::orange;
                if (db > -23.0f) return juce::Colour (0xFFFFDD00);
                return barColour;
            }
        };

        const float xScale = (float) W / (float) juce::jmax (n - 1, 1);
        const float lineAlpha = isMeasuring ? 0.92f : 0.65f;

        // Draw zone-coloured fill segments and stroke in one pass.
        // To avoid massive path count, batch consecutive points of same zone.
        auto getX = [&](int i) { return (float) r.getX() + (float) i * xScale; };
        auto getY = [&](int i)
        {
            float norm = normalizeDb (measurementBuffer[i]);
            return (float) r.getBottom() - norm * (float) H;
        };

        // --- filled area: single semi-transparent fill using dominant colour ---
        // (full fill in one colour avoids z-order artefacts; keep it subtle)
        {
            juce::Path fillPath;
            fillPath.startNewSubPath (getX (0), (float) r.getBottom());
            fillPath.lineTo          (getX (0), getY (0));
            for (int i = 1; i < n; ++i)
                fillPath.lineTo (getX (i), getY (i));
            fillPath.lineTo (getX (n - 1), (float) r.getBottom());
            fillPath.closeSubPath();
            g.setColour (barColour.withAlpha (0.12f));
            g.fillPath (fillPath);
        }

        // --- zone-coloured stroke: split into segments per colour change ---
        {
            juce::Path seg;
            juce::Colour curColour = zoneColour (measurementBuffer[0]);
            seg.startNewSubPath (getX (0), getY (0));

            for (int i = 1; i < n; ++i)
            {
                juce::Colour c = zoneColour (measurementBuffer[i]);
                if (c != curColour)
                {
                    // Flush current segment
                    g.setColour (curColour.withAlpha (lineAlpha));
                    g.strokePath (seg, juce::PathStrokeType (1.8f,
                        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    // Start new segment from the last point (no gap)
                    seg.clear();
                    seg.startNewSubPath (getX (i - 1), getY (i - 1));
                    curColour = c;
                }
                seg.lineTo (getX (i), getY (i));
            }
            // Flush last segment
            g.setColour (curColour.withAlpha (lineAlpha));
            g.strokePath (seg, juce::PathStrokeType (1.8f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Recording indicator dot (top-right corner)
        if (isMeasuring)
        {
            const float dotR = 4.0f;
            g.setColour (juce::Colours::red.withAlpha (0.85f));
            g.fillEllipse ((float) r.getRight() - dotR * 2.5f,
                           (float) r.getY() + dotR * 0.5f,
                           dotR * 2.0f, dotR * 2.0f);
        }
    }

    // Convert linear amplitude (0..1+) to dB
    static float linearToDb (float linear)
    {
        if (linear <= 0.00001f) return -60.0f;
        return juce::jlimit (-60.0f, 60.0f, 20.0f * std::log10 (linear));
    }

    void mouseUp (const juce::MouseEvent&) override {}

    IAudioSource& audioSource;

    MeterMode meterMode { MeterMode::RMS };
    ColorMode colorMode { ColorMode::Standard };

    float smooth01 { 0.0f };
    float alpha { 0.0f };
    float releaseCoeff { 0.9f };
    float smoothed { 0.0f };
    float smoothedDbCache { -60.0f };

    float peakHold { 0.0f };
    float peakHoldTime { 0.0f };

    float sessionMaxDb { -999.0f };

    juce::String stripText { "-inf dBFS" };

    juce::Colour barColour { juce::Colours::limegreen };
    int hostRotation { 0 };

    // ── Trend view + measurement ────────────────────────────────────────
    static constexpr int kMaxMeasurePoints = 54000;  // 30 min @ 30 Hz

    bool               trendMode       { false };   // Momentary=false, Trend=true
    std::vector<float> measurementBuffer;
    bool               isMeasuring     { false };
    float              measureSmoothed { -60.0f };
    float              trendAlpha      { 0.15f };   // EMA – derived from smooth01
};

// Backwards compatibility: allow old code to still use the name VisualRmsBar
using VisualRmsBar = VisualAudioMeter;
