/*
  ==============================================================================

    ModuleInfoWindows.h
    Created: 10 Mar 2026 8:37:48pm
    Author:  Martin

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

// ========================================
// BASE CLASS: Spoločná funkcionalita
// ========================================
class ModuleInfoWindow : public juce::DocumentWindow
{
public:
    explicit ModuleInfoWindow(const juce::String& title)
        : juce::DocumentWindow(title,
            juce::Colours::darkgrey,
            juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);  // not fixed size

        // Content will be set by child classes

        centreWithSize(600, 900);  // Increased height (was 700)
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        delete this;  // Modal-less window (auto-delete on close)
    }
};


// ========================================
// AUDIO METER MODULE INFO
// ========================================
class AudioMeterInfoWindow : public ModuleInfoWindow
{
public:
    AudioMeterInfoWindow()
        : ModuleInfoWindow("Audio Meter - Educational Guide")
    {
        // Create content component with larger height for scrolling
        auto* content = new ContentPanel();

        // Wrap in viewport for scrollbar
        auto* viewport = new juce::Viewport();
        viewport->setViewedComponent(content, true); // true = viewport owns content
        viewport->setScrollBarsShown(true, false);   // vertical scroll only

        setContentOwned(viewport, true);

        setSize(600, 900);  // Increased window height
        centreWithSize(getWidth(), getHeight());
    }

    void resized() override
    {
        ModuleInfoWindow::resized();
        if (auto* vp = dynamic_cast<juce::Viewport*>(getContentComponent()))
        {
            if (auto* content = vp->getViewedComponent())
                content->setSize(vp->getMaximumVisibleWidth(), content->getHeight());
        }
    }

private:
    // Content panel with text + visual elements
    class ContentPanel : public juce::Component
    {
    public:
        ContentPanel()
        {
            setSize(600, 1900);  // Tall content for scrolling (accounts for all sections + macOS fonts)
        }

        void paint(juce::Graphics& g) override
        {
            // Background
            g.fillAll(juce::Colour(0xFF1E1E1E));  // Dark grey

            auto area = getLocalBounds().reduced(20);

            // Title
            g.setColour(juce::Colours::white);
            g.setFont(juce::Font(24.0f, juce::Font::bold));
            g.drawText("Audio Meters", area.removeFromTop(40), juce::Justification::centred);

            area.removeFromTop(10);

           

            // ========================================
            // SECTION: UNDERSTANDING MEASUREMENTS
            // ========================================
            g.setFont(juce::Font(18.0f, juce::Font::bold));
            g.setColour(juce::Colours::white);
            g.drawText("UNDERSTANDING MEASUREMENTS", area.removeFromTop(30), juce::Justification::left);

            area.removeFromTop(8);

            g.setFont(juce::Font(13.0f));
            g.setColour(juce::Colours::lightgrey);

            juce::String measurements =
                "dBFS (decibels Full Scale): Digital audio measurement. 0 dBFS = maximum "
                "digital level before clipping. Everything is negative (-inf to 0 dBFS).\n\n"
                "LUFS (Loudness Units Full Scale): Perceptual loudness standard (ITU-R BS.1770). "
                "Measures how LOUD audio sounds to human ears, not just peak levels.\n\n"
                "dB vs Perceived Loudness: Our ears don't hear linearly! A +10 dB increase "
                "sounds roughly twice as loud. LUFS accounts for frequency sensitivity.";

            g.drawFittedText(measurements, area.removeFromTop(190).toNearestInt(), 
                            juce::Justification::topLeft, 9);

            area.removeFromTop(5);

            // TARGET VALUES section
            g.setFont(juce::Font(16.0f, juce::Font::bold));
            g.setColour(juce::Colours::white);
            g.drawText("TARGET VALUES (Professional Standards)", area.removeFromTop(25), juce::Justification::left);

            area.removeFromTop(5);

            g.setFont(juce::Font(13.0f));
            g.setColour(juce::Colours::lightgrey);

            juce::String targetValues =
                "Peak Levels (dBFS): Keep below -3 dBFS to avoid inter-sample clipping.\n"
                "RMS Average: -18 dBFS (mixing), -12 dBFS (mastering), -8 dBFS (loud masters).\n\n"
                "LUFS Targets:\n"
                "  Broadcast TV: -23 LUFS (EBU R128 standard)\n"
                "  Spotify/Apple Music: -14 LUFS (streaming sweet spot)\n"
                "  YouTube: -13 to -15 LUFS\n"
                "  Pop/EDM Masters: -8 to -10 LUFS (very loud!)\n\n"
                "Avoid: Constant 0 dBFS peaks (clipping), > -6 LUFS (distortion risk).";

            g.drawFittedText(targetValues, area.removeFromTop(240).toNearestInt(), 
                            juce::Justification::topLeft, 12);

            area.removeFromTop(5);

            // Section: Visual Example - LOUDNESS ZONES
            g.setFont(juce::Font(18.0f, juce::Font::bold));
            g.setColour(juce::Colours::white);
            g.drawText("VISUAL EXAMPLE - LOUDNESS ZONES", area.removeFromTop(30), juce::Justification::left);

            // Draw 4 meter bars (all LUFS zones)
            drawMeterBar(g, area.removeFromTop(35), juce::Colours::limegreen, 0.4f, "GREEN: Quiet / Safe (-inf to -23 LUFS)");
            area.removeFromTop(4);
            drawMeterBar(g, area.removeFromTop(35), juce::Colours::yellow.withAlpha(0.9f), 0.65f, "YELLOW: Streaming Target (-23 to -14 LUFS)");
            area.removeFromTop(4);
            drawMeterBar(g, area.removeFromTop(35), juce::Colours::orange.withAlpha(0.9f), 0.85f, "ORANGE: Hot / Loud (-14 to -9 LUFS)");
            area.removeFromTop(4);
            drawMeterBar(g, area.removeFromTop(35), juce::Colours::red.withAlpha(0.95f), 0.98f, "RED: Clipping / Extreme (> -9 LUFS / 0 dBFS)");

            area.removeFromTop(25);

            // ========================================
            // SECTION: METER MODES
            // ========================================
            g.setFont(juce::Font(20.0f, juce::Font::bold));
            g.setColour(juce::Colours::white);
            g.drawText("METER MODES", area.removeFromTop(35), juce::Justification::left);

            area.removeFromTop(10);

            // MODE 1: RMS (Average)
            drawModeSection(g, area, "1. RMS MODE - AVERAGE ENERGY (EDM/Mixing zones)",
                "Root Mean Square = average energy of the signal over time.\n"
                "Shows how LOUD the mix feels continuously - not instant peaks.\n"
                "Bar = RMS only. True Peak is NOT shown here (see CLIP indicator).",
                {
                    {"GREEN  (OK):        <= -12 dBFS RMS  -  safe headroom",        juce::Colours::limegreen},
                    {"YELLOW (Hot):    -12 to -6 dBFS RMS  -  approaching loud",     juce::Colour(0xFFFFDD00)},
                    {"ORANGE (V.Hot):   -6 to -3 dBFS RMS  -  very loud, EDM limit", juce::Colours::orange},
                    {"RED    (Extreme): -3 to  0 dBFS RMS  -  loudness war zone",    juce::Colour(0xFFFF2222)},
                    {"CLIP indicator:      Peak >= 0 dBFS  -  digital clipping!",    juce::Colour(0xFFFF0000)}
                },
                "RMS vs True Peak: RMS = average energy (smoothed). True Peak = absolute max sample.");

            area.removeFromTop(8);

            g.setFont(juce::Font(12.0f));
            g.setColour(juce::Colour(0xFF88CCFF));
            g.drawFittedText(
                "Why separate CLIP indicator?  The RMS bar can stay at -6 dBFS while a transient "
                "(kick drum, snare hit) simultaneously clips at 0 dBFS or above. The CLIP indicator "
                "reads True Peak independently - it lights up red the moment any sample reaches "
                "0 dBFS, even if the RMS bar looks safe. This is the standard DAW behaviour.",
                area.removeFromTop(68), juce::Justification::topLeft, 5);

            area.removeFromTop(15);

            // MODE 2: True Peak
            drawModeSection(g, area, "2. TRUE PEAK MODE - MAXIMUM DETECTION",
                "Detects absolute maximum sample values in your audio signal.\n"
                "Most accurate way to detect clipping before export.",
                {
                    {"GREEN: Safe zone (below -3 dBFS)", juce::Colours::limegreen},
                    {"YELLOW: Hot zone (-3 to 0 dBFS)", juce::Colours::yellow.withAlpha(0.9f)},
                    {"RED: Clipping (above 0 dBFS)", juce::Colours::red.withAlpha(0.95f)}
                },
                "USE CASES: Mastering, export preparation, peak limiting");

            area.removeFromTop(15);

            // MODE 3: LUFS (Loudness)
            drawModeSection(g, area, "3. LUFS MODE - PERCEPTUAL LOUDNESS",
                "Loudness Units Full Scale (ITU-R BS.1770) - measures how LOUD audio\n"
                "SOUNDS to the human ear. Industry standard for streaming platforms.",
                {
                    {"GREEN: Quiet (< -23 LUFS) - Broadcast", juce::Colours::limegreen},
                    {"YELLOW: Streaming (-23 to -14 LUFS) - Spotify", juce::Colours::yellow.withAlpha(0.9f)},
                    {"ORANGE: Hot (-14 to -9 LUFS) - Modern pop", juce::Colours::orange.withAlpha(0.9f)},
                    {"RED: Loudness War (> -9 LUFS) - Extreme", juce::Colours::red.withAlpha(0.95f)}
                },
                "USE CASES: Streaming (Spotify, YouTube), platform compliance");

            area.removeFromTop(20);

            // ========================================
            // SECTION: CONTROLS
            // ========================================
            g.setFont(juce::Font(20.0f, juce::Font::bold));
            g.setColour(juce::Colours::white);
            g.drawText("CONTROLS", area.removeFromTop(35), juce::Justification::left);

            area.removeFromTop(5);

            g.setFont(juce::Font(14.0f));
            g.setColour(juce::Colours::lightgrey);

            juce::String controls =
                "MODE: Switch between RMS / True Peak / LUFS\n"
                "SMOOTH: Adjust response speed (0.0 = fast, 1.0 = slow)\n"
                "COLOR MODE: Standard (DAW colors) / Custom Gradient / Custom Spectrum";

            g.drawText(controls, area.removeFromTop(80), juce::Justification::topLeft);
        }

        void drawMeterBar(juce::Graphics& g, juce::Rectangle<int> area,
            juce::Colour barColor, float level, const juce::String& label)
        {
            // Bar background (dark)
            g.setColour(juce::Colour(0xFF2A2A2A));
            g.fillRect(area);

            // Bar fill
            int fillWidth = (int)(area.getWidth() * level);
            g.setColour(barColor);
            g.fillRect(area.withWidth(fillWidth));

            // Border
            g.setColour(juce::Colours::grey);
            g.drawRect(area, 1);

            // Label overlay
            g.setColour(juce::Colours::white);
            g.setFont(juce::Font(12.0f, juce::Font::bold));
            g.drawText(label, area, juce::Justification::centred);
        }
        void drawModeSection(juce::Graphics& g, juce::Rectangle<int>& area,
            const juce::String& title,
            const juce::String& description,
            const std::vector<std::pair<juce::String, juce::Colour>>& zones,
            const juce::String& useCases)
        {
            // Mode title
            g.setFont(juce::Font(16.0f, juce::Font::bold));
            g.setColour(juce::Colours::white);
            g.drawText(title, area.removeFromTop(25), juce::Justification::left);

            area.removeFromTop(3);

            // Description
            g.setFont(juce::Font(13.0f));
            g.setColour(juce::Colours::lightgrey);
            g.drawFittedText(description, area.removeFromTop(55).toNearestInt(), 
                            juce::Justification::topLeft, 3);  // 3 lines max

            area.removeFromTop(5);

            // Color zones
            g.setFont(juce::Font(12.0f));
            for (const auto& zone : zones)
            {
                auto zoneArea = area.removeFromTop(20);
                auto colorBox = zoneArea.removeFromLeft(20);

                // Color indicator box
                g.setColour(zone.second);
                g.fillRect(colorBox.reduced(2));
                g.setColour(juce::Colours::grey);
                g.drawRect(colorBox.reduced(2), 1);

                // Label
                zoneArea.removeFromLeft(5);
                g.setColour(juce::Colours::white);
                g.drawText(zone.first, zoneArea, juce::Justification::centredLeft);
            }

            area.removeFromTop(3);

            // Use cases
            g.setFont(juce::Font(12.0f, juce::Font::italic));
            g.setColour(juce::Colour(0xFF88CCFF)); // Light blue
            g.drawText(useCases, area.removeFromTop(18), juce::Justification::left);
        }
    };
};
