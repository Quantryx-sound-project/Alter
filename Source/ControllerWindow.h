#pragma once
#include <JuceHeader.h>
#include "AlterState.h"
#include "InfoWindowAudioMeters.h"

// ========================================
// Custom LookAndFeel: VST-style rotary knobs
// ========================================
class AlterKnobLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override
    {
        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        const float trackW = juce::jmax (2.0f, radius * 0.14f);

        // Background arc (full range track)
        {
            juce::Path arc;
            arc.addCentredArc (cx, cy, radius - trackW * 0.5f, radius - trackW * 0.5f,
                               0.0f, rotaryStartAngle, rotaryEndAngle, true);
            g.setColour (juce::Colour (0xFF3A3A3A));
            g.strokePath (arc, juce::PathStrokeType (trackW, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        }

        // Value arc (filled portion)
        if (sliderPos > 0.001f)
        {
            juce::Path arc;
            arc.addCentredArc (cx, cy, radius - trackW * 0.5f, radius - trackW * 0.5f,
                               0.0f, rotaryStartAngle, angle, true);
            g.setColour (juce::Colour (0xFF00BFFF));
            g.strokePath (arc, juce::PathStrokeType (trackW, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        }

        // Knob body (gradient circle)
        const float knobR = (radius - trackW) * 0.75f;
        g.setGradientFill (juce::ColourGradient (
            juce::Colour (0xFF555555), cx, cy - knobR,
            juce::Colour (0xFF2A2A2A), cx, cy + knobR, false));
        g.fillEllipse (cx - knobR, cy - knobR, knobR * 2.0f, knobR * 2.0f);

        // Subtle ring
        g.setColour (juce::Colour (0xFF666666));
        g.drawEllipse (cx - knobR, cy - knobR, knobR * 2.0f, knobR * 2.0f, 1.0f);

        // Pointer
        const float pLen = knobR * 0.8f;
        const float pW = juce::jmax (1.5f, radius * 0.1f);
        juce::Path pointer;
        pointer.addRoundedRectangle (-pW * 0.5f, -pLen, pW, pLen, pW * 0.5f);
        pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (cx, cy));
        g.setColour (juce::Colours::white);
        g.fillPath (pointer);
    }
};

// Controller v2:
// - left: Add... + list panel instances (RMS #id, SPECTRUM #id)
// - right: editor vybraného panelu + Delete
// - footer: Always on top + Audio Mode + Quit
// - drag & drop support for reordering panels

class ControllerContent : public juce::Component,
						  private juce::ListBoxModel,
						  private juce::Timer
{
public:
	explicit ControllerContent (AlterState& s);
	~ControllerContent() override;

	std::function<void (bool)> onAlwaysOnTopChanged;
	std::function<void (int)> onAudioModeChangedInternal;
	std::function<void()> onCloseRequested;

	void timerCallback() override;
	void resized() override;
	void setInfoWindowAlwaysOnTop (bool on);

private:
	// ===== ListBoxModel =====
	int getNumRows() override;
	juce::var getDragSourceDescription (const juce::SparseSet<int>& selectedRows) override;
	void paintListBoxItem (int rowNumber, juce::Graphics& g,
						   int width, int height, bool rowIsSelected) override;
	void selectedRowsChanged (int lastRowSelected) override;
	juce::Component* refreshComponentForRow (int row, bool isSelected, juce::Component* existing) override;

	// ===== Drag & Drop Helper Component =====
	class DraggableListItem : public juce::Component
	{
	public:
		DraggableListItem (ControllerContent& o) : owner (o) 
		{
			setMouseCursor (juce::MouseCursor::PointingHandCursor);
		}

		void setRow (int r) { row = r; }

		void mouseDown (const juce::MouseEvent& e) override
		{
			owner.panelList.selectRow (row);
			dragging = false;
			dragStartPos = e.getPosition();
		}

		void mouseDrag (const juce::MouseEvent& e) override
		{
			if (! dragging && e.getDistanceFromDragStart() > 5)
			{
				dragging = true;
				owner.isDragging = true;
				owner.draggedRow = row;
				setMouseCursor (juce::MouseCursor::DraggingHandCursor);
			}

			if (dragging)
			{
				const auto pos = e.getEventRelativeTo (&owner.panelList).position.toInt();
				const int numRows = owner.getNumRows();
				const int rowHeight = owner.panelList.getRowHeight();
				int newIndex = juce::jlimit (0, numRows, (pos.y + rowHeight / 2) / rowHeight);

				if (newIndex != owner.dragInsertIndex)
				{
					owner.dragInsertIndex = newIndex;
					owner.panelList.repaint();
				}
			}
		}

		void mouseUp (const juce::MouseEvent&) override
		{
			if (dragging)
			{
				dragging = false;
				setMouseCursor (juce::MouseCursor::PointingHandCursor);
				owner.finalizeDrag();
			}
		}

		void mouseExit (const juce::MouseEvent&) override
		{
			if (! dragging)
				setMouseCursor (juce::MouseCursor::PointingHandCursor);
		}

	private:
		ControllerContent& owner;
		int row = 0;
		bool dragging = false;
		juce::Point<int> dragStartPos;
	};

	// ===== helpers =====
	void finalizeDrag();
	void setEditorVisible (bool rms, bool spec, bool osc, bool syn);
	void refreshRightEditorFromSelection();

	AlterState& settings;

	// left
	juce::TextButton btnAdd { "Create" };
	juce::ListBox    panelList;
	int selectedPanelId = -1;

	// drag & drop state
	bool isDragging = false;
	int draggedRow = -1;
	int dragInsertIndex = -1;
	juce::Component dragOverlay;
	juce::Component::SafePointer<ModuleInfoWindow> currentInfoWindow;

	// right common
	juce::TextButton btnDelete { "Destroy" };
	juce::TextButton btnGetInfo{ "Explore" };
	juce::Label lblRenderPoints { {}, "Render points" };
	juce::Slider sRenderPoints;

	// RMS editor
	juce::Label  lblRmsMode;
	juce::ComboBox cbRmsPeakMode;
	juce::Label  lblRmsSmooth;
	juce::Slider sRmsSmooth;

	// Spectrum editor
	juce::ToggleButton specAWeight;
	juce::Label        lblSpecSmooth, lblBins;
	juce::Slider       sSpecSmooth;
	juce::ComboBox     cbBins;

	// Oscillator editor
	juce::Label  lblOscSmooth;
	juce::Slider sOscSmooth;
	juce::ToggleButton tbFill;
	juce::Label  lblDisplayMode;
	juce::ComboBox cbDisplayMode;
	juce::Label  lblOscZoom;
	juce::Slider sOscZoom;

	// Synesthesia editor
	juce::Label  lblSynSmooth;
	juce::Slider sSynSmooth;
	juce::Label  lblBPM;
	juce::ToggleButton tbSyncBPM;
	juce::Label  lblZoom;
	juce::Slider sZoom;
	juce::Label  lblRotation;
	juce::Slider sRotation;
	juce::Label  lblSymmetry;
	juce::Slider sSymmetry;
	juce::Label  lblSaturation;
	juce::Slider sSaturation;
	juce::Label  lblBloom;
	juce::Slider sBloom;

	// footer
	juce::ToggleButton alwaysOnTop;
	juce::Label        lblAudioMode;
	juce::ComboBox     cbAudioMode;
	juce::TextButton   btnQuit { "Quit" };
	juce::TextButton   btnClose{ "Close" };

	// color UI (used in module settings)
	juce::Label        lblColor;
	juce::Label        lblColorPreview;
	juce::TextButton   btnColor { "Color..." };
	juce::Label        lblColorMode;
	juce::ComboBox     cbColorMode;

	// Module rotation (common for all modules)
	juce::Label        lblModuleRotation { {}, "Rotate" };
	juce::TextButton   btnRotate { "0 deg" };

	// Scrollable editor viewport
	juce::Viewport     editorViewport;
	juce::Component    editorPanel;

	// System Audio gain
	juce::Label        lblGain;
	juce::Slider       sGain;

	// Custom rotary knob style
	AlterKnobLookAndFeel knobLAF;
};

// Window wrapper
class ControllerWindow : public juce::DocumentWindow
{
public:
    explicit ControllerWindow (AlterState& s, juce::DocumentWindow* hudWindow = nullptr)
        : juce::DocumentWindow ("ALTER Controller",
                                juce::Colours::black,
                                juce::DocumentWindow::allButtons),
          content (s)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, true);
        setContentOwned (&content, false);
        setSize (540, 380);

        // Notify both the controller window and optionally the HUD window
        content.onAlwaysOnTopChanged = [this, hudWindow](bool on)
        {
            setAlwaysOnTop (on);
            if (hudWindow) hudWindow->setAlwaysOnTop (on);
            content.setInfoWindowAlwaysOnTop (on);
            if (onAlwaysOnTopChanged) onAlwaysOnTopChanged (on);
        };

        content.onCloseRequested = [this]() { closeButtonPressed(); };

        // Forward audio mode changes
        content.onAudioModeChangedInternal = [this](int mode)
        {
            if (onAudioModeChanged)
                onAudioModeChanged (mode);
        };

        setVisible (false);
    }

    void closeButtonPressed() override { setVisible (false); }

    // external hooks
    std::function<void(bool)> onAlwaysOnTopChanged;
    std::function<void(int)> onAudioModeChanged;

private:
    ControllerContent content;
};
