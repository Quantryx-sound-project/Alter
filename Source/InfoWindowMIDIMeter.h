/*
  ==============================================================================

    InfoWindowMIDIMeter.h
    Created: 25 Apr 2026 8:39:00pm
    Author:  Quantryx

    MIDI Chord/Note display window - integrates MIDIMeter into HUD layout

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include "MIDIMeter.h"
#include "AlterState.h"

class InfoWindowMIDIMeter : public juce::Component,
                            public juce::ValueTree::Listener
{
public:
    InfoWindowMIDIMeter (UdpReceiver& udpReceiver, AlterState& state, int panelId)
        : receiver (udpReceiver),
          alterState (state),
          panelId (panelId),
          panelTree (alterState.getPanelById (panelId))
    {
        if (panelTree.isValid())
        {
            panelTree.addListener (this);
        }

        // Create MIDI meter component
        midiMeter = std::make_unique<MIDIMeter> (receiver);
        addAndMakeVisible (*midiMeter);

        setInterceptsMouseClicks (false, false);
    }

    ~InfoWindowMIDIMeter() override
    {
        if (panelTree.isValid())
        {
            panelTree.removeListener (this);
        }
    }

    void resized() override
    {
        if (midiMeter)
        {
            midiMeter->setBounds (getLocalBounds());
        }
    }

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override
    {
        // Handle property changes from state
        repaint();
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

private:
    UdpReceiver& receiver;
    AlterState& alterState;
    int panelId;
    juce::ValueTree panelTree;

    std::unique_ptr<MIDIMeter> midiMeter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InfoWindowMIDIMeter)
};
