// MainComponent.h
#pragma once

#include <JuceHeader.h>
#include <unordered_map>
#include <map>
#include <memory>
#include <vector>
#include <functional>

#include "AudioSourceInterface.h"
#include "UdpReceiver.h"
#include "ModuleAudioSource.h"
#include "SystemAudioCapture.h"
#include "AudioMeter.h"
#include "Spectrum.h"
#include "Oscilator.h"
#include "Synesthesia.h"
#include "ChladniPaterns.h"
#include "ToneAnalyzer.h"
#include "Spectrogram.h"
#include "Stereoscope.h"
#include "GeometryVisual.h"
#include "FusionVisual.h"
#include "AlterState.h"
#include "AlterTheme.h"

// JUCE 8's Direct2D window renderer conflicts with attached OpenGLContexts
// (Synesthesia) and with images that read pixels back, causing crashes in the
// GL driver / Direct2D. Force the software (GDI) renderer on any window that may
// host an OpenGL module.
namespace AlterRender
{
    inline void useSoftwareRenderer (juce::Component& topLevel)
    {
       #if JUCE_WINDOWS
        if (auto* p = topLevel.getPeer())
        {
            const auto engines = p->getAvailableRenderingEngines();
            for (int i = 0; i < engines.size(); ++i)
                if (! engines[i].containsIgnoreCase ("direct2d"))
                {
                    if (p->getCurrentRenderingEngine() != i)
                        p->setCurrentRenderingEngine (i);
                    break;
                }
        }
       #else
        juce::ignoreUnused (topLevel);
       #endif
    }
}

class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::ValueTree::Listener
{
public:
    explicit MainComponent (AlterState& s);
    ~MainComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    /** Ctrl+Z / Ctrl+Shift+Z: state-wide undo / redo. */
    bool keyPressed (const juce::KeyPress& key) override
    {
        return settings.handleUndoRedoKey (key);
    }

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
                    systemAudioSource->setFftOrder (AlterState::fftOrderForBins (settings.maxFftBins()));
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

        // point every module's per-module source at the new global (auto modules)
        for (auto& s : slots)
            if (s.host && s.host->getAudioSource())
                s.host->getAudioSource()->setGlobalSource (activeAudioSource);

        DBG ("Rebuilding visual modules...");

        // Force complete rebuild when source changes (happens in rebuildSlots)
        forceRebuildAllModules();

        // Force repaint all visuals
        repaint();

        DBG ("=== SWITCH COMPLETE ===");
    }

    // Access to the UDP source (plugin instance picker in Controller)
    UdpReceiver& getUdpSource() noexcept { return udpSource; }

    // ── transparent (alpha) capture ──────────────────────────────────────────
    /** Put every module into the same state a Fusion layer is in: no theme
        background, and the GL modules routed through their offscreen FBO so the
        `uNoBg` branch of their shaders actually runs. That is the ONLY state in
        which a module hands out pixels whose alpha means anything — which is why
        alpha export reuses it wholesale instead of adding a second render path.

        MESSAGE THREAD. Turn it off again when the recording ends: while it is on
        the HUD is drawing itself on transparency, which is not what it should look
        like in normal use. */
    void setAlphaCaptureMode (bool on);

    /** MESSAGE THREAD. One frame of the modules on a fully transparent canvas —
        module pixels only, no window chrome, no panel frames, no background.
        Pass a module to get JUST that one. `outW`/`outH` are the size the caller
        wants; the paint is SCALED into it rather than done full size and resampled
        afterwards, because this runs on the message thread 30 times a second and
        every wasted pixel there is felt as a stuck UI. Pass 0 for native. */
    juce::Image grabAlphaFrame (juce::Component* onlyThis = nullptr,
                                int outW = 0, int outH = 0);

    /** One HUD module, as something that can be exported on its own. */
    struct ExportableModule
    {
        juce::Component*     view = nullptr;
        juce::String         name;              // for the file name
        juce::uint32         audioInstance = 0; // 0 = auto (the effective instance)
        juce::Rectangle<int> screenArea;        // physical px, for the screen grab
    };

    /** MESSAGE THREAD. The modules that "export each separately" should produce a
        file for: everything laid out in the HUD, with a Fusion counting as ONE
        module — its layers are its content, not separate videos. */
    std::vector<ExportableModule> getExportableModules() const;

private:
    /** The modules THIS recording put into layer mode, so that leaving alpha mode
        restores exactly those and never a module a Fusion still owns. */
    std::vector<juce::Component::SafePointer<juce::Component>> alphaTouched;

    // Grab cost, reported once a second while a transparent recording runs.
    double alphaGrabMs = 0.0, alphaGrabLogSec = 0.0;
    int    alphaGrabCount = 0;

public:

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

    // One shared GPU context for the whole HUD window: GPU-accelerates all 2D
    // module painting and renders any Synesthesia fractals. Replaces the slow
    // GDI software renderer.
    AlterGLHost glHost;

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
        ResizeDivider (MainComponent& owner, int leftPanelId, int rightPanelId)
            : mainComp (owner), leftId (leftPanelId), rightId (rightPanelId)
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        }

        void paint (juce::Graphics& g) override
        {
            if (isMouseOver())
                g.fillAll (juce::Colours::grey.withAlpha (0.5f));
        }

        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override { repaint(); }

        void mouseDown (const juce::MouseEvent& e) override
        {
            dragStartX = e.getScreenX();

            auto* leftHost  = mainComp.findHostById (leftId);
            auto* rightHost = mainComp.findHostById (rightId);
            dragStartLeftWidth  = leftHost  ? leftHost->getWidth()  : 0;
            dragStartRightWidth = rightHost ? rightHost->getWidth() : 0;
            dragStartFree       = rowFreeSpace;
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            const int dx = e.getScreenX() - dragStartX;

            // A row of nothing but fixed-width panels is CENTRED, so growing it by
            // one pixel moves each edge by half of one — the grabbed edge travelled
            // at half the mouse's speed and slid out from under the cursor, which is
            // what made a meter feel like it refused to be widened. Doubling the
            // delta there puts the edge back under the pointer; in a row with a
            // flexible panel the group is left-anchored and 1:1 is already right.
            const int scaled = centredRow ? dx * 2 : dx;

            // rightId < 0 = TRAILING-EDGE grip: there is no neighbour to trade
            // pixels with, the panel just grows into the free space of the row.
            if (rightId < 0)
            {
                mainComp.setModuleWidth (leftId, juce::jmax (40, dragStartLeftWidth + scaled));
                return;
            }

            // leftId < 0 = LEADING-EDGE grip, the same thing mirrored: the panel is
            // the one on the RIGHT of this handle, and it grows as the edge is pulled
            // LEFT — so the delta is subtracted rather than added. Nothing else
            // differs, including the fact that the row re-centres around the new
            // width, which is why the doubling above applies here too.
            if (leftId < 0)
            {
                mainComp.setModuleWidth (rightId, juce::jmax (40, dragStartRightWidth - scaled));
                return;
            }

            const int combinedWidth = dragStartLeftWidth + dragStartRightWidth;
            if (combinedWidth < 100)
                return; // modules too narrow to resize safely

            // GROW INTO THE ROW'S FREE SPACE FIRST, then into the neighbour.
            //
            // This used to redistribute `combinedWidth` and nothing else, which is
            // right for a full row and useless for a row that is mostly empty: two
            // meters are 96 px each, so the divider between them could only ever
            // shuffle 192 px back and forth while the other 1000 px of the block sat
            // there untouched. Widening one meter to the width of the HUD was not
            // hard to discover, it was impossible.
            //
            // A row only HAS free space when every column in it is fixed-width (a
            // flexible module soaks the remainder up), which is exactly the meter
            // case; in any other row dragStartFree is 0 and this reduces to the
            // 1:1 trade it always was.
            const int growth   = scaled;                       // + = the edge moved right
            const int fromFree = juce::jlimit (0, juce::jmax (0, growth), dragStartFree);
            const int fromRight = juce::jmax (0, growth - fromFree);

            const int newLeftWidth  = juce::jmax (50, dragStartLeftWidth + growth);
            // Shrinking the left one hands its pixels back to the row, not to the
            // neighbour — the neighbour never asked for them and moving it sideways
            // under a drag it is not part of reads as a glitch.
            const int newRightWidth = juce::jmax (50, dragStartRightWidth - fromRight);

            mainComp.setModuleWidths (leftId, newLeftWidth, rightId, newRightWidth);
        }

        /** Told by layoutRow: is this grip's row centred (i.e. every panel in it
            is fixed-width, so it has no flexible member to soak up the remainder)? */
        void setCentredRow (bool b) noexcept { centredRow = b; }

        /** Told by layoutRow: unused pixels in this divider's row. Non-zero only
            when every column in it is fixed-width. Sampled at mouseDown, because it
            shrinks as the drag consumes it. */
        void setRowFreeSpace (int px) noexcept { rowFreeSpace = px; }

    private:
        MainComponent& mainComp;
        const int leftId, rightId;
        int dragStartX { 0 };
        int dragStartLeftWidth { 0 };
        int dragStartRightWidth { 0 };
        int rowFreeSpace  { 0 };
        int dragStartFree { 0 };
        bool centredRow { false };
    };

    // =======================================================
    // Divider between two modules STACKED inside one column of a block.
    // Drag it to change how the column's height is split between them.
    // =======================================================
    class StackDivider : public juce::Component
    {
    public:
        StackDivider (MainComponent& owner, int upperPanelId, int lowerPanelId)
            : mainComp (owner), upperId (upperPanelId), lowerId (lowerPanelId)
        {
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        }

        void setPanels (int upperPanelId, int lowerPanelId) noexcept
        {
            upperId = upperPanelId;
            lowerId = lowerPanelId;
        }

        void paint (juce::Graphics& g) override
        {
            if (isMouseOver() || isMouseButtonDown())
                g.fillAll (juce::Colours::grey.withAlpha (0.5f));
        }

        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit  (const juce::MouseEvent&) override { repaint(); }

        void mouseDown (const juce::MouseEvent& e) override
        {
            dragStartY = e.getScreenY();
            auto* up = mainComp.findHostById (upperId);
            auto* lo = mainComp.findHostById (lowerId);
            startUpperH = up ? up->getHeight() : 0;
            startLowerH = lo ? lo->getHeight() : 0;
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            const int combined = startUpperH + startLowerH;
            if (combined < 60) return;

            const int dy = e.getScreenY() - dragStartY;
            const int newUpper = juce::jlimit (30, combined - 30, startUpperH + dy);
            mainComp.setStackHeights (upperId, newUpper, lowerId, combined - newUpper);
        }

    private:
        MainComponent& mainComp;
        int upperId, lowerId;
        int dragStartY { 0 };
        int startUpperH { 0 };
        int startLowerH { 0 };
    };

    // =======================================================
    // Horizontal divider between HUD blocks (rows) – drag to change block heights
    // =======================================================
    class BlockDivider : public juce::Component
    {
    public:
        BlockDivider (MainComponent& owner, int upperLayer, int lowerLayer)
            : mainComp (owner), upper (upperLayer), lower (lowerLayer)
        {
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        }

        void setLayers (int upperLayer, int lowerLayer) noexcept { upper = upperLayer; lower = lowerLayer; }

        void paint (juce::Graphics& g) override
        {
            if (isMouseOver() || isMouseButtonDown())
                g.fillAll (juce::Colours::grey.withAlpha (0.5f));
        }

        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit  (const juce::MouseEvent&) override { repaint(); }
        void mouseDown  (const juce::MouseEvent& e) override { mainComp.beginBlockResize (e.getScreenY()); }
        void mouseDrag  (const juce::MouseEvent& e) override { mainComp.dragBlockResize (upper, lower, e.getScreenY()); }

    private:
        MainComponent& mainComp;
        int upper;
        int lower;
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
                   std::unique_ptr<juce::Component> inner,
                   std::unique_ptr<ModuleAudioSource> src = nullptr);

        ~PanelHost() override = default;

        int getId() const noexcept { return id; }
        juce::Component* getInner() const noexcept { return view.get(); }
        ModuleAudioSource* getAudioSource() const noexcept { return audioSrc.get(); }
        juce::ValueTree getNode() const { return panel; }

        void resized() override;
        void paintOverChildren (juce::Graphics& g) override;

        // debug/UX: double click toggle (necháš si, kým ladíš drag)
        void mouseDoubleClick (const juce::MouseEvent&) override;

        // drag detach (touchpad OK)
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp   (const juce::MouseEvent& e) override;

        // cursor readout: the inner views get no mouse, so the host tracks the pointer
        void mouseMove  (const juce::MouseEvent& e) override { cursorPos = e.position; cursorIn = true;  repaint(); }
        void mouseEnter (const juce::MouseEvent& e) override { cursorPos = e.position; cursorIn = true;  repaint(); }
        void mouseExit  (const juce::MouseEvent&)   override { cursorIn = false; repaint(); }
        juce::Point<float> cursorPos;
        bool               cursorIn = false;

        // inner views don't intercept mouse – forward wheel for zoom interactivity.
        // Forward ONLY to views that override mouseWheelMove (the base-class
        // implementation bounces the event back to the parent = infinite loop).
        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            if (auto* osc = dynamic_cast<VisualOscilator*> (view.get()))
                osc->mouseWheelMove (e.getEventRelativeTo (osc), wheel);
            // The meter's Level history has its own wheel zoom and its own
            // onLevelHistoryWindowChanged callback, both already wired — it was
            // simply never on this list, so the wheel did nothing over it. The view
            // ignores the event itself in every other meter mode.
            else if (auto* am = dynamic_cast<VisualAudioMeter*> (view.get()))
                am->mouseWheelMove (e.getEventRelativeTo (am), wheel);
        }

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
        std::unique_ptr<ModuleAudioSource> audioSrc;   // per-module routing; must outlive `view`
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

    // Dividers between modules stacked inside one column.
    //
    // POOLED, never rebuilt: dragging one writes kStackWeight, which re-enters the
    // layout through the ValueTree listener. If the layout recreated these, the
    // divider currently delivering the mouseDrag would delete itself mid-callback.
    // So they are only ever re-pointed (setPanels), re-placed and hidden.
    std::vector<std::unique_ptr<StackDivider>> stackDividers;
    size_t stackDividerCursor { 0 };                 // how many are in use this pass
    StackDivider& acquireStackDivider();             // next from the pool, growing it if needed
    void hideUnusedStackDividers();                  // park the surplus after a layout pass

    // Horizontal dividers between HUD blocks (rows)
    std::vector<std::unique_ptr<BlockDivider>> blockDividers;
    int blockDragStartY { 0 };
    std::array<float, 3> blockDragStartWeights { { 1.0f, 1.0f, 1.0f } };
    void beginBlockResize (int screenY);
    void dragBlockResize (int upperLayer, int lowerLayer, int screenY);

    // ── whole-row reattach drop preview ──
    bool rowDropActive = false;   // a RowWindow is being dragged over the HUD
    int  rowDropLayer  = -1;      // which block (its current layer key) is being dragged
    int  rowDropSlot   = 0;       // insertion slot among the visible rows (0..N)
    void updateRowDropPreview (int layer, juce::Point<int> screenPos);
    void endRowDrop           (int layer, juce::Point<int> screenPos);
    std::vector<int> visibleLayersInHud();          // distinct in-HUD layers, ascending
    int  computeRowDropSlot (int screenY);          // 0..visibleLayers.size()

    // =======================================================
    // Detached windows
    // =======================================================
    class PanelWindow;
    std::unordered_map<int, std::unique_ptr<PanelWindow>> windows;

    // One floating window per torn-off block (keyed by layer index)
    class RowWindow;
    std::unordered_map<int, std::unique_ptr<RowWindow>> rowWindows;

    // HUD build/sync
    void rebuildSlotsFromState();
    void forceRebuildAllModules();  // Clear all slots/windows and rebuild
    void showModuleContextMenu (int panelId);   // right-click: Destroy / Hide
    void applyPanelPropsToView (const juce::ValueTree& panel, juce::Component& view);

    /** Parks every module that is currently a Fusion layer and hands the views
        to their Fusion.

        A layer keeps rendering — it is a normal module, it just stops taking space
        in the HUD. JUCE has no "render but do not display" state, so the host is
        left parented (that is what keeps its threads, its size and, for the GL
        modules, isShowing() alive) and simply placed outside the visible area at
        the Fusion's working resolution. Called from layoutActiveViews. */
    void layoutFusionLayers();

    // ====== Plugin control channel (Control-mode automation from DAW) ======
    void pushControlRegistry();     // snapshot modules -> UdpReceiver
    void computeAndPushNeeds();     // per-instance data-needs mask -> UdpReceiver
    void applyControlValue (juce::uint32 moduleId, juce::uint8 paramId, float value);

    // ── Coalescing of the automation traffic ─────────────────────────────────
    // A Control-mode plugin can drive several parameters at once, and each one
    // arrives as its own packet. Applying them one at a time meant one full
    // ValueTree update — and one complete registry rebuild — per packet, on the
    // same message thread that paints every module. Nothing downstream can use
    // more than one value per displayed frame, so incoming values are collapsed
    // per (module, parameter) and applied once per timer tick instead.
    void drainControlQueue();          // apply the collapsed values (message thread)
    void pushControlRegistryIfDirty(); // rebuild the plugin-facing snapshot, rate-limited

    juce::CriticalSection         ctrlQueueLock;
    std::map<juce::uint64, float> ctrlQueue;   // (moduleId << 8 | paramId) -> newest value
    bool                          registryDirty { false };
    juce::uint32                  lastRegistryPushMs { 0 };

    // Resize helpers
    PanelHost* findHostById (int panelId);
    std::vector<PanelHost*> getHudHosts();
    void setModuleWidths (int leftId, int leftWidth, int rightId, int rightWidth);
    void setModuleWidth  (int panelId, int width);   // trailing-edge grip (no neighbour)
    void setStackHeights (int upperId, int upperHeight, int lowerId, int lowerHeight);
    void applyColumnWidth (int panelId, int width);  // width is a property of the whole column

    // ── Columns inside a block ───────────────────────────────────────────────
    // A column is a run of panels in tree order: the first has stackRow 0, the
    // ones stacked under it have 1 and 2. Everything below works on those runs.
    struct Column
    {
        std::vector<PanelHost*> hosts;   // top to bottom, at most AlterState::kMaxStack
        int   fixedW = 0;                // >0 = fixed pixel width (widest member wins)
        float ratio  = 0.0f;             // used when fixedW == 0; 0 until a member sets it
        bool  anyFlexible = false;       // one flexible member makes the column flexible
    };
    std::vector<Column> buildColumns (const std::vector<PanelHost*>& rowHosts);
    std::vector<int>    panelIdsInColumnOf (int panelId);   // whole column, top to bottom
    void layoutColumn (const Column& col, juce::Rectangle<int> area);
    void stackPanelOnto (int panelId, int targetId, bool below);

    // ── stack drop preview (dragging a module ONTO another to stack them) ────
    int  hudStackTargetId = -1;   // panel the ghost is hovering over, -1 = none
    bool hudStackBelow    = true; // drop under (true) or above (false) the target
    bool computeStackDropTarget (juce::Point<int> screenPos);   // updates the two above
    void rebuildResizeDividers (const std::vector<std::pair<int,int>>& pairs);
    std::vector<std::pair<int,int>> dividerPairs;

    // ── Live-resize coalescing (see MainComponent::resized) ──────────────────
    // While a window edge is being dragged, resized() fires on every WM_SIZE —
    // often over a hundred times a second. layoutFusionLayers() is far too
    // expensive to run at that rate, so a SIZE-driven pass is deferred until the
    // drag settles; a structural one still runs immediately.
    juce::Rectangle<int> lastLaidOutSize;
    bool         fusionLayoutDirty { false };
    juce::uint32 lastSizeChangeMs  { 0 };
    // Asks resized() to defer the layer pass even when the HUD's own bounds did
    // not change — set around a divider drag, which resizes modules without
    // resizing the window (see the kStackWeight handler).
    bool         deferFusionLayout { false };

    // lays out one HUD block (row) of hosts; consumes dividers from dividerIdx
    void layoutRow (const std::vector<PanelHost*>& rowHosts,
                    juce::Rectangle<int> rowArea, int& dividerIdx);

    

    // detach/attach helpers (volané async)
    void detachPanelToWindow (int id);
    void attachPanelBackToHud (int id);

    // whole-row (block) tear-off helpers
    void beginRowDetach (int layer, juce::Point<int> dropScreenPos); // sets kRowDetached + kRowWnd
    void syncRowWindows();                // single authority: build/refresh/close RowWindows from state
    void handlePanelWindowDrop (int id, juce::Point<int> centreScreen); // rejoin block / HUD / stay
    std::vector<PanelHost*> getLayerHudHosts (int layer); // in-HUD hosts of one layer, in order
    void layoutRowHandles();              // place per-row grips in the right strip

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
    private:
        std::unique_ptr<juce::Drawable> logo;   // brand mark drawn as the top "letter"
        juce::Colour logoColour;                // colour the cached logo is currently tinted to
    };

    // Right-edge grip: drag it to move the whole (borderless) HUD window.
    class WindowMoveHandle : public juce::Component
    {
    public:
        WindowMoveHandle();
        int  idealWidth() const noexcept { return 16; }   // narrow grip on the right edge
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void paint     (juce::Graphics& g) override;
    private:
        juce::ComponentDragger dragger;
    };

    // Per-row grip (left half of the right strip): drag it OUT of the HUD to tear
    // the whole block (layer) off into its own floating window.
    class RowDetachHandle : public juce::Component
    {
    public:
        RowDetachHandle (MainComponent& owner, int layer);
        static int idealWidth() noexcept { return 14; }
        void setLayer (int l) noexcept { layer = l; }
        int  getLayer() const noexcept { return layer; }

        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp   (const juce::MouseEvent& e) override;
        void paint     (juce::Graphics& g) override;
    private:
        MainComponent&   mainComp;
        int              layer = 0;
        bool             dragging = false;
        bool             dragActive = false;   // moved past threshold
        juce::Point<int> startScreenPos;
    };

    std::unique_ptr<VerticalControllerButton> leftCtrl;
    std::unique_ptr<WindowMoveHandle>         moveHandle;

    // Brand logo shown as a watermark behind an EMPTY HUD (no modules).
    std::unique_ptr<juce::Drawable> emptyLogo;   // recoloured per theme (white on dark / black on light)
    juce::Colour emptyLogoColour;                // colour the cached emptyLogo is tinted to
    bool         hudEmpty = true;                // set by layoutActiveViews: no visible HUD modules
    void refreshEmptyLogo();                     // (re)build emptyLogo for the active theme
    std::vector<std::unique_ptr<RowDetachHandle>> rowHandles;   // one per active layer
    juce::Rectangle<int> rowHandleColumn;                        // left sub-column of right strip
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
