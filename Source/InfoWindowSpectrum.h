/*
  ==============================================================================

    InfoWindowSpectrum.h
    Created: 29 Mar 2026 1:53:13pm
    Author:  Martin

  ==============================================================================
*/

#pragma once
#include "InfoWindowAudioMeters.h"

class SpectrumInfoWindow : public ModuleInfoWindow
{
public:
    SpectrumInfoWindow()
        : ModuleInfoWindow ("Spectrum Analyzer - Educational Guide")
    {
        auto* content = new ContentPanel();
        auto* viewport = new juce::Viewport();
        viewport->setViewedComponent (content, true);
        viewport->setScrollBarsShown (true, false);
        setContentOwned (viewport, true);
        setSize (600, 900);
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
        ContentPanel() { setSize (600, 400); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xFF1E1E1E));
            auto area = getLocalBounds().reduced (20);

            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (24.0f, juce::Font::bold));
            g.drawText ("Spectrum Analyzer", area.removeFromTop (40), juce::Justification::centred);
            area.removeFromTop (20);

            g.setFont (juce::Font (14.0f));
            g.setColour (juce::Colours::lightgrey);
            g.drawText ("Coming soon...", area.removeFromTop (30), juce::Justification::centred);
        }
    };
};
