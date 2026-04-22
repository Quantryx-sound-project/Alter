/*
  ==============================================================================

    InfoWindowOscilator.h
    Created: 29 Mar 2026 1:52:49pm
    Author:  Martin

  ==============================================================================
*/

#pragma once
#include "InfoWindowAudioMeters.h"

class OscillatorInfoWindow : public ModuleInfoWindow
{
public:
    OscillatorInfoWindow()
        : ModuleInfoWindow ("Oscillator - Educational Guide")
    {
        auto* content = new ContentPanel();
        auto* viewport = new juce::Viewport();
        viewport->setViewedComponent (content, true);
        viewport->setScrollBarsShown (true, false);
        setContentOwned (viewport, true);
        setSize (620, 900);
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
    class ContentPanel : public juce::Component
    {
    public:
        ContentPanel() { setSize (620, 2600); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xFF1E1E1E));
            auto area = getLocalBounds().reduced (20);

            // Title
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (24.0f, juce::Font::bold));
            g.drawText ("Oscilloscope", area.removeFromTop (40), juce::Justification::centred);
            area.removeFromTop (10);

            // ========================================
            // SECTION: WHAT IS AN OSCILLOSCOPE?
            // ========================================
            drawSectionTitle (g, area, "WHAT IS AN OSCILLOSCOPE?");

            g.setFont (juce::Font (13.0f));
            g.setColour (juce::Colours::lightgrey);
            juce::String intro =
                "An oscilloscope displays raw audio samples over time. Each point on the waveform "
                "represents the position of the speaker membrane at that instant. Positive values push "
                "the membrane forward (towards you), negative values pull it back.\n\n"
                "This oscilloscope uses a time-window approach: the Zoom control sets how wide the "
                "displayed time window is. Low zoom shows long windows (up to 85 ms), "
                "high zoom shows short windows (down to 100 us). "
                "Higher frequencies naturally appear closer together, lower ones more spread out.";
            g.drawFittedText (intro, area.removeFromTop (170), juce::Justification::topLeft, 10);
            area.removeFromTop (10);

            

            // ========================================
            // SECTION: PERIOD DETECTION
            // ========================================
            drawSectionTitle (g, area, "ZOOM & TIME WINDOW");

            g.setFont (juce::Font (13.0f));
            g.setColour (juce::Colours::lightgrey);
            juce::String zoomText =
                "The Zoom control sets the visible time window using a logarithmic mapping:\n\n"
                "  Zoom 0.0   ->  85 ms    (fully zoomed out)\n"
                "  Zoom 0.25  ->  ~16 ms\n"
                "  Zoom 0.42  ->  ~5 ms    (default, standard scope timebase)\n"
                "  Zoom 0.75  ->  ~0.5 ms\n"
                "  Zoom 1.0   ->  100 us   (fully zoomed in)\n\n"
                "Formula: timeWindow = 85 ms x 0.001176^zoom\n\n"
                "The logarithmic scale mirrors human hearing: each equal step on the slider "
                "covers the same ratio of frequencies. The default of ~5 ms is a standard "
                "timebase used in professional audio oscilloscopes.";
            g.drawFittedText (zoomText, area.removeFromTop (280), juce::Justification::topLeft, 16);
            area.removeFromTop (10);

            // ========================================
            // SECTION: DISPLAY MODES
            // ========================================
            drawSectionTitle(g, area, "DISPLAY MODES");

            g.setFont(juce::Font(13.0f));
            g.setColour(juce::Colours::lightgrey);
            juce::String modes =
                "MONO: Single waveform (Left channel only). Clean, simple view.\n\n"
                "STEREO: Left and Right channels drawn simultaneously. L on top half, "
                "R on bottom half. Useful for checking stereo width and phase issues.\n\n"
                "MIRROR: When L and R are identical (mono source), the waveform is "
                "mirrored vertically around the center line for a symmetric visual effect. "
                "When L != R (true stereo), behaves like Stereo mode.";
            g.drawFittedText(modes, area.removeFromTop(160), juce::Justification::topLeft, 10);
            area.removeFromTop(10);

            // ========================================
            // SECTION: CONTROLS
            // ========================================
            drawSectionTitle(g, area, "CONTROLS");

            g.setFont(juce::Font(13.0f));
            g.setColour(juce::Colours::lightgrey);
            juce::String controls =
                "SMOOTH: Waveform response speed (0.0 = instant, 1.0 = very slow EMA)\n"
                "FILL: Toggle semi-transparent area fill under the waveform curve\n"
                "DISPLAY: Switch between Mono / Stereo / Mirror modes\n"
                "ZOOM: Time window width (0 = 85 ms zoomed out, 1 = 100 us zoomed in, default ~5 ms)\n"
                "COLOR: Custom waveform color\n"
                "ROTATE: Module rotation (0 / 90 / 180 / 270 degrees)";
            g.drawFittedText(controls, area.removeFromTop(140), juce::Justification::topLeft, 8);
            area.removeFromTop(10);

            // ========================================
            // SECTION: KEY CONCEPTS
            // ========================================
            drawSectionTitle(g, area, "KEY CONCEPTS");

            g.setFont(juce::Font(13.0f));
            g.setColour(juce::Colours::lightgrey);
            juce::String concepts =
                "SPEAKER MEMBRANE: The displayed waveform is literally the position of your "
                "speaker cone over time. A sine wave makes the speaker move in a smooth "
                "back-and-forth motion.\n\n"
                "TIME-WINDOW DISPLAY: This oscilloscope shows a fixed time window. A low-pitched "
                "note fills fewer cycles in the window, while a high-pitched note shows many "
                "tightly packed cycles. This is how real hardware oscilloscopes work.\n\n"
                "PHASE STABILITY: The zero-crossing trigger keeps the waveform locked in place. "
                "It searches backwards for a rising zero-crossing to anchor the display, preventing "
                "the waveform from scrolling randomly.";
            g.drawFittedText(concepts, area.removeFromTop(240), juce::Justification::topLeft, 14);

            // ========================================
            // SECTION: DATA PIPELINE
            // ========================================
            drawSectionTitle(g, area, "DATA PIPELINE (ALTW PACKET)");

            g.setFont(juce::Font(13.0f));
            g.setColour(juce::Colours::lightgrey);
            juce::String pipeline =
                "The oscilloscope receives raw audio data via UDP from the AlterListener VST plugin:\n\n"
                "1. VST Plugin captures 4096 stereo samples from the DAW audio buffer\n"
                "2. Samples are interleaved [L0, R0, L1, R1, ...] into a raw buffer\n"
                "3. Sent as ALTW packet: 4-byte header 'ALTW' + 4096 x 2ch x 4 bytes = 32772 bytes\n"
                "4. UdpReceiver parses the packet with memcpy + deinterleaves into L/R arrays\n"
                "5. Oscilloscope reads these arrays at ~60 FPS for display\n\n"
                "The same pipeline also works with System Audio (WASAPI Loopback on Windows), "
                "where the captured system audio is treated as mono (L = R).";
            g.drawFittedText(pipeline, area.removeFromTop(230), juce::Justification::topLeft, 14);
            area.removeFromTop(10);


            // ========================================
            // SECTION: RENDERING
            // ========================================
            drawSectionTitle (g, area, "RENDERING PIPELINE");

            g.setFont (juce::Font (13.0f));
            g.setColour (juce::Colours::lightgrey);
            juce::String rendering =
                "The waveform is rendered in several stages:\n\n"
                "1. ZERO-CROSSING TRIGGER: Finds a rising zero-crossing point in the audio "
                "buffer to start the display. This keeps the waveform visually stable.\n\n"
                "2. CATMULL-ROM INTERPOLATION: Raw audio samples are resampled to screen "
                "resolution using cubic Catmull-Rom splines. This produces smooth, "
                "anti-aliased curves even when zoomed in.\n\n"
                "3. EMA SMOOTHING: Exponential Moving Average smoothing is applied between "
                "frames to reduce visual jitter. Controlled by the Smooth slider (0 = no "
                "smoothing, 1 = very slow response).\n\n"
                "4. NORMALIZATION: The waveform is normalized to fill the display height, "
                "preventing very quiet signals from appearing as flat lines.\n\n"
                "5. STROKE RENDERING: The final waveform is drawn as a plain colored stroke "
                "(2.0px main line, 1.2px secondary). Optionally, Fill mode adds a "
                "semi-transparent area under the curve (alpha 0.25).";
            g.drawFittedText (rendering, area.removeFromTop (340), juce::Justification::topLeft, 20);
            area.removeFromTop (10);
        }

    private:
        void drawSectionTitle (juce::Graphics& g, juce::Rectangle<int>& area, const juce::String& title)
        {
            g.setFont (juce::Font (18.0f, juce::Font::bold));
            g.setColour (juce::Colours::white);
            g.drawText (title, area.removeFromTop (30), juce::Justification::left);
            area.removeFromTop (8);
        }
    };
};
