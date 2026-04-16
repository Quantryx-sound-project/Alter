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
            // RMS: smooth both attack and release
            alpha = juce::jmap (smooth01, 0.0f, 1.0f, 0.0f, 0.97f);
        }
        else if (meterMode == MeterMode::TruePeak)
        {
            // Peak: fast attack, variable release
            releaseCoeff = juce::jmap (smooth01, 0.0f, 1.0f, 0.7f, 0.97f);
        }
        else // LUFS
        {
            // LUFS: slow integration (400ms-3s window)
            alpha = juce::jmap (smooth01, 0.0f, 1.0f, 0.85f, 0.98f);
        }
    }

    // Set meter mode (RMS or True Peak)
    void setMeterMode (MeterMode mode)
    {
        if (meterMode != mode)
        {
            meterMode = mode;
            smoothed = (mode == MeterMode::LUFS) ? -100.0f : 0.0f;
            peakHold = (mode == MeterMode::LUFS) ? -100.0f : 0.0f;
            setSmoothAmount (smooth01);  // Recalculate coefficients
            repaint();
        }
    }

    MeterMode getMeterMode() const { return meterMode; }

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
            info.maxDb = +6.0f;
            info.checkpoints = { -60.0f, -18.0f, -12.0f, -6.0f, 0.0f };
        }
        else if (meterMode == MeterMode::TruePeak)
        {
            info.maxDb = +3.0f;
            info.checkpoints = { -60.0f, -12.0f, -6.0f, -3.0f, 0.0f };
        }
        else // LUFS
        {
            info.maxDb = 0.0f;
            info.checkpoints = { -60.0f, -23.0f, -14.0f, -9.0f, 0.0f };
        }
        return info;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);

        // Draw full border first
        g.setColour (juce::Colours::darkgrey);
        g.drawRect (getLocalBounds(), 1);

        // Get mode-specific input
        float incoming;
        bool incomingIsDb = false;

        if (meterMode == MeterMode::TruePeak)
        {
            incoming = audioSource.getLastPeak();  // linear peak
        }
        else if (meterMode == MeterMode::LUFS)
        {
            incoming = audioSource.getLastLufs();   // already in LUFS (dB-like)
            incomingIsDb = true;
        }
        else
        {
            incoming = audioSource.getLastRms();    // linear RMS
        }

        // Safety clamp
        const float safeIncoming = incomingIsDb
            ? juce::jlimit (-100.0f, 0.0f, incoming)
            : juce::jlimit (0.0f, 10.0f, incoming);

        // Smoothing (different behavior for each mode)
        if (meterMode == MeterMode::RMS)
        {
            // RMS: EMA smoothing (both attack and release)
            smoothed = smoothed * alpha + safeIncoming * (1.0f - alpha);
        }
        else if (meterMode == MeterMode::TruePeak)
        {
            // Peak: FAST attack, slow release
            if (safeIncoming > smoothed)
                smoothed = safeIncoming;  // Instant attack!
            else
                smoothed = smoothed * releaseCoeff + safeIncoming * (1.0f - releaseCoeff);
        }
        else // LUFS
        {
            // LUFS: Slow integration (mimics 400ms-3s window)
            smoothed = smoothed * alpha + safeIncoming * (1.0f - alpha);
        }

        // Update peak hold
        if (smoothed > peakHold)
        {
            peakHold = smoothed;
            peakHoldTime = 0.0f;
        }

        auto r = getLocalBounds().reduced (4);
        if (r.getWidth() <= 0 || r.getHeight() <= 0)
            return;  // Safety check

        // Convert to dB
        const float smoothedDb = incomingIsDb ? smoothed : linearToDb (smoothed);
        const float peakDb     = incomingIsDb ? peakHold : linearToDb (peakHold);

        // dB range depends on mode
        const float minDb = -60.0f;
        float maxDb = +6.0f;  // Default for RMS

        if (meterMode == MeterMode::TruePeak)
            maxDb = +3.0f;  // Peak: +3dB headroom
        else if (meterMode == MeterMode::LUFS)
            maxDb = 0.0f;   // LUFS: 0 LUFS is reference (EBU R128: -23 LUFS target)

        const float zeroDbFs = 0.0f;

        // Normalize to 0..1 range for display
        auto normalizeDb = [minDb, maxDb](float db) -> float
        {
            return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
        };

        const float normalizedLevel = normalizeDb (smoothedDb);
        const float normalizedPeak = normalizeDb (peakDb);
        const float normalizedZero = normalizeDb (zeroDbFs);

        const int barWidth = r.getWidth();
        const int levelWidth = (int) (normalizedLevel * barWidth);
        const int zeroWidth = (int) (normalizedZero * barWidth);

        // Render based on mode
        if (meterMode == MeterMode::RMS)
        {
            renderRmsMode (g, r, levelWidth, zeroWidth);
        }
        else if (meterMode == MeterMode::TruePeak)
        {
            renderPeakMode (g, r, levelWidth, barWidth, normalizeDb);
        }
        else // LUFS
        {
            renderLufsMode (g, r, levelWidth, barWidth, normalizeDb);
        }

        // Peak hold line (always white for now)
        if (peakHold > 0.001f && normalizedPeak > 0.0f)
        {
            const int peakX = r.getX() + (int) (normalizedPeak * barWidth);
            if (peakX >= r.getX() && peakX <= r.getRight())
            {
                g.setColour (juce::Colours::white.withAlpha (0.9f));
                g.drawLine ((float) peakX, (float) r.getY(), (float) peakX, (float) r.getBottom(), 2.0f);
            }
        }

        // Draw 0 dBFS marker
        drawZeroDbMarker (g, r, normalizeDb, meterMode);

        // Draw scale markings (key checkpoints for each mode)
        drawScaleMarkings (g, r, normalizeDb, meterMode);

        // NOTE: Text info is now rendered by PanelHost (always horizontal)
    }

private:
    void renderRmsMode (juce::Graphics& g, juce::Rectangle<int> r, int levelWidth, int zeroWidth)
    {
        // RMS mode: split at 0 dBFS (green below, red above)
        if (levelWidth > 0)
        {
            // Part 1: Below 0 dBFS (normal bar color)
            const int safeWidth = juce::jmin (levelWidth, zeroWidth);
            if (safeWidth > 0)
            {
                auto safeFill = r.withWidth (safeWidth);
                g.setColour (barColour);
                g.fillRect (safeFill);
            }

            // Part 2: Above 0 dBFS (CLIPPING - color depends on mode)
            if (levelWidth > zeroWidth)
            {
                const int clipWidth = levelWidth - zeroWidth;
                auto clipFill = r.withX (r.getX() + zeroWidth).withWidth (clipWidth);

                juce::Colour clipColour;
                if (colorMode == ColorMode::Standard)
                    clipColour = juce::Colours::red.withAlpha (0.85f);
                else if (colorMode == ColorMode::CustomGradient)
                    clipColour = barColour.withBrightness (0.3f);
                else // CustomSpectrum
                    clipColour = getAdjacentColor (barColour, 0.3f);  // 30% towards adjacent

                g.setColour (clipColour);
                g.fillRect (clipFill);
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
        // position: 1.0 = base color, 0.0 = adjacent color
        // Adjacent color = +60° on HSV color wheel
        float hue = baseColour.getHue();
        const float sat = baseColour.getSaturation();
        const float brightness = baseColour.getBrightness();

        // Rotate hue by +60° (0.166 in 0-1 range) for adjacent/analogous color
        const float targetHue = std::fmod (hue + 0.166f, 1.0f);

        // Interpolate between base and adjacent hue
        float interpolatedHue = hue + (targetHue - hue) * (1.0f - position);
        if (interpolatedHue < 0.0f) interpolatedHue += 1.0f;
        if (interpolatedHue > 1.0f) interpolatedHue -= 1.0f;

        return juce::Colour::fromHSV (interpolatedHue, sat, brightness, baseColour.getAlpha());
    }
    void timerCallback() override
    {
        // Peak hold decay (varies by mode)
        float holdTime = 2.0f;  // Default for RMS
        if (meterMode == MeterMode::TruePeak)
            holdTime = 3.0f;
        else if (meterMode == MeterMode::LUFS)
            holdTime = 3.0f;  // LUFS: longer hold (integrated loudness)

        peakHoldTime += 1.0f / 30.0f;

        if (peakHoldTime > holdTime)
        {
            // Decay rate depends on mode
            float decayFactor = 0.98f;  // Default for RMS
            if (meterMode == MeterMode::TruePeak)
                decayFactor = 0.95f;  // Faster decay
            else if (meterMode == MeterMode::LUFS)
                decayFactor = 0.99f;  // Slower decay (integrated)

            peakHold *= decayFactor;

            if (peakHold < 0.001f)
                peakHold = 0.0f;
        }

        repaint();
    }

    void drawZeroDbMarker (juce::Graphics& g, juce::Rectangle<int> r, 
                          std::function<float(float)> normalizeDb,
                          MeterMode mode)
    {
        const float zeroDb = 0.0f;
        const float normalized = normalizeDb (zeroDb);
        const int x = r.getX() + (int) (normalized * r.getWidth());

        if (x >= r.getX() && x <= r.getRight())
        {
            juce::Colour markerColour = (mode == MeterMode::TruePeak) 
                                      ? juce::Colours::red.withAlpha (0.7f)
                                      : juce::Colours::white.withAlpha (0.5f);

            g.setColour (markerColour);
            g.drawLine ((float) x, (float) r.getY(), (float) x, (float) (r.getY() + 8), 1.5f);

            // Only draw text label when NOT rotated 90/270
            if (hostRotation == 0 || hostRotation == 2)
            {
                g.setFont (10.0f);
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawText ("0", x - 8, r.getY() + 10, 16, 12, juce::Justification::centred, false);
            }
        }
    }

    void drawScaleMarkings (juce::Graphics& g, juce::Rectangle<int> r,
                           std::function<float(float)> normalizeDb,
                           MeterMode mode)
    {
        // Key checkpoint values for each mode
        std::vector<float> checkpoints;

        if (mode == MeterMode::RMS)
        {
            checkpoints = { -60.0f, -18.0f, -12.0f, -6.0f, 0.0f };
        }
        else if (mode == MeterMode::TruePeak)
        {
            checkpoints = { -60.0f, -12.0f, -6.0f, -3.0f, 0.0f };
        }
        else // LUFS
        {
            checkpoints = { -60.0f, -23.0f, -14.0f, -9.0f, 0.0f };
        }

        g.setFont (10.0f);

        for (float dbValue : checkpoints)
        {
            const float normalized = normalizeDb (dbValue);
            const int x = r.getX() + (int) (normalized * r.getWidth());

            if (x >= r.getX() && x <= r.getRight())
            {
                // Draw tick mark (brighter)
                g.setColour (juce::Colours::lightgrey.withAlpha (0.7f));
                const int tickHeight = (dbValue == 0.0f || dbValue == -60.0f) ? 12 : 8;
                g.drawLine ((float) x, (float) r.getY(), (float) x, (float) (r.getY() + tickHeight), 1.0f);

                // Only draw text labels when NOT rotated 90/270
                // (PanelHost draws them in host space for rotated meters)
                if (hostRotation == 0 || hostRotation == 2)
                {
                    juce::String label;
                    if (mode == MeterMode::LUFS)
                        label = juce::String ((int) dbValue);
                    else
                        label = (dbValue == 0.0f) ? "0" : juce::String ((int) dbValue);

                    g.setColour (juce::Colours::white.withAlpha (0.85f));
                    const int labelWidth = 24;
                    g.drawText (label, x - labelWidth/2, r.getY() + tickHeight + 2, labelWidth, 11,
                               juce::Justification::centred, false);
                }
            }
        }
    }

    // Convert linear amplitude (0..1+) to dB
    static float linearToDb (float linear)
    {
        if (linear <= 0.00001f) return -60.0f;  // Floor at -60 dB
        return juce::jlimit (-60.0f, 60.0f, 20.0f * std::log10 (linear));
    }

    IAudioSource& audioSource;

    // Mode
    MeterMode meterMode { MeterMode::RMS };
    ColorMode colorMode { ColorMode::Standard };

    // Smoothing
    float smooth01 { 0.0f };
    float alpha { 0.0f };           // RMS mode coefficient
    float releaseCoeff { 0.9f };    // Peak mode release coefficient
    float smoothed { 0.0f };

    // Peak hold
    float peakHold { 0.0f };
    float peakHoldTime { 0.0f };

    // Color
    juce::Colour barColour { juce::Colours::limegreen };

    // Host rotation state (0=0°, 1=90°, 2=180°, 3=270°)
    int hostRotation { 0 };
};

// Backwards compatibility: allow old code to still use the name VisualRmsBar
using VisualRmsBar = VisualAudioMeter;
