// MainComponent.h
#pragma once

#include <JuceHeader.h>
#include <unordered_map>
#include <memory>
#include <vector>

#include "AudioSourceInterface.h"
#include "UdpReceiver.h"
#include "SystemAudioCapture.h"
#include "AudioMeter.h"
#include "Spectrum.h"
#include "Oscilator.h"
#include "Synesthesia.h"
#include "ChladniPaterns.h"
#include "AlterState.h"

class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::ValueTree::Listener
{
public:
    explicit MainComponent (AlterState& s);
    ~MainComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    void setShowControllerCallback (std::function<void()> cb) { showController = std::move (cb); }

    // Audio source switching
    void setAudioSourceMode (int mode)
    {
        DBG ("=== SWITCHING AUDIO SOURCE ===");
        DBG ("Mode: " + juce::String (mode) + " (" + (mode == 1 ? "UDP" : "System Audio") + ")");

        if (mode == 1)
        {
            activeAudioSource = &udpSource;
            DBG ("Active source: UdpReceiver");
        }
        else if (mode == 2)
        {
            // Lazy initialization - create only when first needed
            if (!systemAudioSource)
            {
                DBG ("Creating SystemAudioCapture for the first time...");
                try
                {
                    systemAudioSource = std::make_unique<SystemAudioCapture>();
                    DBG ("SystemAudioCapture created successfully!");
                }
                catch (const std::exception& e)
                {
                    DBG ("ERROR: Failed to create SystemAudioCapture: " + juce::String (e.what()));
                    return;
                }
            }

            activeAudioSource = systemAudioSource.get();
            DBG ("Active source: SystemAudioCapture");

            // Test if system audio is receiving data
            float rms = systemAudioSource->getLastRms();
            DBG ("SystemAudioCapture RMS: " + juce::String (rms, 3));
        }

        DBG ("Rebuilding visual modules...");

        // Force complete rebuild when source changes (happens in rebuildSlots)
        forceRebuildAllModules();

        // Force repaint all visuals
        repaint();

        DBG ("=== SWITCH COMPLETE ===");
    }

    // selection helpers exposed publicly so external controllers can query/drive them
    void selectPanelById (int id);
    void setAlwaysOnTopForSelectedPanels (bool on);
    void setAlwaysOnTopForAllDetachedPanels (bool on);
    // HUD drag preview state
    void beginHudDrag (int panelId, juce::Point<int> screenPos);
    void updateHudDrag (int panelId, juce::Point<int> screenPos);
    void endHudDrag (int panelId, juce::Point<int> screenPos);

    // live drag state
    int hudDragPanelId { -1 };
    int hudDragInsertIndex { -1 };
    std::unique_ptr<juce::ImageComponent> hudDragGhost;
    juce::Point<int> hudDragOffset { 0, 0 };

    // Visual placeholder for drag & drop (mobile-style gap)
    class DragPlaceholder : public juce::Component,
                            private juce::Timer
    {
    public:
        DragPlaceholder()
        {
            setInterceptsMouseClicks (false, false);
            startTimerHz (30); // smooth pulsing animation
        }

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced (2.0f);

            // Animated pulsing effect
            const float pulse = 0.3f + 0.15f * std::sin (pulsePhase);

            // Dashed outline with glow
            g.setColour (juce::Colours::skyblue.withAlpha (pulse));
            const float dash[] = { 8.0f, 4.0f };
            g.drawDashedLine (juce::Line<float> (b.getX(), b.getY(), b.getRight(), b.getY()), dash, 2, 2.5f);
            g.drawDashedLine (juce::Line<float> (b.getRight(), b.getY(), b.getRight(), b.getBottom()), dash, 2, 2.5f);
            g.drawDashedLine (juce::Line<float> (b.getRight(), b.getBottom(), b.getX(), b.getBottom()), dash, 2, 2.5f);
            g.drawDashedLine (juce::Line<float> (b.getX(), b.getBottom(), b.getX(), b.getY()), dash, 2, 2.5f);

            // Subtle fill with pulse
            g.setColour (juce::Colours::skyblue.withAlpha (0.05f + 0.03f * std::sin (pulsePhase)));
            g.fillRect (b);

            // Center indicator
            const float centerSize = 16.0f;
            const auto center = b.getCentre();
            const juce::Rectangle<float> centerRect (center.x - centerSize/2, center.y - centerSize/2, centerSize, centerSize);
            g.setColour (juce::Colours::skyblue.withAlpha (pulse * 0.8f));
            g.fillEllipse (centerRect);
        }

    private:
        void timerCallback() override
        {
            pulsePhase += 0.15f;
            if (pulsePhase > juce::MathConstants<float>::twoPi)
                pulsePhase -= juce::MathConstants<float>::twoPi;
            repaint();
        }

        float pulsePhase = 0.0f;
    };

    std::unique_ptr<DragPlaceholder> dragPlaceholder;

private:
    AlterState& settings;

    // Audio sources (lazy initialization to prevent crashes)
    UdpReceiver udpSource { 7000 };
    std::unique_ptr<SystemAudioCapture> systemAudioSource;  // Created on demand
    IAudioSource* activeAudioSource { &udpSource }; // Default: UDP

    // =======================================================
    // Resize Divider between modules
    // =======================================================
    class ResizeDivider : public juce::Component
    {
    public:
        ResizeDivider (MainComponent& owner, int leftPanelId)
            : mainComp (owner), leftId (leftPanelId)
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        }

        void paint (juce::Graphics& g) override
        {
            // Invisible by default - only visible on hover
            if (isMouseOver())
            {
                // Show grey line on hover for visual feedback
                g.fillAll (juce::Colours::grey.withAlpha (0.5f));
            }
        }

        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override { repaint(); }

        void mouseDown (const juce::MouseEvent& e) override
        {
            dragStartX = e.getScreenX();

            // Get current widths of LEFT and RIGHT modules
            auto* leftHost = mainComp.findHostById (leftId);
            if (!leftHost) return;

            dragStartLeftWidth = leftHost->getWidth();

            // Find right module
            auto hudHosts = mainComp.getHudHosts();
            for (size_t i = 0; i < hudHosts.size() - 1; ++i)
            {
                if (hudHosts[i]->getId() == leftId)
                {
                    rightId = hudHosts[i + 1]->getId();
                    auto* rightHost = hudHosts[i + 1];
                    dragStartRightWidth = rightHost->getWidth();
                    break;
                }
            }
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            int dx = e.getScreenX() - dragStartX;

            // Calculate new widths: left grows/shrinks, right shrinks/grows
            int combinedWidth = dragStartLeftWidth + dragStartRightWidth;

            // FIX: Ensure combinedWidth is large enough to avoid invalid jlimit bounds
            if (combinedWidth < 100)
                return; // Modules too narrow to resize safely

            int newLeftWidth = juce::jlimit (50, combinedWidth - 50, dragStartLeftWidth + dx);
            int newRightWidth = combinedWidth - newLeftWidth;

            // Update ratios for both modules
            mainComp.setModuleWidths (leftId, newLeftWidth, rightId, newRightWidth);
        }

    private:
        MainComponent& mainComp;
        int leftId;
        int rightId { -1 };
        int dragStartX { 0 };
        int dragStartLeftWidth { 0 };
        int dragStartRightWidth { 0 };
    };

    // =======================================================
    // PanelHost wrapper
    // =======================================================
    class PanelHost : public juce::Component
    {
    public:
        PanelHost (MainComponent& owner,
                   juce::ValueTree panelNode,
                   int panelId,
                   juce::String panelType,
                   std::unique_ptr<juce::Component> inner);

        ~PanelHost() override = default;

        int getId() const noexcept { return id; }
        juce::Component* getInner() const noexcept { return view.get(); }
        juce::ValueTree getNode() const { return panel; }

        void resized() override;
        void paintOverChildren (juce::Graphics& g) override;

        // debug/UX: double click toggle (necháš si, kým ladíš drag)
        void mouseDoubleClick (const juce::MouseEvent&) override;

        // drag detach (touchpad OK)
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp   (const juce::MouseEvent& e) override;

        // Enable/disable transparent drag overlay (for detached windows)
        void setDragOverlayEnabled (bool enabled);

    private:
        // Transparent overlay that intercepts all mouse events for dragging
        class DragOverlay : public juce::Component
        {
        public:
            DragOverlay (PanelHost& h) : host (h)
            {
                setInterceptsMouseClicks (true, true);
                setMouseCursor (juce::MouseCursor::DraggingHandCursor);
                // Remove always on top - causes performance issues
                // setAlwaysOnTop (true);
            }

            void paint (juce::Graphics&) override
            {
                // Completely transparent - no rendering at all for best performance
            }

            void mouseDown (const juce::MouseEvent& e) override { host.mouseDown (e); }
            void mouseDrag (const juce::MouseEvent& e) override { host.mouseDrag (e); }
            void mouseUp (const juce::MouseEvent& e) override { host.mouseUp (e); }
            void mouseDoubleClick (const juce::MouseEvent& e) override { host.mouseDoubleClick (e); }

        private:
            PanelHost& host;
        };

        MainComponent& owner;
        juce::ValueTree panel;
        int id = -1;
        juce::String type;
        std::unique_ptr<juce::Component> view;
        std::unique_ptr<DragOverlay> dragOverlay;

        bool dragging = false;
        bool startedDetached = false;
        juce::Point<int> lastScreenPos;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PanelHost)
    };

    // =======================================================
    // Slot: 1 panel = 1 host (stála inštancia)
    // =======================================================
    struct PanelSlot
    {
        int id = -1;
        juce::String type;
        juce::ValueTree node;
        std::unique_ptr<PanelHost> host;
    };

    std::vector<PanelSlot> slots;

    // Resize dividers between modules
    std::vector<std::unique_ptr<ResizeDivider>> resizeDividers;

    // =======================================================
    // Detached windows
    // =======================================================
    class PanelWindow;
    std::unordered_map<int, std::unique_ptr<PanelWindow>> windows;

    // HUD build/sync
    void rebuildSlotsFromState();
    void forceRebuildAllModules();  // Clear all slots/windows and rebuild
    void applyPanelPropsToView (const juce::ValueTree& panel, juce::Component& view);

    // Resize helpers
    PanelHost* findHostById (int panelId);
    std::vector<PanelHost*> getHudHosts();
    void setModuleWidths (int leftId, int leftWidth, int rightId, int rightWidth);
    void rebuildResizeDividers();

    

    // detach/attach helpers (volané async)
    void detachPanelToWindow (int id);
    void attachPanelBackToHud (int id);

    // reorder panel based on a drop location (screen coordinates)
    void reorderPanelByScreenPosition (int panelId, juce::Point<int> screenPos);

    // helpers
    juce::Rectangle<int> getHudScreenBounds() const;
    PanelSlot* findSlotById (int id);

    // ====== controller tlačidlá (po bokoch HUD) ======
    class VerticalControllerButton : public juce::Button
    {
    public:
        VerticalControllerButton() : juce::Button ("Controller") {}
        int idealWidth() const noexcept { return 28; }
        void paintButton (juce::Graphics& g, bool over, bool down) override;
    };

    std::unique_ptr<VerticalControllerButton> leftCtrl, rightCtrl;
    std::function<void()> showController;

    // ====== layout + repaint ======
    void timerCallback() override;
    void layoutActiveViews (juce::Rectangle<int> area);

    // ====== ValueTree listener ======
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override;
    void valueTreeChildAdded      (juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved    (juce::ValueTree& parent, juce::ValueTree& child, int index) override;
    void valueTreeChildOrderChanged(juce::ValueTree& parent, int oldIndex, int newIndex) override;
    void valueTreeParentChanged   (juce::ValueTree& tree) override;
    void valueTreeRedirected      (juce::ValueTree& tree) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
