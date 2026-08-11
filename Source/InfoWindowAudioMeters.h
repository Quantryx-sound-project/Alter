/*
  ==============================================================================

    ModuleInfoWindows.h
    Created: 10 Mar 2026 8:37:48pm
    Author:  Martin

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include "AlterTheme.h"
#include <vector>

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
        // Owned by the controller (std::unique_ptr) – just hide, never self-delete.
        setVisible (false);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// TextInfoWindow – reusable scrollable text guide (heading + body sections).
// Module guides only provide their sections – no duplicated window/viewport/
// painting code per module.
// ─────────────────────────────────────────────────────────────────────────────
class TextInfoWindow : public ModuleInfoWindow
{
public:
    struct Section { juce::String heading, body; };

    TextInfoWindow (const juce::String& title, std::vector<Section> sections)
        : ModuleInfoWindow (title)
    {
        auto* viewport = new juce::Viewport();
        viewport->setViewedComponent (new Panel (title, std::move (sections)), true);
        viewport->setScrollBarsShown (true, false);
        setContentOwned (viewport, true);

        setSize (620, 720);
        centreWithSize (getWidth(), getHeight());
    }

    void resized() override
    {
        ModuleInfoWindow::resized();
        if (auto* vp = dynamic_cast<juce::Viewport*> (getContentComponent()))
            if (auto* content = vp->getViewedComponent())
                content->setSize (vp->getMaximumVisibleWidth(), content->getHeight());
    }

private:
    class Panel : public juce::Component
    {
    public:
        Panel (juce::String t, std::vector<Section> s)
            : title (std::move (t)), sections (std::move (s))
        {
            int hgt = 90;
            for (const auto& sec : sections)
                hgt += 34 + 19 * estimateLines (sec.body) + 14;
            setSize (620, hgt);
        }

        void paint (juce::Graphics& g) override
        {
            AlterTheme::paintBackground (g, getLocalBounds().toFloat());
            auto area = getLocalBounds().reduced (24, 18);

            g.setColour (AlterTheme::textBright);
            g.setFont (juce::Font (juce::FontOptions (24.0f).withStyle ("Bold")));
            g.drawText (title, area.removeFromTop (44), juce::Justification::centred);
            area.removeFromTop (10);

            for (const auto& sec : sections)
            {
                g.setColour (AlterTheme::pictonBlue);
                g.setFont (juce::Font (juce::FontOptions (17.0f).withStyle ("Bold")));
                g.drawText (sec.heading, area.removeFromTop (28), juce::Justification::left);
                area.removeFromTop (6);

                const int lines = estimateLines (sec.body);
                g.setColour (AlterTheme::textNormal);
                g.setFont (juce::Font (juce::FontOptions (13.5f)));
                g.drawFittedText (sec.body, area.removeFromTop (19 * lines),
                                  juce::Justification::topLeft, lines);
                area.removeFromTop (14);
            }
        }

    private:
        static int estimateLines (const juce::String& body)
        {
            int lines = 0;
            for (const auto& ln : juce::StringArray::fromLines (body))
                lines += juce::jmax (1, ln.length() / 78 + 1);
            return lines;
        }

        juce::String title;
        std::vector<Section> sections;
    };
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
            // Tall content for scrolling (accounts for all sections + macOS fonts).
            // Grown with the CONTROLS block: that section is drawn into a fixed-height
            // rectangle at the very bottom, so text added there falls off the end of
            // the canvas rather than making the canvas longer.
            setSize(600, 3060);
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
                "dBTP (dB True Peak): peak of the RECONSTRUCTED analog waveform, i.e. "
                "including inter-sample peaks (ITU-R BS.1770). Can exceed 0 dBTP even when "
                "every stored sample is below 0 dBFS - that clips DACs and lossy codecs.\n\n"
                "LUFS (Loudness Units Full Scale): Perceptual loudness standard (ITU-R BS.1770). "
                "Measures how LOUD audio sounds to human ears, not just peak levels.\n\n"
                "dB vs Perceived Loudness: Our ears don't hear linearly! A +10 dB increase "
                "sounds roughly twice as loud. LUFS accounts for frequency sensitivity.";

            g.drawFittedText(measurements, area.removeFromTop(250).toNearestInt(),
                            juce::Justification::topLeft, 13);

            area.removeFromTop(5);

            // TARGET VALUES section
            g.setFont(juce::Font(16.0f, juce::Font::bold));
            g.setColour(juce::Colours::white);
            g.drawText("TARGET VALUES (Professional Standards)", area.removeFromTop(25), juce::Justification::left);

            area.removeFromTop(5);

            g.setFont(juce::Font(13.0f));
            g.setColour(juce::Colours::lightgrey);

            juce::String targetValues =
                "True Peak ceiling: -1.0 dBTP (EBU R128 / streaming). Loud masters on\n"
                "Spotify (louder than -14 LUFS): -2.0 dBTP. US broadcast (ATSC A/85): -2 dBTP.\n"
                "RMS Average: -18 dBFS (mixing), -12 dBFS (mastering), -8 dBFS (loud masters).\n\n"
                "LUFS Targets:\n"
                "  Broadcast TV: -23 LUFS (EBU R128), US TV: -24 LKFS (ATSC A/85)\n"
                "  Spotify/Apple Music/YouTube: -14 LUFS (streaming normalization)\n"
                "  Pop/EDM Masters: -8 to -10 LUFS (very loud!)\n\n"
                "Avoid: anything above -1 dBTP (clips DACs and lossy codecs: MP3/AAC/OGG\n"
                "encoding adds overshoot), and > -6 LUFS (distortion risk).";

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
                "RMS vs True Peak: RMS = average energy (smoothed). True Peak = inter-sample maximum (dBTP).");

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
            drawModeSection(g, area, "2. TRUE PEAK MODE - INTER-SAMPLE PEAK (dBTP)",
                "Reconstructs the analog waveform (4x oversampling, ITU-R BS.1770) and\n"
                "reads peaks BETWEEN samples - can exceed the sample peak by 0-3 dB.\n"
                "Strip: momentary dBTP + session MAX + SP (sample peak). Double-click = reset MAX.",
                {
                    {"GREEN: Safe zone (below -3 dBTP)", juce::Colours::limegreen},
                    {"YELLOW: Hot (-3 to 0 dBTP) - keep masters at or below -1 dBTP", juce::Colours::yellow.withAlpha(0.9f)},
                    {"RED: Above 0 dBTP - will clip DACs and lossy codecs", juce::Colours::red.withAlpha(0.95f)}
                },
                "USE CASES: Mastering ceiling checks, export QA, limiter setup");

            area.removeFromTop(8);

            g.setFont(juce::Font(12.0f));
            g.setColour(juce::Colour(0xFF88CCFF));
            g.drawFittedText(
                "Why do meters disagree?  1) MEASUREMENT POINT: this module meters the "
                "WINDOWS OUTPUT MIX (WASAPI loopback - after app volume, master volume and "
                "Windows enhancements), while DAW plugins (Youlean, dpMeter) meter their own "
                "insert slot - different signals, different numbers. 2) MOMENTARY vs MAX: "
                "most plugins print the session maximum - compare it with this meter's MAX "
                "value, not the moving number. 3) Oversampling factor (2x/4x/8x) and filter "
                "quality change true-peak readings by a few tenths of dB - it is an ESTIMATE. "
                "The SYSTEM GAIN control is NOT included in True Peak / LUFS / SP - these "
                "always measure the real signal (RMS and the visuals do follow the gain).",
                area.removeFromTop(130), juce::Justification::topLeft, 9);

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

            area.removeFromTop(15);

            // MODE 4: Level History
            drawModeSection(g, area, "4. LEVEL HISTORY - PEAK OUTSIDE, RMS INSIDE",
                "A scrolling timeline (0.1-30 s window): the outer silhouette is PEAK\n"
                "movement, the inner body is RMS energy. The gap between them is the\n"
                "story of punch versus density. Stereo view draws L and R separately.",
                {
                    {"Big gap: transients + headroom - punchy, breathing mix",  juce::Colours::limegreen},
                    {"Shrinking gap: dense / compressed / limited material",    juce::Colours::orange},
                    {"Outline hugging the body: heavy limiting (sausage alert)", juce::Colour(0xFFFF2222)}
                },
                "USE CASES: compressor pumping, drop energy, DJ track comparison, limiter abuse");

            area.removeFromTop(8);

            g.setFont(juce::Font(12.0f));
            g.setColour(juce::Colour(0xFF88CCFF));
            g.drawFittedText(
                "There is no official target like \"-14 Level History\" - it is a behaviour view, "
                "not a delivery number. Read the MOTION: a breakdown that feels empty because the "
                "body disappears, a limiter that clamps only in the drop, a compressor that pumps "
                "rhythmically - Level History shows that the problem happened five seconds ago, "
                "not only right now.",
                area.removeFromTop(68), juce::Justification::topLeft, 5);

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
                "MODE: RMS / True Peak / LUFS / Level history\n"
                "SMOOTH: the display time constant - how long the meter takes to cover\n"
                "  63% of a step. 10 ms at 0, 2 s at 1, geometric in between, and it is\n"
                "  now the SAME scale in every mode, including Level history (which\n"
                "  smooths the envelope as it is recorded). It used to be three unrelated\n"
                "  mappings behind one slider - LUFS at 0 was more smoothed than RMS at 1\n"
                "  - so the same number meant a different thing in each mode and two\n"
                "  meters set identically did not behave identically.\n"
                "  The modes still differ in CHARACTER, which is the part that should:\n"
                "  True Peak rises instantly and falls at this time constant, as a peak\n"
                "  meter must; RMS and LUFS move both ways at it.\n"
                "VIEW: Momentary bar / Trend timeline. WINDOW (Level history): 0.1-30 s\n"
                "\n"
                "PEAK HOLD BAR (on by default): the wide bar is the smoothed level, the\n"
                "  narrow one beside it is the peak/max hold - where the level just WAS.\n"
                "  A smoothed bar alone hides the transient that actually clipped. Untick\n"
                "  it for the classic single bar.\n"
                "  The hold sits at a new maximum for 5 s, then falls at 12 dB/s until it\n"
                "  meets the live level - the same in RMS, True Peak and LUFS, so the\n"
                "  three modes can be compared against each other. Double-click the meter\n"
                "  to re-arm it (and the printed MAX) immediately.\n"
                "\n"
                "HOVER anywhere on the bar for the dB value at that height; in Trend the\n"
                "  readout adds the position in the capture.\n"
                "\n"
                "READOUT: the module's top-right info carries it on separate lines - what\n"
                "  it is measuring, the number, and the unit. (It used to be one line of\n"
                "  text along the bottom edge, which in a 96 px column was unreadable.)\n"
                "\n"
                "SCALE: -60 dB to +6, the SAME in every mode including Level history.\n"
                "  The 6 dB above full scale is there because True Peak needs it - an\n"
                "  inter-sample peak genuinely goes above 0 dBFS - and because it is the\n"
                "  only way the modes can line up with each other. While RMS and Level\n"
                "  history stopped at 0 dBFS, the same signal drew at two different\n"
                "  heights next to a True Peak bar: one put 0 dB at the top of its\n"
                "  picture, the other nine tenths of the way up. Now 0 dB is at the same\n"
                "  place in all of them, and over-scale material has somewhere to go.\n"
                "  Level history is two bar meters mirrored about its centre line: each\n"
                "  half runs -60 dB at the centre to the top of the scale at the edge, so\n"
                "  a given dB sits at the same fraction of the travel as it does on a\n"
                "  bar, and the two line up when placed side by side.\n"
                "  It used to plot linear amplitude, which is why it never agreed with the\n"
                "  meter beside it: a -20 dBFS passage is 0.1 in amplitude, so it drew as\n"
                "  a 10%-high ripple while the True Peak bar stood two thirds of the way\n"
                "  up. Same signal, two answers. Now the history is the recording of what\n"
                "  the bar just did, and the two line up.\n"
                "  One consequence worth knowing: a quiet noise floor is now VISIBLE as a\n"
                "  band rather than a flat line, exactly as it registers on the bar.\n"
                "\n"
                "WIDTH: the meter is the only module the layout does not size for you -\n"
                "  it is a column, thin and tall on purpose, so it starts at 96 px and\n"
                "  keeps that until you say otherwise. Put a spectrum next to it and the\n"
                "  spectrum takes the whole block except those 96 px.\n"
                "  It is a DEFAULT, not a lock. Set the number here, or drag the module's\n"
                "  right edge in the HUD - either way it goes as wide as the block. With\n"
                "  two meters side by side, dragging the divider grows one into the empty\n"
                "  part of the row first and only then into its neighbour, so one meter\n"
                "  can be taken to the full width of the HUD.\n"
                "\n"
                "COLOR MODE: STANDARD is the fixed DAW ladder - green, yellow, orange,\n"
                "  red. CUSTOM GRADIENT and CUSTOM COMPLEMENTARY build the same ladder\n"
                "  from YOUR colour instead: gradient keeps one hue and steps it down in\n"
                "  brightness, complementary walks the hot zones toward the far side of\n"
                "  the colour wheel so the top of the bar is a different colour rather\n"
                "  than a darker one.\n"
                "COLOR BY TONE: the same ladder, built from the dominant note. ZONES\n"
                "  offers the same two choices - GRADIENT or COMPLEMENTARY - because it\n"
                "  is the same question asked of a different base colour.\n"
                "\n"
                "  Every mode draws the SAME number of zones at the SAME thresholds,\n"
                "  whichever colour scheme is chosen: RMS breaks at -12 and -6, True Peak\n"
                "  at -3 and 0, LUFS at -23, -14 and -9. And it reaches the bar, the hold\n"
                "  bar and the Trend curve together, because all three read one ladder -\n"
                "  so a Trend recording crosses its thresholds in exactly the colours the\n"
                "  bar showed live.\n"
                "\n"
                "TREND: the module's top-right info line adds AVG - the mean of the whole\n"
                "  capture. A curve shows how the level MOVED; the one thing you cannot\n"
                "  read off it by eye is where it sat on balance, which for a loudness\n"
                "  pass is the number you deliver against.\n"
                "\n"
                "LEVEL HISTORY: CLIPPING ZONE draws dashed 0 dBFS lines top and bottom\n"
                "  plus a dB ruler down the right edge (0 / -6 / -12 / -20), in the\n"
                "  MODULE'S colour. Without it the view has no absolute reference at all\n"
                "  and a hot master and a quiet stem draw the same shape.\n"
                "  Wherever the audio reaches the ceiling, a RED BAR is laid along the\n"
                "  line. The envelope is clamped at full scale (as a DAW clip overview\n"
                "  is), so a column 6 dB over looks the same as one that just touched -\n"
                "  the red marks tell them apart and show where in time it happened.\n"
                "  For HOW far over, use True Peak: that is a meter's job, and dBTP also\n"
                "  catches inter-sample peaks a waveform view cannot show at all.\n"
                "  L/R COLOR sets the R channel's hue in the overlaid stereo layout.\n"
                "  HOVER for the time and level under the pointer.";

            g.drawText(controls, area.removeFromTop(340), juce::Justification::topLeft);
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
