#include "ControllerWindow.h"
#include "InfoWindowOscilator.h"
#include "InfoWindowSpectrum.h"
#include "InfoWindowSynesthesia.h"

// ===== ControllerContent =====

ControllerContent::ControllerContent (AlterState& s) : settings (s)
{
    startTimerHz (2);
    // LEFT: Add button + list
    addAndMakeVisible (btnAdd);
    btnAdd.onClick = [this]
    {
        juce::PopupMenu m;
        m.addItem (1, "Audio Meter");
        m.addItem (2, "Spectrum");
        m.addItem (3, "Oscillator");
        m.addItem (4, "Synesthesia");

        m.showMenuAsync (juce::PopupMenu::Options(),
                         [this](int res)
                         {
                             if (res == 0) return;

                             if (res == 1) settings.addPanel ("audiometer");
                             if (res == 2) settings.addPanel ("spectrum");
                             if (res == 3) settings.addPanel ("oscillator");
                             if (res == 4) settings.addPanel ("synesthesia");

                             panelList.updateContent();
                             panelList.repaint();
                         });
    };

    addAndMakeVisible (panelList);
    panelList.setModel (this);
    panelList.setRowHeight (26);

    // Enable drag & drop reordering
    dragOverlay.setAlwaysOnTop (true);
    dragOverlay.setVisible (false);
    dragOverlay.setInterceptsMouseClicks (false, false);

    // RIGHT: Delete button
    addAndMakeVisible (btnDelete);
    btnDelete.onClick = [this]
    {
        if (selectedPanelId < 0) return;

        settings.removePanelById (selectedPanelId);

        selectedPanelId = -1;
        panelList.deselectAllRows();
        panelList.updateContent();

        resized();
        repaint();
    };

    // Get Info button (educational guide)
    addAndMakeVisible(btnGetInfo);
    btnGetInfo.onClick = [this]
        {
            // Check if any panel is selected
            if (selectedPanelId < 0) return;

            // Get selected panel
            auto panel = settings.getPanelById(selectedPanelId);
            if (!panel.isValid()) return;

            // Get module type
            const auto type = panel.getProperty(AlterState::kType).toString();

            // Open info window based on module type
            if (type == "audiometer")
            {
                if (currentInfoWindow != nullptr)
                    delete currentInfoWindow.getComponent();
                currentInfoWindow = new AudioMeterInfoWindow();
                if (alwaysOnTop.getToggleState())
                    currentInfoWindow->setAlwaysOnTop (true);
            }
            else if (type == "spectrum")
            {
                if (currentInfoWindow != nullptr)
                    delete currentInfoWindow.getComponent();
                currentInfoWindow = new SpectrumInfoWindow();
                if (alwaysOnTop.getToggleState())
                    currentInfoWindow->setAlwaysOnTop (true);
            }
            else if (type == "oscillator" || type == "oscilator")
            {
                if (currentInfoWindow != nullptr)
                    delete currentInfoWindow.getComponent();
                currentInfoWindow = new OscillatorInfoWindow();
                if (alwaysOnTop.getToggleState())
                    currentInfoWindow->setAlwaysOnTop (true);
            }
            else if (type == "synesthesia")
            {
                if (currentInfoWindow != nullptr)
                    delete currentInfoWindow.getComponent();
                currentInfoWindow = new SynesthesiaInfoWindow();
                if (alwaysOnTop.getToggleState())
                    currentInfoWindow->setAlwaysOnTop (true);
            }
        };

    // RMS editor
    addAndMakeVisible (lblRmsMode);
    lblRmsMode.setText ("Mode", juce::dontSendNotification);
    lblRmsMode.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (cbRmsPeakMode);
    cbRmsPeakMode.addItem ("RMS (Average)", 1);
    cbRmsPeakMode.addItem ("True Peak", 2);
    cbRmsPeakMode.addItem ("LUFS (Loudness)", 3);
    cbRmsPeakMode.setSelectedId (1, juce::dontSendNotification);
    cbRmsPeakMode.onChange = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        // 1 = RMS, 2 = True Peak, 3 = LUFS
        panel.setProperty (AlterState::kMeterMode, cbRmsPeakMode.getSelectedId() - 1, nullptr);
    };

    addAndMakeVisible (lblRmsSmooth);
    lblRmsSmooth.setText ("Smooth", juce::dontSendNotification);
    lblRmsSmooth.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (sRmsSmooth);
    sRmsSmooth.setSliderStyle (juce::Slider::LinearHorizontal);
    sRmsSmooth.setRange (0.0, 1.0, 0.001);
    sRmsSmooth.onValueChange = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kSmooth, (float) sRmsSmooth.getValue(), nullptr);
    };

    // Spectrum editor
    addAndMakeVisible (specAWeight);
    specAWeight.setButtonText ("A-weight");
    specAWeight.setClickingTogglesState (true);
    specAWeight.onClick = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kAWeight, specAWeight.getToggleState(), nullptr);
    };

    addAndMakeVisible (lblSpecSmooth);
    lblSpecSmooth.setText ("Smooth", juce::dontSendNotification);
    lblSpecSmooth.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (sSpecSmooth);
    sSpecSmooth.setSliderStyle (juce::Slider::LinearHorizontal);
    sSpecSmooth.setRange (0.0, 1.0, 0.001);
    sSpecSmooth.onValueChange = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kSmooth, (float) sSpecSmooth.getValue(), nullptr);
    };

    // Oscillator editor
    addAndMakeVisible (lblOscSmooth);
    lblOscSmooth.setText ("Smooth", juce::dontSendNotification);
    lblOscSmooth.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (sOscSmooth);
    sOscSmooth.setSliderStyle (juce::Slider::LinearHorizontal);
    sOscSmooth.setRange (0.0, 1.0, 0.001);
    sOscSmooth.onValueChange = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kSmooth, (float) sOscSmooth.getValue(), nullptr);
    };

    addAndMakeVisible (tbFill);
    tbFill.setButtonText ("Fill");
    tbFill.setClickingTogglesState (true);
    tbFill.onClick = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kNeon, tbFill.getToggleState(), nullptr);
    };

    addAndMakeVisible (lblDisplayMode);
    lblDisplayMode.setText ("Display", juce::dontSendNotification);
    lblDisplayMode.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (cbDisplayMode);
    cbDisplayMode.addItem ("Mono", 1);
    cbDisplayMode.addItem ("Stereo", 2);
    cbDisplayMode.addItem ("Mirror", 3);
    cbDisplayMode.onChange = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kDisplayMode, juce::jlimit (0, 2, cbDisplayMode.getSelectedId() - 1), nullptr);
    };

    // Oscillator zoom (time window)
    addAndMakeVisible (lblOscZoom);
    lblOscZoom.setText ("Zoom", juce::dontSendNotification);
    lblOscZoom.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (sOscZoom);
    sOscZoom.setSliderStyle (juce::Slider::LinearHorizontal);
    sOscZoom.setRange (0.0, 1.0, 0.001);
    sOscZoom.setValue (0.420);
    sOscZoom.setDoubleClickReturnValue (true, 0.420);
    sOscZoom.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    sOscZoom.textFromValueFunction = [](double v)
    {
        const double twMs = 85.0 * std::pow (0.001176, v);
        if (twMs >= 1.0)
            return juce::String (twMs, 1) + " ms";
        return juce::String (twMs * 1000.0, 0) + juce::String::charToString (0x00B5) + "s";
    };
    sOscZoom.valueFromTextFunction = [](const juce::String& text)
    {
        double ms = text.getDoubleValue();
        if (text.containsIgnoreCase ("us") || text.containsChar (0x00B5))
            ms *= 0.001;
        if (ms <= 0.0) return 0.420;
        return juce::jlimit (0.0, 1.0, std::log (ms / 85.0) / std::log (0.001176));
    };
    sOscZoom.onValueChange = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kZoom, (float) sOscZoom.getValue(), nullptr);
    };

    // Synesthesia editor
    addAndMakeVisible (lblSynSmooth);
    lblSynSmooth.setText ("Smooth", juce::dontSendNotification);
    lblSynSmooth.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (sSynSmooth);
    sSynSmooth.setSliderStyle (juce::Slider::LinearHorizontal);
    sSynSmooth.setRange (0.0, 1.0, 0.001);
    sSynSmooth.onValueChange = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kSmooth, (float) sSynSmooth.getValue(), nullptr);
    };

    addAndMakeVisible (lblBPM);
    lblBPM.setText ("BPM: 120", juce::dontSendNotification);
    lblBPM.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (tbSyncBPM);
    tbSyncBPM.setButtonText ("Sync to BPM");
    tbSyncBPM.setClickingTogglesState (true);
    tbSyncBPM.setToggleState (true, juce::dontSendNotification);
    tbSyncBPM.onClick = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kSyncBPM, tbSyncBPM.getToggleState(), nullptr);
    };

    addAndMakeVisible (lblZoom);
    lblZoom.setText ("Zoom", juce::dontSendNotification);
    lblZoom.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (sZoom);
    sZoom.setSliderStyle (juce::Slider::LinearHorizontal);
    sZoom.setRange (0.5, 2.0, 0.01);
    sZoom.setValue (1.0);
    sZoom.onValueChange = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kZoom, (float) sZoom.getValue(), nullptr);
    };

    addAndMakeVisible (lblRotation);
    lblRotation.setText ("Rotation", juce::dontSendNotification);
    lblRotation.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (sRotation);
    sRotation.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    sRotation.setRange (0.0, 360.0, 1.0);
    sRotation.setValue (0.0);
    sRotation.onValueChange = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kRotation, (float) sRotation.getValue(), nullptr);
    };

    addAndMakeVisible (lblSymmetry);
    lblSymmetry.setText ("Symmetry", juce::dontSendNotification);
    lblSymmetry.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (sSymmetry);
    sSymmetry.setSliderStyle (juce::Slider::IncDecButtons);
    sSymmetry.setRange (1, 8, 1);
    sSymmetry.setValue (1);
    sSymmetry.onValueChange = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kSymmetry, (int) sSymmetry.getValue(), nullptr);
    };

    addAndMakeVisible (lblSaturation);
    lblSaturation.setText ("Saturation", juce::dontSendNotification);
    lblSaturation.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (sSaturation);
    sSaturation.setSliderStyle (juce::Slider::LinearHorizontal);
    sSaturation.setRange (0.0, 2.0, 0.01);
    sSaturation.setValue (1.0);
    sSaturation.onValueChange = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kSaturation, (float) sSaturation.getValue(), nullptr);
    };

    addAndMakeVisible (lblBloom);
    lblBloom.setText ("Bloom", juce::dontSendNotification);
    lblBloom.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (sBloom);
    sBloom.setSliderStyle (juce::Slider::LinearHorizontal);
    sBloom.setRange (0.0, 1.0, 0.01);
    sBloom.setValue (0.0);
    sBloom.onValueChange = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kBloom, (float) sBloom.getValue(), nullptr);
    };

    addAndMakeVisible (lblBins);
    lblBins.setText ("Bins", juce::dontSendNotification);
    lblBins.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (cbBins);
    cbBins.addItem ("512", 512);
    cbBins.addItem ("1024", 1024);
    cbBins.addItem ("2048", 2048);
    cbBins.onChange = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;
        panel.setProperty (AlterState::kBins, cbBins.getSelectedId(), nullptr);
    };

    // FOOTER
    addAndMakeVisible (alwaysOnTop);
    alwaysOnTop.setButtonText ("Always on top");
    alwaysOnTop.setClickingTogglesState (true);
    alwaysOnTop.setToggleState (settings.controllerAlwaysOnTop(), juce::dontSendNotification);
    alwaysOnTop.onClick = [this]
    {
        settings.setControllerAlwaysOnTop (alwaysOnTop.getToggleState());
        if (onAlwaysOnTopChanged) onAlwaysOnTopChanged (alwaysOnTop.getToggleState());
    };

    addAndMakeVisible (lblAudioMode);
    lblAudioMode.setText ("Audio Input", juce::dontSendNotification);
    lblAudioMode.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    lblAudioMode.setJustificationType (juce::Justification::centred);

    addAndMakeVisible (cbAudioMode);
    cbAudioMode.addItem ("VST Plugin", 1);

    // System Audio only available on Windows
    #if JUCE_WINDOWS
        cbAudioMode.addItem ("System Audio", 2);
    #else
        cbAudioMode.addItem ("System Audio (Windows only)", 2);
    #endif

    cbAudioMode.setSelectedId (1, juce::dontSendNotification); // Default: VST Plugin

    // Platform-specific tooltip
    #if JUCE_WINDOWS
        cbAudioMode.setTooltip ("Choose audio input source:\n"
                               "- VST Plugin: Receive from DAW (Ableton, FL Studio, etc.)\n"
                               "- System Audio: Capture from Windows (Spotify, YouTube, etc.)");
    #else
        cbAudioMode.setTooltip ("Choose audio input source:\n"
                               "- VST Plugin: Receive from DAW (works on all platforms)\n"
                               "- System Audio: Not available on macOS/Linux");
    #endif

    cbAudioMode.onChange = [this]
    {
        const int mode = cbAudioMode.getSelectedId();

        #if !JUCE_WINDOWS
            // Block System Audio selection on non-Windows platforms
            if (mode == 2)
            {

                juce::NativeMessageBox::showMessageBoxAsync (
                    juce::MessageBoxIconType::WarningIcon,
                    "Feature Not Available",
                    "System Audio Capture is only available on Windows.\n\n"
                    "On macOS/Linux, please use VST Plugin mode instead.\n"
                    "(VST Plugin works identically on all platforms)",
                    nullptr
                );

                // Reset to VST Plugin
                cbAudioMode.setSelectedId (1, juce::dontSendNotification);
                return;
            }
        #endif

        DBG ("Audio Mode changed to: " + juce::String (mode == 1 ? "VST Plugin" : "System Audio"));

        // Call external callback if set
        if (onAudioModeChangedInternal)
            onAudioModeChangedInternal (mode);

        resized();  // re-layout footer (show/hide gain knob)
    };

    addAndMakeVisible (btnQuit);
    btnQuit.setButtonText ("Quit");
    btnQuit.onClick = [this]
        {
            if (currentInfoWindow != nullptr)
                delete currentInfoWindow.getComponent();
            juce::JUCEApplicationBase::quit();
        };

    addAndMakeVisible(btnClose);
    btnClose.onClick = [this]
        {
            if (onCloseRequested) onCloseRequested();
        };

    // Color UI: label, preview, button
    addAndMakeVisible (lblColor);
    lblColor.setText ("Color", juce::dontSendNotification);
    lblColor.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (lblColorPreview);
    lblColorPreview.setText ("", juce::dontSendNotification);
    lblColorPreview.setOpaque (true);
    lblColorPreview.setColour (juce::Label::backgroundColourId, juce::Colours::red);
    lblColorPreview.setColour (juce::Label::textColourId, juce::Colours::transparentBlack);

    addAndMakeVisible (btnColor);
    btnColor.onClick = [this]
    {
        // launch modal CallOutBox with ColourSelector
        struct ColorPopup  : public juce::Component
        {
            ColorPopup (juce::Colour initial, std::function<void(juce::Colour)> cb)
                : onOk (std::move (cb))
            {
                addAndMakeVisible (cs);
                cs.setCurrentColour (initial);
                cs.setColour (juce::ColourSelector::backgroundColourId, juce::Colours::black);

                addAndMakeVisible (ok);
                addAndMakeVisible (cancel);
                ok.setButtonText ("OK");
                cancel.setButtonText ("Cancel");

                ok.onClick = [this]() {
                    if (onOk) onOk (cs.getCurrentColour());
                    if (auto* co = findParentComponentOfClass<juce::CallOutBox>()) co->dismiss();
                };
                cancel.onClick = [this]() {
                    if (auto* co = findParentComponentOfClass<juce::CallOutBox>()) co->dismiss();
                };

                setSize (340, 300);
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced (6);
                cs.setBounds (r.removeFromTop (r.getHeight() - 28));
                auto b = r.removeFromTop (28);
                ok.setBounds (b.removeFromLeft (80));
                cancel.setBounds (b.removeFromLeft (80));
            }

            juce::ColourSelector cs { juce::ColourSelector::showColourAtTop | juce::ColourSelector::showSliders }; 
            juce::TextButton ok, cancel;
            std::function<void(juce::Colour)> onOk;
        };

        juce::Colour initial = juce::Colours::red;
        if (selectedPanelId >= 0)
        {
            auto p = settings.getPanelById (selectedPanelId);
            if (p.isValid() && p.hasProperty (AlterState::kColor))
                initial = juce::Colour ((uint32_t) (int) p.getProperty (AlterState::kColor));
        }

        auto popup = std::make_unique<ColorPopup> (initial, [this] (juce::Colour c)
        {
            if (selectedPanelId >= 0)
            {
                auto p = settings.getPanelById (selectedPanelId);
                if (p.isValid())
                {
                    p.setProperty (AlterState::kColor, (int) c.getARGB(), nullptr);
                    lblColorPreview.setColour (juce::Label::backgroundColourId, c);
                }
            }
        });

        juce::CallOutBox::launchAsynchronously (std::move (popup), btnColor.getScreenBounds(), this);
    };

    // Color mode selector (Standard / Custom)
    addAndMakeVisible (lblColorMode);
    lblColorMode.setText ("Color Mode", juce::dontSendNotification);
    lblColorMode.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (cbColorMode);
    cbColorMode.addItem ("Standard", 1);
    cbColorMode.addItem ("Custom Gradient", 2);
    cbColorMode.addItem ("Custom Spectrum", 3);
    cbColorMode.setSelectedId (1, juce::dontSendNotification);  // Default: Standard
    cbColorMode.onChange = [this]
    {
        auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId) : juce::ValueTree();
        if (! panel.isValid()) return;

        // Store mode: 0 = Standard, 1 = Custom Gradient, 2 = Custom Spectrum
        const int mode = cbColorMode.getSelectedId() - 1;
        panel.setProperty (AlterState::kColorMode, mode, nullptr);

        // Legacy support: kCustomColorMode = true if NOT Standard
        panel.setProperty (AlterState::kCustomColorMode, (mode != 0), nullptr);

        // Refresh visibility (show/hide color picker based on mode)
        resized();
    };

    // Module rotation button (cycles through 0°, 90°, 180°, 270°)
    addAndMakeVisible (lblModuleRotation);
    lblModuleRotation.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

    addAndMakeVisible (btnRotate);
    btnRotate.onClick = [this]
    {
        if (selectedPanelId < 0) return;

        auto panel = settings.getPanelById (selectedPanelId);
        if (! panel.isValid()) return;

        // Check if this is Synesthesia (OpenGL component - rotation not supported)
        const auto type = panel.getProperty (AlterState::kType).toString();
        if (type == "synesthesia")
        {
            // Rotation doesn't work on OpenGL components in JUCE
            juce::AlertWindow::showMessageBoxAsync (
                juce::AlertWindow::InfoIcon,
                "Rotation Not Supported",
                "Rotation is not supported for Synesthesia module (OpenGL rendering).",
                "OK");
            return;
        }

        // Get current rotation (0-3)
        int currentRotation = (int) panel.getProperty (AlterState::kRotationAngle, 0);

        // Cycle to next rotation
        int nextRotation = (currentRotation + 1) % 4;
        panel.setProperty (AlterState::kRotationAngle, nextRotation, nullptr);

        // Update button text (ASCII safe - no Unicode degree symbols)
        const char* rotationText[] = { "0 deg", "90 deg", "180 deg", "270 deg" };
        btnRotate.setButtonText (rotationText[nextRotation]);
    };

    // Double-click to reset sliders to default value (VST plugin standard)
    sRmsSmooth.setDoubleClickReturnValue (true, 0.5);
    sSpecSmooth.setDoubleClickReturnValue (true, 0.5);
    sOscSmooth.setDoubleClickReturnValue (true, 0.5);
    sSynSmooth.setDoubleClickReturnValue (true, 0.15);
    sZoom.setDoubleClickReturnValue (true, 1.0);
    sRotation.setDoubleClickReturnValue (true, 0.0);
    sSymmetry.setDoubleClickReturnValue (true, 1.0);
    sSaturation.setDoubleClickReturnValue (true, 1.0);
    sBloom.setDoubleClickReturnValue (true, 0.0);

    // Custom rotary knob look (VST-style circle with pointer)
    sRotation.setLookAndFeel (&knobLAF);
    sRotation.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    sRotation.setPopupDisplayEnabled (true, false, this);

    // Gain knob for System Audio mode (rotary, classic DAW style)
    addAndMakeVisible (lblGain);
    lblGain.setText ("Gain", juce::dontSendNotification);
    lblGain.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    lblGain.setJustificationType (juce::Justification::centred);
    lblGain.setVisible (false);

    addAndMakeVisible (sGain);
    sGain.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    sGain.setRange (0.0, 20.0, 0.5);
    sGain.setValue (0.0, juce::dontSendNotification);
    sGain.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    sGain.setPopupDisplayEnabled (true, false, this);
    sGain.setTextValueSuffix (" dB");
    sGain.setDoubleClickReturnValue (true, 0.0);
    sGain.setLookAndFeel (&knobLAF);
    sGain.onValueChange = [this]
    {
        settings.getTree().setProperty (AlterState::kSystemGain, (float) sGain.getValue(), nullptr);
    };
    sGain.setVisible (false);

    // Setup scrollable editor viewport for right panel
    editorViewport.setViewedComponent (&editorPanel, false);
    editorViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (editorViewport);

    // Reparent all editor widgets to editorPanel (for scrollable right panel)
    auto moveToEditor = [this](juce::Component& c) {
        removeChildComponent (&c);
        editorPanel.addAndMakeVisible (c);
    };
    moveToEditor (btnDelete);  moveToEditor (btnGetInfo);
    moveToEditor (lblModuleRotation);  moveToEditor (btnRotate);
    moveToEditor (lblRmsMode);  moveToEditor (cbRmsPeakMode);
    moveToEditor (lblRmsSmooth);  moveToEditor (sRmsSmooth);
    moveToEditor (specAWeight);
    moveToEditor (lblSpecSmooth);  moveToEditor (sSpecSmooth);
    moveToEditor (lblBins);  moveToEditor (cbBins);
    moveToEditor (lblOscSmooth);  moveToEditor (sOscSmooth);
    moveToEditor (tbFill);  moveToEditor (lblDisplayMode);
    moveToEditor (cbDisplayMode);
    moveToEditor (lblOscZoom);  moveToEditor (sOscZoom);
    moveToEditor (lblSynSmooth);  moveToEditor (sSynSmooth);
    moveToEditor (lblBPM);  moveToEditor (tbSyncBPM);
    moveToEditor (lblZoom);  moveToEditor (sZoom);
    moveToEditor (lblRotation);  moveToEditor (sRotation);
    moveToEditor (lblSymmetry);  moveToEditor (sSymmetry);
    moveToEditor (lblSaturation);  moveToEditor (sSaturation);
    moveToEditor (lblBloom);  moveToEditor (sBloom);
    moveToEditor (lblColor);  moveToEditor (lblColorPreview);  moveToEditor (btnColor);
    moveToEditor (lblColorMode);  moveToEditor (cbColorMode);

    // initial state
    refreshRightEditorFromSelection();
    resized();
}

ControllerContent::~ControllerContent()
{
    sGain.setLookAndFeel (nullptr);
    sRotation.setLookAndFeel (nullptr);
}

void ControllerContent::timerCallback()
{
    // Update BPM label if synesthesia panel is selected
    if (selectedPanelId < 0) return;

    auto panel = settings.getPanelById (selectedPanelId);
    if (! panel.isValid()) return;

    const auto type = panel.getProperty (AlterState::kType).toString();
    if (type != "synesthesia") return;

    // Get BPM from UDP (udp receiver needs to be accessible)
    // For now, just update the label text - need MainComponent reference for udp
}

void ControllerContent::resized()
{
    auto r = getLocalBounds().reduced (10);
    const int row = 28;
    const int gap = 6;

    // footer (2 rows: label + controls)
    const int footerHeight = row * 2 + gap + 4;
    auto footer = r.removeFromBottom (footerHeight);
    auto labelRow = footer.removeFromTop (row);
    footer.removeFromTop (gap);

    // Bottom row: position controls first (right to left)
    auto controlRow = footer;
    btnClose.setBounds(controlRow.removeFromRight(80));
    controlRow.removeFromRight(6);
    btnQuit.setBounds(controlRow.removeFromRight(80));
    controlRow.removeFromRight(10);

    // Gain knob (visible only in System Audio mode)
    const bool showGain = (cbAudioMode.getSelectedId() == 2);
    sGain.setVisible (showGain);
    lblGain.setVisible (showGain);
    if (showGain)
    {
        sGain.setBounds (controlRow.removeFromRight (55));
        controlRow.removeFromRight (4);
    }

    cbAudioMode.setBounds (controlRow.removeFromRight (130));
    controlRow.removeFromRight (10);
    alwaysOnTop.setBounds (controlRow.removeFromLeft (160));

    // Top row: labels aligned with their controls
    lblAudioMode.setBounds (cbAudioMode.getX(), labelRow.getY(), cbAudioMode.getWidth(), row);
    if (showGain)
        lblGain.setBounds (sGain.getX(), labelRow.getY(), sGain.getWidth(), row);

    // left
    auto left = r.removeFromLeft (180);
    btnAdd.setBounds (left.removeFromTop (row));
    left.removeFromTop (gap);
    panelList.setBounds (left);

    // right editor area (scrollable via viewport)
    auto rightArea = r.reduced (10);
    editorViewport.setBounds (rightArea);
    const int panelW = juce::jmax (1, rightArea.getWidth() - editorViewport.getScrollBarThickness());

    // nothing selected
    if (selectedPanelId < 0)
    {
        btnDelete.setVisible (false);
        btnGetInfo.setVisible (false);
        setEditorVisible (false, false, false, false);
        editorPanel.setSize (panelW, rightArea.getHeight());
        return;
    }

    // Layout in editorPanel coordinates (0,0 origin, tall virtual height)
    const int virtualH = 10000;
    auto p = juce::Rectangle<int> (0, 0, panelW, virtualH);

    // Show both Delete and Explore buttons
    btnDelete.setVisible(true);
    btnGetInfo.setVisible(true);

    // Layout: [Delete] [Explore] on same row
    auto deleteRow = p.removeFromTop(row);
    btnDelete.setBounds(deleteRow.removeFromLeft(100));
    deleteRow.removeFromLeft(6);  // gap between buttons
    btnGetInfo.setBounds(deleteRow.removeFromLeft(100));

    p.removeFromTop(gap);

    // Module rotation (common for all modules)
    auto rotRow = p.removeFromTop (row);
    lblModuleRotation.setBounds (rotRow.removeFromLeft (70));
    rotRow.removeFromLeft (6);
    btnRotate.setBounds (rotRow.removeFromLeft (60));
    p.removeFromTop (gap);

    auto panel = settings.getPanelById (selectedPanelId);
    const auto type = panel.isValid() ? panel.getProperty (AlterState::kType).toString() : juce::String();

    const bool isAudioMeter  = (type == "audiometer" || type == "rms");
    const bool isSpec = (type == "spectrum");
    const bool isOsc  = (type == "oscillator" || type == "oscilator");
    const bool isSyn  = (type == "synesthesia");

    setEditorVisible (isAudioMeter, isSpec, isOsc, isSyn);

    if (isAudioMeter)
    {
        // Mode selector (RMS vs True Peak vs LUFS)
        auto a = p.removeFromTop (row);
        lblRmsMode.setBounds (a.removeFromLeft (70));
        a.removeFromLeft (6);
        cbRmsPeakMode.setBounds (a.removeFromLeft (140));

        p.removeFromTop (gap);

        // Smooth slider
        a = p.removeFromTop (row);
        lblRmsSmooth.setBounds (a.removeFromLeft (70));
        a.removeFromLeft (6);
        sRmsSmooth.setBounds (a);

        p.removeFromTop (gap);

        // Color Mode selector
        a = p.removeFromTop (row);
        lblColorMode.setBounds (a.removeFromLeft (70));
        a.removeFromLeft (6);
        cbColorMode.setBounds (a.removeFromLeft (120));

        // Show color picker in Custom Gradient OR Custom Spectrum mode
        const int colorMode = (int) panel.getProperty (AlterState::kColorMode, 0);  // 0=Standard, 1=Gradient, 2=Spectrum
        if (colorMode != 0)  // Not Standard
        {
            p.removeFromTop (gap);

            // Color picker (visible in Custom modes only)
            a = p.removeFromTop (row);
            lblColor.setBounds (a.removeFromLeft (70));
            lblColorPreview.setBounds (a.removeFromLeft (24));
            a.removeFromLeft (6);
            btnColor.setBounds (a.removeFromLeft (90));
        }
    }

    else if (isSyn)
    {
        auto a = p.removeFromTop (row);
        lblSynSmooth.setBounds (a.removeFromLeft (70));
        a.removeFromLeft (6);
        sSynSmooth.setBounds (a);

        p.removeFromTop (gap);

        a = p.removeFromTop (row);
        lblBPM.setBounds (a.removeFromLeft (120));
        a.removeFromLeft (6);
        tbSyncBPM.setBounds (a.removeFromLeft (120));

        p.removeFromTop (gap);

        a = p.removeFromTop (row);
        lblZoom.setBounds (a.removeFromLeft (70));
        a.removeFromLeft (6);
        sZoom.setBounds (a);

        p.removeFromTop (gap);

        a = p.removeFromTop (row);
        lblRotation.setBounds (a.removeFromLeft (70));
        a.removeFromLeft (6);
        sRotation.setBounds (a.removeFromLeft (80));

        p.removeFromTop (gap);

        a = p.removeFromTop (row);
        lblSymmetry.setBounds (a.removeFromLeft (70));
        a.removeFromLeft (6);
        sSymmetry.setBounds (a.removeFromLeft (80));

        p.removeFromTop (gap);

        a = p.removeFromTop (row);
        lblSaturation.setBounds (a.removeFromLeft (70));
        a.removeFromLeft (6);
        sSaturation.setBounds (a);

        p.removeFromTop (gap);

        a = p.removeFromTop (row);
        lblBloom.setBounds (a.removeFromLeft (70));
        a.removeFromLeft (6);
        sBloom.setBounds (a);

        // Color picker removed from synesthesia
    }

    else if (isOsc)
    {
        auto a = p.removeFromTop (row);
        lblOscSmooth.setBounds (a.removeFromLeft (70));
        a.removeFromLeft (6);
        sOscSmooth.setBounds (a);

        p.removeFromTop (gap);

        a = p.removeFromTop (row);
        tbFill.setBounds (a.removeFromLeft (120));

        p.removeFromTop (gap);

        // Display mode (Mono/Stereo/Mirror)
        a = p.removeFromTop (row);
        lblDisplayMode.setBounds (a.removeFromLeft (70));
        a.removeFromLeft (6);
        cbDisplayMode.setBounds (a.removeFromLeft (120));

        p.removeFromTop (gap);

        // Zoom (time window)
        a = p.removeFromTop (row);
        lblOscZoom.setBounds (a.removeFromLeft (70));
        a.removeFromLeft (6);
        sOscZoom.setBounds (a);

        p.removeFromTop (gap);

        a = p.removeFromTop (row);
        lblColor.setBounds (a.removeFromLeft (70));
        lblColorPreview.setBounds (a.removeFromLeft (24));
        a.removeFromLeft (6);
        btnColor.setBounds (a.removeFromLeft (90));
    }
    else if (isSpec)
    {
        auto a = p.removeFromTop (row);
        specAWeight.setBounds (a.removeFromLeft (120));

        p.removeFromTop (gap);

        a = p.removeFromTop (row);
        lblSpecSmooth.setBounds (a.removeFromLeft (70));
        a.removeFromLeft (6);
        sSpecSmooth.setBounds (a);

        p.removeFromTop (gap);

        a = p.removeFromTop (row);
        lblBins.setBounds (a.removeFromLeft (70));
        cbBins.setBounds (a.removeFromLeft (100));

        p.removeFromTop (gap);

        a = p.removeFromTop (row);
        lblColor.setBounds (a.removeFromLeft (70));
        lblColorPreview.setBounds (a.removeFromLeft (24));
        a.removeFromLeft (6);
        btnColor.setBounds (a.removeFromLeft (90));
    }

    // Set scrollable editor panel content size
    const int usedHeight = virtualH - p.getHeight();
    editorPanel.setSize (panelW, juce::jmax (rightArea.getHeight(), usedHeight));

    // (no inline selector)
}

void ControllerContent::setInfoWindowAlwaysOnTop (bool on)
{
    if (currentInfoWindow != nullptr)
        currentInfoWindow->setAlwaysOnTop (on);
}

// ===== ListBoxModel =====

int ControllerContent::getNumRows()
{
    auto panels = settings.getPanelsRoot();
    return panels.isValid() ? panels.getNumChildren() : 0;
}

juce::var ControllerContent::getDragSourceDescription (const juce::SparseSet<int>& selectedRows)
{
    if (selectedRows.size() == 1)
        return "panel_" + juce::String (selectedRows[0]);
    return {};
}

void ControllerContent::paintListBoxItem (int rowNumber, juce::Graphics& g,
                                          int width, int height, bool rowIsSelected)
{
    auto panels = settings.getPanelsRoot();
    if (! panels.isValid() || rowNumber < 0 || rowNumber >= panels.getNumChildren())
        return;

    auto p = panels.getChild (rowNumber);
    const int id = (int) p.getProperty (AlterState::kId);
    const auto type = p.getProperty (AlterState::kType).toString();

    // Highlight being dragged
    if (isDragging && rowNumber == draggedRow)
    {
        g.setColour (juce::Colours::grey.withAlpha (0.3f));
        g.fillAll();
    }
    else if (rowIsSelected)
    {
        g.fillAll (juce::Colours::darkgrey);
    }

    // Draw drop indicator line
    if (isDragging && rowNumber == dragInsertIndex)
    {
        g.setColour (juce::Colours::skyblue);
        g.fillRect (0, 0, width, 3);
    }

    g.setColour (juce::Colours::white);
    juce::String text = type.toUpperCase() + "  #" + juce::String (id);
    g.drawText (text, 8, 0, width - 16, height, juce::Justification::centredLeft);
}

void ControllerContent::selectedRowsChanged (int lastRowSelected)
{
    auto panels = settings.getPanelsRoot();
    if (! panels.isValid() || lastRowSelected < 0 || lastRowSelected >= panels.getNumChildren())
    {
        selectedPanelId = -1;
        refreshRightEditorFromSelection();
        resized();
        repaint();
        return;
    }

    auto p = panels.getChild (lastRowSelected);
    selectedPanelId = (int) p.getProperty (AlterState::kId);

    refreshRightEditorFromSelection();
    resized();
    repaint();
}

juce::Component* ControllerContent::refreshComponentForRow (int row, bool isSelected, juce::Component* existing)
{
    auto* item = dynamic_cast<DraggableListItem*> (existing);
    if (item == nullptr)
        item = new DraggableListItem (*this);

    item->setRow (row);
    return item;
}

void ControllerContent::finalizeDrag()
{
    if (! isDragging || draggedRow < 0) return;

    auto panels = settings.getPanelsRoot();
    if (! panels.isValid()) return;

    const int numRows = getNumRows();
    int targetIndex = juce::jlimit (0, numRows - 1, dragInsertIndex);

    // Adjust target if dragging down
    if (targetIndex > draggedRow)
        targetIndex--;

    if (targetIndex != draggedRow && draggedRow < numRows)
    {
        // Move child in ValueTree
        panels.moveChild (draggedRow, targetIndex, nullptr);
        panelList.selectRow (targetIndex);
    }

    isDragging = false;
    draggedRow = -1;
    dragInsertIndex = -1;
    panelList.repaint();
}

// ===== helpers =====

void ControllerContent::setEditorVisible (bool rms, bool spec, bool osc, bool syn)
{
    const bool anySelected = rms || spec || osc || syn;

    // RMS
    lblRmsMode.setVisible   (rms);
    cbRmsPeakMode.setVisible(rms);
    lblRmsSmooth.setVisible (rms);
    sRmsSmooth.setVisible   (rms);
    lblColorMode.setVisible (rms);
    cbColorMode.setVisible  (rms);

    // Color picker visibility depends on custom color mode (handled in resized())

    // Spectrum
    specAWeight.setVisible  (spec);
    lblSpecSmooth.setVisible(spec);
    sSpecSmooth.setVisible  (spec);
    lblBins.setVisible      (spec);
    cbBins.setVisible       (spec);

    // Oscillator
    lblOscSmooth.setVisible (osc);
    sOscSmooth.setVisible   (osc);
    tbFill.setVisible       (osc);
    lblDisplayMode.setVisible(osc);
    cbDisplayMode.setVisible (osc);
    lblOscZoom.setVisible   (osc);
    sOscZoom.setVisible     (osc);

    // Synesthesia
    lblSynSmooth.setVisible (syn);
    sSynSmooth.setVisible   (syn);
    lblBPM.setVisible       (syn);
    tbSyncBPM.setVisible    (syn);
    lblZoom.setVisible      (syn);
    sZoom.setVisible        (syn);
    lblRotation.setVisible  (syn);
    sRotation.setVisible    (syn);
    lblSymmetry.setVisible  (syn);
    sSymmetry.setVisible    (syn);
    lblSaturation.setVisible(syn);
    sSaturation.setVisible  (syn);
    lblBloom.setVisible     (syn);
    sBloom.setVisible       (syn);

    // Color (visible for RMS, Spectrum, and Oscillator only - NOT synesthesia)
    const bool showColor = rms || spec || osc;
    lblColor.setVisible        (showColor);
    lblColorPreview.setVisible (showColor);
    btnColor.setVisible        (showColor);

    // Rotation (visible for all module types)
    lblModuleRotation.setVisible (anySelected);
    btnRotate.setVisible         (anySelected);
}

void ControllerContent::refreshRightEditorFromSelection()
{
    if (selectedPanelId < 0)
    {
        setEditorVisible (false, false, false, false);
        // no selection -> default color preview
        lblColorPreview.setColour (juce::Label::backgroundColourId, juce::Colours::red);
        return;
    }

    auto panel = settings.getPanelById (selectedPanelId);
    if (! panel.isValid())
    {
        setEditorVisible (false, false, false, false);
        lblColorPreview.setColour (juce::Label::backgroundColourId, juce::Colours::red);
        return;
    }

    const auto type = panel.getProperty (AlterState::kType).toString();

    if (type == "rms" || type == "audiometer")
    {
        const int meterMode = (int) panel.getProperty (AlterState::kMeterMode, 0);
        cbRmsPeakMode.setSelectedId (meterMode + 1, juce::dontSendNotification);

        const float smooth = (float) panel.getProperty (AlterState::kSmooth, 0.5f);
        sRmsSmooth.setValue (smooth, juce::dontSendNotification);

        const int colorMode = (int) panel.getProperty (AlterState::kColorMode, 0);  // 0=Standard, 1=Gradient, 2=Spectrum
        cbColorMode.setSelectedId (colorMode + 1, juce::dontSendNotification);
    }
    else if (type == "spectrum")
    {
        const bool aW = (bool) panel.getProperty (AlterState::kAWeight, false);
        const float smooth = (float) panel.getProperty (AlterState::kSmooth, 0.5f);
        const int bins = (int) panel.getProperty (AlterState::kBins, 2048);
        const int rpoints = (int) panel.getProperty (AlterState::kRenderPoints, 1024);

        specAWeight.setToggleState (aW, juce::dontSendNotification);
        sSpecSmooth.setValue (smooth, juce::dontSendNotification);
        cbBins.setSelectedId (bins, juce::dontSendNotification);
        sRenderPoints.setValue (rpoints, juce::dontSendNotification);
    }
    else if (type == "oscillator" || type == "oscilator")
    {
    const float smooth = (float) panel.getProperty (AlterState::kSmooth, 0.5f);
        const bool neon = (bool) panel.getProperty (AlterState::kNeon, false);
        sOscSmooth.setValue (smooth, juce::dontSendNotification);
        tbFill.setToggleState (neon, juce::dontSendNotification);
        const int dm = (int) panel.getProperty (AlterState::kDisplayMode, 0);
        cbDisplayMode.setSelectedId (juce::jlimit (1, 3, dm + 1), juce::dontSendNotification);
    const float oz = (float) panel.getProperty (AlterState::kZoom, 0.485f);
    sOscZoom.setValue (oz, juce::dontSendNotification);
    }
    else if (type == "synesthesia")
    {
        const float smooth = (float) panel.getProperty (AlterState::kSmooth, 0.15f);
        sSynSmooth.setValue (smooth, juce::dontSendNotification);

        const bool syncBPM = (bool) panel.getProperty (AlterState::kSyncBPM, true);
        tbSyncBPM.setToggleState (syncBPM, juce::dontSendNotification);

        const float zoom = (float) panel.getProperty (AlterState::kZoom, 1.0f);
        sZoom.setValue (zoom, juce::dontSendNotification);

        const float rotation = (float) panel.getProperty (AlterState::kRotation, 0.0f);
        sRotation.setValue (rotation, juce::dontSendNotification);

        const int symmetry = (int) panel.getProperty (AlterState::kSymmetry, 1);
        sSymmetry.setValue (symmetry, juce::dontSendNotification);

        const float saturation = (float) panel.getProperty (AlterState::kSaturation, 1.0f);
        sSaturation.setValue (saturation, juce::dontSendNotification);

        const float bloom = (float) panel.getProperty (AlterState::kBloom, 0.0f);
        sBloom.setValue (bloom, juce::dontSendNotification);
    }

    // update color preview from panel property if present
    if (panel.hasProperty (AlterState::kColor))
    {
        const int argb = (int) panel.getProperty (AlterState::kColor);
        lblColorPreview.setColour (juce::Label::backgroundColourId, juce::Colour ((uint32_t) argb));
    }

    // Update rotation button text
    int rotationState = (int) panel.getProperty (AlterState::kRotationAngle, 0);
    const char* rotationText[] = { "0 deg", "90 deg", "180 deg", "270 deg" };
    btnRotate.setButtonText (rotationText[juce::jlimit (0, 3, rotationState)]);
}
