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

// VisualOscilator: displays direct time-domain waveform from ALTW stereo
// samples received via IAudioSource. Supports Mono, Stereo (L up / R down),
// and Mirror (mono mirrored) display modes with zero-crossing trigger.

class VisualOscilator  : public juce::Component,
                         private juce::Timer
{
public:
    enum class DisplayMode { Mono, Stereo, Mirror };

    explicit VisualOscilator (IAudioSource& receiver)
        : audioSource (receiver)
    {
        setOpaque (true);
        lineColour = juce::Colours::violet;
        startTimerHz (22);
    }

    ~VisualOscilator() override { stopTimer(); }

    void setFill (bool b) noexcept { showFill = b; repaint(); }
    void setColour (juce::Colour c) noexcept { lineColour = c; repaint(); }
    void setLineColour (juce::Colour c) noexcept { setColour (c); }

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

    // Compatibility no-ops (panel props applied uniformly)
    void setUseAWeight (bool) noexcept {}
    void setDisplayBins (int) noexcept {}

    DisplayMode getDisplayMode() const { return displayMode; }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.fillAll (juce::Colours::black);
        g.setColour (juce::Colours::darkgrey);
        g.drawRect (bounds, 1.0f);

        // Get stereo waveform from audio source
        std::vector<float> waveL, waveR;
        const int count = audioSource.getLastWaveform (waveL, waveR);

        if (count < 4)
            return;

        const int W = juce::jmax (64, getWidth());

        // --- Time window from zoom ---
        // zoom01 maps logarithmically: 0.0 = 85 ms (~12 Hz), 1.0 = 0.1 ms (10 kHz)
        const double timeWindowSec = 0.085 * std::pow (0.001176, (double) zoom01);
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
            g.setColour (lineColour.withAlpha (0.30f));
            g.fillRect (bounds.getX(), cy - 0.5f, bounds.getWidth(), 1.0f);
        }

        // Build paths based on display mode
        const float vScale = bounds.getHeight() * 0.42f;
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

            g.setColour (lineColour.withAlpha (0.25f));
            g.fillPath (fillPath);
        }

        // Stroke paths
        g.setColour (lineColour);
        g.strokePath (pMain, juce::PathStrokeType (2.0f));
        if (! pSecondary.isEmpty())
        {
            g.setColour (lineColour.withAlpha (0.5f));
            g.strokePath (pSecondary, juce::PathStrokeType (1.2f));
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
    bool showFill { false };
    double assumedSampleRateHz { 48000.0 };

    DisplayMode displayMode { DisplayMode::Mono };
    float zoom01 { 0.420f };

    float smooth01 { 0.0f };
    float smoothAlpha { 0.0f };
    std::vector<float> prevL, prevR;

    void timerCallback() override
    {
        repaint();
    }
};
