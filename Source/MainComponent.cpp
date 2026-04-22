// MainComponent.cpp
#include "MainComponent.h"
#include "AudioMeter.h"
#include "Spectrum.h"
#include "Oscilator.h"
#include "Synesthesia.h"

#if JUCE_WINDOWS
 #include <Windows.h>
#endif

// =======================================================
// PanelWindow (1 detached window per panel id)
// =======================================================
class MainComponent::PanelWindow : public juce::Component
{
public:
    PanelWindow (juce::ValueTree panelNode, int panelId, PanelHost* hostNonOwning, MainComponent& ownerRef)
        : panel (panelNode), id (panelId), host (hostNonOwning), owner (ownerRef)
    {
        // Create borderless resizable window with taskbar presence (important for OpenGL!)
        addToDesktop (juce::ComponentPeer::windowHasDropShadow |
                      juce::ComponentPeer::windowIsResizable |
                      juce::ComponentPeer::windowAppearsOnTaskbar);

        // Add host as child (non-owned)
        addAndMakeVisible (host);

        applyBoundsFromTree();

        // HIGHEST Z-ORDER: Detached modules always on top
        setAlwaysOnTop (true);

        // Set opaque for better OpenGL performance
        setOpaque (true);

        setVisible (true);
        toFront (true);

        DBG ("PanelWindow created for id=" + juce::String (panelId) + " bounds=" + getBounds().toString());
    }

    ~PanelWindow() override
    {
        // Save bounds before destruction
        saveBoundsToTree();

        // Host is non-owned, just remove it
        removeChildComponent (host);
    }

    void paint (juce::Graphics& g) override
    {
        // Black background for OpenGL content
        g.fillAll (juce::Colours::black);
    }

    void resized() override
    {
        // Fill entire window with host (no borders!)
        if (host)
        {
            host->setBounds (getLocalBounds());
            host->setVisible (true);  // Ensure host is visible

            // Ensure inner view is visible too
            if (auto* inner = host->getInner())
                inner->setVisible (true);
        }

        // Save new size to tree
        saveBoundsToTree();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // Remember where we clicked relative to window top-left
        dragOffset = e.getPosition();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // Calculate new window position: mouse - offset
        auto newPos = e.getScreenPosition() - dragOffset;
        setTopLeftPosition (newPos.x, newPos.y);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        // Save final position
        saveBoundsToTree();

        // Check if dropped in HUD area
        checkReattachToHud();
    }

    void moved() override
    {
        // Save position to tree
        saveBoundsToTree();
    }

    void userTriedToCloseWindow() override
    {
        // Ignore close - controlled by drag/drop only
    }

    // Public getter for host
    PanelHost* getHost() const { return host; }

private:
    void checkReattachToHud()
    {
        // Get actual HUD bounds from MainComponent (not top 3% of screen!)
        const auto hudRect = owner.getHudScreenBounds();

        // Check if window centre is in HUD
        // getBounds() for a top-level (desktop) component already returns screen coords
        const auto centreScreen = getBounds().getCentre();

        if (hudRect.contains (centreScreen))
        {
            DBG ("PanelWindow dropped in HUD -> request reattach id=" + juce::String (id));
            panel.setProperty (AlterState::kDetached, false, nullptr);
        }
    }

    void saveBoundsToTree()
    {
        if (panel.isValid())
        {
            auto b = getBounds();
            panel.setProperty (AlterState::kWnd, b.toString(), nullptr);
        }
    }

    void applyBoundsFromTree()
    {
        auto s = panel.getProperty (AlterState::kWnd).toString();
        if (s.isEmpty())
        {
            setBounds (200, 200, 600, 180);
            return;
        }

        juce::StringArray parts;
        parts.addTokens (s, " ,\t", "");
        parts.removeEmptyStrings();

        if (parts.size() >= 4)
        {
            const int x = parts[0].getIntValue();
            const int y = parts[1].getIntValue();
            const int w = parts[2].getIntValue();
            const int h = parts[3].getIntValue();
            if (w > 0 && h > 0)
            {
                setBounds (x, y, w, h);
                return;
            }
        }

        setBounds (200, 200, 600, 180);
    }

    juce::ValueTree panel;
    int id = -1;
    PanelHost* host = nullptr;  // non-owned
    MainComponent& owner;  // Reference to MainComponent for HUD bounds
    juce::Point<int> dragOffset;  // Offset from window top-left to mouse click
};

// =======================================================
// PanelHost
// =======================================================
MainComponent::PanelHost::PanelHost (MainComponent& o,
                                     juce::ValueTree panelNode,
                                     int panelId,
                                     juce::String panelType,
                                     std::unique_ptr<juce::Component> inner)
    : owner (o), panel (panelNode), id (panelId), type (panelType), view (std::move (inner))
{
    addAndMakeVisible (*view);

    // ✅ kľúč: myš/trackpad eventy idú do hostu, nie do inner view
    view->setInterceptsMouseClicks (false, false);
    setInterceptsMouseClicks (true, true);
}

void MainComponent::PanelHost::resized()
{
    if (view)
    {
        int rotationState = (int) panel.getProperty (AlterState::kRotationAngle, 0);
        rotationState = juce::jlimit (0, 3, rotationState);

        auto bounds = getLocalBounds();

        bool isOpenGLComponent = (dynamic_cast<juce::OpenGLAppComponent*>(view.get()) != nullptr);

        if (rotationState == 0 || isOpenGLComponent)
        {
            view->setBounds (bounds);
            view->setTransform (juce::AffineTransform());
        }
        else if (rotationState == 1 || rotationState == 3)
        {
            const int w = bounds.getWidth();
            const int h = bounds.getHeight();

            view->setBounds (bounds.getX(), bounds.getY(), h, w);

            float angleRadians = (float) rotationState * juce::MathConstants<float>::halfPi;
            auto viewCentre = juce::Point<float> (h / 2.0f, w / 2.0f);
            auto hostCentre = bounds.getCentre().toFloat();

            auto transform = juce::AffineTransform()
                .translated (-viewCentre.x, -viewCentre.y)
                .rotated (angleRadians)
                .translated (hostCentre.x, hostCentre.y);

            view->setTransform (transform);
        }
        else  // 180°
        {
            view->setBounds (bounds);
            auto centre = bounds.getCentre().toFloat();
            view->setTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::pi, centre.x, centre.y));
        }
    }

    if (dragOverlay)
    {
        dragOverlay->setBounds (getLocalBounds());
        dragOverlay->setTransform (juce::AffineTransform());
    }
}

void MainComponent::PanelHost::paintOverChildren (juce::Graphics& g)
{
    // Draw module info text ALWAYS HORIZONTAL in top-right corner
    // Text is metadata ABOUT the module, not part of visualization
    if (!view) return;

    g.setColour (juce::Colours::white.withAlpha (0.9f));
    g.setFont (juce::Font (juce::FontOptions (14.0f)));

    juce::String label;

    // Build detailed label based on module type
    if (type == "spectrum")
    {
        if (auto* spectrum = dynamic_cast<VisualSpectrum*>(view.get()))
        {
            label << "Spectrum"
                  << " | bins: " << spectrum->getDisplayBins()
                  << " | mode: " << (spectrum->isUsingAWeight() ? "A-weighted" : "Flat")
                  << " | ALTF/s: " << juce::String (spectrum->getFftPacketsPerSecond(), 1);
        }
    }
    else if (type == "oscillator" || type == "oscilator")
    {
        if (auto* oscilator = dynamic_cast<VisualOscilator*>(view.get()))
        {
            juce::String modeName = "Mono";
            if (oscilator->getDisplayMode() == VisualOscilator::DisplayMode::Stereo)
                modeName = "Stereo";
            else if (oscilator->getDisplayMode() == VisualOscilator::DisplayMode::Mirror)
                modeName = "Mirror";

            label << "Oscillator (" << modeName << ")";
        }
    }
    else if (type == "rms" || type == "audiometer")
    {
        // AudioMeter handles all rendering in its own paint() (rotation=0, no overlay needed)
    }
    else if (type == "synesthesia")
    {
        label = "Synesthesia";
    }

    // Draw text in top-right corner (ALL modules now use top-right)
    auto bounds = getLocalBounds();
    g.drawText (label, bounds.reduced (6), juce::Justification::topRight, false);
}

void MainComponent::PanelHost::setDragOverlayEnabled (bool enabled)
{
    if (enabled && !dragOverlay)
    {
        dragOverlay = std::make_unique<DragOverlay> (*this);
        addAndMakeVisible (*dragOverlay);
        dragOverlay->setBounds (getLocalBounds());
        // Bring to front for mouse events but don't use setAlwaysOnTop
        dragOverlay->toFront (true);
    }
    else if (!enabled && dragOverlay)
    {
        removeChildComponent (dragOverlay.get());
        dragOverlay.reset();
    }
}

void MainComponent::PanelHost::mouseDoubleClick (const juce::MouseEvent&)
{
    const bool detached = (bool) panel.getProperty (AlterState::kDetached, false);

    if (!detached)
    {
        auto b = localAreaToGlobal (getLocalBounds());
        panel.setProperty (AlterState::kWnd, b.toString(), nullptr);
    }

    // toggle (listener spraví presun async)
    panel.setProperty (AlterState::kDetached, !detached, nullptr);
}

void MainComponent::PanelHost::mouseDown (const juce::MouseEvent& e)
{
    dragging = true;
    startedDetached = (bool) panel.getProperty (AlterState::kDetached, false);
    lastScreenPos = e.getScreenPosition();

    // If detached and in a PanelWindow, forward to parent for dragging
    if (startedDetached)
    {
        if (auto* parent = getParentComponent())
            parent->mouseDown (e.withNewPosition (parent->getLocalPoint (this, e.getPosition())));
        return;
    }

    // selection will occur when an actual drag starts (not on simple click)
}

void MainComponent::PanelHost::mouseDrag (const juce::MouseEvent& e)
{
    if (!dragging) return;

    // if this panel was already detached, forward to PanelWindow for drag handling
    if (startedDetached)
    {
        if (auto* parent = getParentComponent())
            parent->mouseDrag (e.withNewPosition (parent->getLocalPoint (this, e.getPosition())));
        return;
    }

    const auto now = e.getScreenPosition();
    const int dx = std::abs (now.getX() - lastScreenPos.getX());
    const int dy = std::abs (now.getY() - lastScreenPos.getY());
    const int threshold = 6;

    // if movement exceeds threshold, begin HUD drag (if not already)
    if (! owner.hudDragGhost && (dx >= threshold || dy >= threshold))
    {
        owner.beginHudDrag ((int) panel.getProperty (AlterState::kId), now);
    }

    // Only update if ghost and drag are still active
    if (owner.hudDragGhost && owner.hudDragPanelId >= 0)
    {
        owner.updateHudDrag ((int) panel.getProperty (AlterState::kId), now);
    }

    lastScreenPos = now;
}

void MainComponent::PanelHost::mouseUp (const juce::MouseEvent& e)
{
    if (!dragging) return;
    dragging = false;

    // If detached, forward to PanelWindow for drop detection
    if (startedDetached)
    {
        if (auto* parent = getParentComponent())
            parent->mouseUp (e.withNewPosition (parent->getLocalPoint (this, e.getPosition())));
        return;
    }

    // Get current mouse position (not from event, because event might be stale if mouse left window)
    const auto currentMousePos = juce::Desktop::getMousePosition();

    // if we had a HUD ghost (we initiated a drag), finish it
    if (owner.hudDragGhost)
    {
        owner.endHudDrag ((int) panel.getProperty (AlterState::kId), currentMousePos);
        return;
    }

    // Drop from HUD: check if we should detach
    const auto hud = owner.getHudScreenBounds();
    if (! hud.contains (currentMousePos))
    {
        // ulož bounds pre okno + nastav detached
        auto b = localAreaToGlobal (getLocalBounds());
        panel.setProperty (AlterState::kWnd, b.toString(), nullptr);
        panel.setProperty (AlterState::kDetached, true, nullptr);
    }
}

// =======================================================
// VerticalControllerButton paint
// =======================================================
void MainComponent::VerticalControllerButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto b = getLocalBounds().toFloat();
    auto bg = juce::Colours::black.withAlpha (0.85f);
    if (down) bg = bg.brighter (0.20f);
    else if (over) bg = bg.brighter (0.10f);
    g.fillAll (bg);
    g.setColour (juce::Colours::grey.withAlpha (0.85f));
    g.drawRect (b.toNearestInt(), 1);

    const juce::String text = "CONTROLLER";
    const int n = text.length();
    const float pad = 6.0f;
    const float cellH = (b.getHeight() - 2.0f * pad) / juce::jmax (1, n);
    float fontSize = juce::jmin (cellH * 0.9f, b.getWidth() - 6.0f);
    fontSize = juce::jmax (10.0f, fontSize);

    g.setColour (juce::Colours::white.withAlpha (0.95f));
    g.setFont (juce::Font (juce::FontOptions (fontSize)));
    for (int i = 0; i < n; ++i)
    {
        juce::Rectangle<float> cell (b.getX(), b.getY() + pad + i * cellH, b.getWidth(), cellH);
        g.drawFittedText (juce::String::charToString (text[i]), cell.toNearestInt(),
                          juce::Justification::centred, 1);
    }
}

// =======================================================
// MainComponent
// =======================================================
MainComponent::MainComponent (AlterState& s)
    : settings (s)
{
    rebuildSlotsFromState();

    leftCtrl  = std::make_unique<VerticalControllerButton>();
    rightCtrl = std::make_unique<VerticalControllerButton>();
    addAndMakeVisible (*leftCtrl);
    addAndMakeVisible (*rightCtrl);

    auto open = [this]{ if (showController) showController(); };
    leftCtrl->onClick  = open;
    rightCtrl->onClick = open;

    settings.getTree().addListener (this);
    startTimerHz (60);
}

MainComponent::~MainComponent()
{
    settings.getTree().removeListener (this);

    // zavri okná korektne
    windows.clear();
    slots.clear();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    const int sideW = leftCtrl ? leftCtrl->idealWidth() : 28;
    auto left  = area.removeFromLeft (sideW);
    auto right = area.removeFromRight (sideW);
    if (leftCtrl)  leftCtrl->setBounds (left);
    if (rightCtrl) rightCtrl->setBounds (right);

    layoutActiveViews (area);
}

void MainComponent::timerCallback()
{
    // Track mouse globally during HUD drag (so drag works even when mouse leaves HUD window)
    if (hudDragPanelId >= 0 && hudDragGhost != nullptr)
    {
        try
        {
            // Safely get mouse position and update drag
            auto currentMousePos = juce::Desktop::getInstance().getMousePosition();

            // Validate the drag ghost is still on desktop before updating
            if (hudDragGhost->isOnDesktop())
                updateHudDrag (hudDragPanelId, currentMousePos);
            else
            {
                // Ghost was removed from desktop, cleanup drag state
                hudDragPanelId = -1;
                hudDragInsertIndex = -1;
                hudDragGhost.reset();
            }
        }
        catch (...)
        {
            // If anything goes wrong, safely cleanup drag state
            hudDragPanelId = -1;
            hudDragInsertIndex = -1;
            hudDragGhost.reset();
            DBG ("Exception in timerCallback during drag update");
        }
    }

    for (auto& s : slots)
        if (s.host && s.host->getInner())
            s.host->getInner()->repaint();
}

// mark a panel selected in the ValueTree (clears others)
void MainComponent::selectPanelById (int id)
{
    auto panels = settings.getPanelsRoot();
    for (int i = 0; i < panels.getNumChildren(); ++i)
    {
        auto p = panels.getChild (i);
        const int pid = (int) p.getProperty (AlterState::kId);
        p.setProperty (AlterState::kSelected, pid == id, nullptr);
    }
}

// set AlwaysOnTop on detached windows that are marked selected
void MainComponent::setAlwaysOnTopForSelectedPanels (bool on)
{
    for (auto& kv : windows)
    {
        const int id = kv.first;
        auto& win = kv.second;
        auto panel = settings.getPanelById (id);
        if (! panel.isValid()) continue;
        const bool sel = (bool) panel.getProperty (AlterState::kSelected, false);
        if (sel)
            win->setAlwaysOnTop (on);
        else
            win->setAlwaysOnTop (false);
    }
}

// set AlwaysOnTop on ALL detached windows (regardless of selection)
void MainComponent::setAlwaysOnTopForAllDetachedPanels (bool on)
{
    for (auto& kv : windows)
    {
        auto& win = kv.second;
        if (win)
            win->setAlwaysOnTop (on);
    }
}

void MainComponent::layoutActiveViews (juce::Rectangle<int> area)
{
    std::vector<PanelHost*> hudHosts;
    hudHosts.reserve (slots.size());

    for (auto& s : slots)
    {
        if (!s.host) continue;

        const bool detached = (bool) s.node.getProperty (AlterState::kDetached, false);
        if (!detached && s.host->getParentComponent() == this)
            hudHosts.push_back (s.host.get());
    }

    const int gap = 0;
    const int dividerWidth = 4;  // Width of resize divider (overlay, not added to layout)

    // if no hosts, nothing to layout
    if (hudHosts.empty()) return;

    // MOBILE-STYLE DRAG & DROP: show visual placeholder and shift panels
    if (hudDragPanelId >= 0)
    {
        // exclude dragged panel from visible hosts
        std::vector<PanelHost*> visibleHosts;
        visibleHosts.reserve (hudHosts.size());
        for (auto* h : hudHosts)
        {
            if (h->getId() == hudDragPanelId) continue;
            visibleHosts.push_back (h);
        }

        // total slots = visible panels + 1 placeholder slot
        const int totalSlots = (int) visibleHosts.size() + 1;
        const int slotWidth = (area.getWidth() - gap * (totalSlots - 1)) / totalSlots;

        int x = area.getX();
        int visibleIndex = 0;
        const int insertPos = juce::jlimit (0, totalSlots - 1, hudDragInsertIndex);

        // Layout panels and placeholder
        for (int slot = 0; slot < totalSlots; ++slot)
        {
            if (slot == insertPos)
            {
                // Place visual placeholder here (mobile-style gap)
                if (dragPlaceholder)
                {
                    dragPlaceholder->setBounds (x, area.getY(), slotWidth, area.getHeight());
                }
                x += slotWidth + gap;
            }
            else
            {
                // Place actual panel
                if (visibleIndex < (int) visibleHosts.size())
                {
                    auto* host = visibleHosts[(size_t) visibleIndex++];
                    host->setBounds (x, area.getY(), slotWidth, area.getHeight());
                    x += slotWidth + gap;
                }
            }
        }
        return;
    }

    // Normal layout (no drag in progress)
    const int n = (int) hudHosts.size();

    // Rebuild dividers if count changed
    if ((int) resizeDividers.size() != n - 1)
        rebuildResizeDividers();

    const int availableWidth = area.getWidth();

    // Two-pass layout: fixed-width panels first, then flexible panels get remainder
    // Pass 1: calculate fixed-width total
    int fixedTotal = 0;
    float flexRatioSum = 0.0f;
    const float defaultRatio = 1.0f;

    struct HostLayout { int fixedW; float ratio; };
    std::vector<HostLayout> layouts (n);

    for (int i = 0; i < n; ++i)
    {
        auto* slot = findSlotById (hudHosts[i]->getId());
        int prefW = slot ? (int) slot->node.getProperty (AlterState::kPreferredWidth, 0) : 0;

        if (prefW > 0)
        {
            // Fixed-width panel: clamp to reasonable bounds
            prefW = juce::jlimit (30, availableWidth / 2, prefW);
            layouts[i] = { prefW, 0.0f };
            fixedTotal += prefW;
        }
        else
        {
            float ratio = slot ? (float) slot->node.getProperty (AlterState::kWidthRatio, defaultRatio) : defaultRatio;
            layouts[i] = { 0, ratio };
            flexRatioSum += ratio;
        }
    }

    // Pass 2: distribute remaining space to flexible panels
    int flexibleWidth = juce::jmax (0, availableWidth - fixedTotal);
    if (flexRatioSum <= 0.0f) flexRatioSum = 1.0f;

    int x = area.getX();

    for (int i = 0; i < n; ++i)
    {
        auto* host = hudHosts[i];
        int w;

        if (layouts[i].fixedW > 0)
            w = layouts[i].fixedW;
        else
            w = (int) ((layouts[i].ratio / flexRatioSum) * flexibleWidth);

        host->setBounds (x, area.getY(), w, area.getHeight());

        // Place divider OVERLAY at the RIGHT edge of this module (if not last)
        if (i < n - 1 && i < (int) resizeDividers.size())
        {
            auto* divider = resizeDividers[i].get();
            int dividerX = x + w - (dividerWidth / 2);
            divider->setBounds (dividerX, area.getY(), dividerWidth, area.getHeight());
        }

        x += w;
    }
}

// =======================================================
// Helpers
// =======================================================
juce::Rectangle<int> MainComponent::getHudScreenBounds() const
{
    return localAreaToGlobal (getLocalBounds());
}

MainComponent::PanelSlot* MainComponent::findSlotById (int id)
{
    for (auto& s : slots)
        if (s.id == id) return &s;
    return nullptr;
}

// =======================================================
// Detach / attach helpers (volané async)
// =======================================================
void MainComponent::detachPanelToWindow (int id)
{
    auto* slot = findSlotById (id);
    if (!slot || !slot->host) return;

    DBG ("detachPanelToWindow id=" + juce::String (id));

    if (windows.find (id) != windows.end())
        return; // už existuje

    // ak je v HUD, odpoj
    if (slot->host->getParentComponent() == this)
        removeChildComponent (slot->host.get());

    // vytvor okno s referenciou na MainComponent (pre HUD bounds detection)
    windows[id] = std::make_unique<PanelWindow> (slot->node, id, slot->host.get(), *this);

    resized();
    repaint();
}

// reorder panel based on a drop location (screen coordinates)
void MainComponent::reorderPanelByScreenPosition (int panelId, juce::Point<int> screenPos)
{
    // compute HUD hosts in current order
    std::vector<PanelHost*> hudHosts;
    hudHosts.reserve (slots.size());
    for (auto& s : slots)
    {
        if (!s.host) continue;
        const bool detached = (bool) s.node.getProperty (AlterState::kDetached, false);
        if (!detached && s.host->getParentComponent() == this)
            hudHosts.push_back (s.host.get());
    }

    if (hudHosts.empty()) return;

    // gather center x positions in screen coords
    std::vector<int> centers;
    centers.reserve (hudHosts.size());
    for (auto* h : hudHosts)
    {
        const auto r = h->getBounds();
        const auto g = localAreaToGlobal (r);
        centers.push_back (g.getCentreX());
    }

    // find insertion index as first center > screenPos.x
    int targetIndex = (int) centers.size() - 1;
    for (size_t i = 0; i < centers.size(); ++i)
    {
        if (screenPos.x < centers[i]) { targetIndex = (int) i; break; }
    }

    // find panels root and current index of panelId
    auto panels = settings.getPanelsRoot();
    if (! panels.isValid()) return;

    int currentIndex = -1;
    for (int i = 0; i < panels.getNumChildren(); ++i)
    {
        if ((int) panels.getChild (i).getProperty (AlterState::kId) == panelId) { currentIndex = i; break; }
    }
    if (currentIndex < 0) return;

    // clamp targetIndex to valid range (0..numChildren-1)
    targetIndex = juce::jlimit (0, panels.getNumChildren() - 1, targetIndex);

    if (targetIndex == currentIndex) return; // no change

    // move child in ValueTree (this will trigger child order changed listener which rebuilds HUD)
    panels.moveChild (currentIndex, targetIndex, nullptr);
}

void MainComponent::beginHudDrag (int panelId, juce::Point<int> screenPos)
{
    if (hudDragPanelId == panelId) return;
    hudDragPanelId = panelId;
    hudDragInsertIndex = 0;

    auto* slot = findSlotById (panelId);
    if (! slot || ! slot->host || ! slot->host->getInner()) return;

    // select now (on drag start)
    selectPanelById (panelId);

    // create snapshot image of inner component
    juce::Image snap = slot->host->getInner()->createComponentSnapshot (slot->host->getInner()->getLocalBounds());
    hudDragGhost = std::make_unique<juce::ImageComponent>();
    hudDragGhost->setImage (snap);

    // compute offset from top-left of host bounds
    const auto hostGlobal = slot->host->localAreaToGlobal (slot->host->getLocalBounds());
    hudDragOffset = screenPos - hostGlobal.getTopLeft();

    // show ghost as top-level so it can follow mouse across screen
    hudDragGhost->addToDesktop (0);
    hudDragGhost->setTopLeftPosition (screenPos - hudDragOffset);

    // create and show placeholder in HUD
    dragPlaceholder = std::make_unique<DragPlaceholder>();
    addAndMakeVisible (*dragPlaceholder);

    // hide original host while dragging
    slot->host->setVisible (false);

    // trigger layout update
    resized();
}

void MainComponent::updateHudDrag (int panelId, juce::Point<int> screenPos)
{
    if (hudDragPanelId != panelId || ! hudDragGhost) return;

    hudDragGhost->setTopLeftPosition (screenPos - hudDragOffset);

    // compute insertion index based on current hud host centers (excluding dragged)
    std::vector<PanelHost*> hudHosts;
    for (auto& s : slots)
    {
        if (! s.host) continue;
        const bool detached = (bool) s.node.getProperty (AlterState::kDetached, false);
        if (! detached && s.host->getParentComponent() == this && s.id != hudDragPanelId)
            hudHosts.push_back (s.host.get());
    }

    std::vector<int> centers;
    centers.reserve (hudHosts.size());
    for (auto* h : hudHosts)
    {
        const auto r = h->getBounds();
        const auto g = localAreaToGlobal (r);
        centers.push_back (g.getCentreX());
    }

    int insert = (int) centers.size();
    for (size_t i = 0; i < centers.size(); ++i)
        if (screenPos.x < centers[i]) { insert = (int) i; break; }

    hudDragInsertIndex = insert;
    resized();
}

void MainComponent::endHudDrag (int panelId, juce::Point<int> screenPos)
{
    if (hudDragPanelId != panelId) return;

    // find slot (may become invalid after reorder) and capture raw host pointer early
    auto* slot = findSlotById (panelId);
    PanelHost* host = slot ? slot->host.get() : nullptr;

    // IMPORTANT: Reset drag state BEFORE removing ghost to prevent timer from accessing nullptr
    hudDragPanelId = -1;
    hudDragInsertIndex = -1;

    // remove ghost and placeholder
    if (hudDragGhost)
    {
        hudDragGhost->removeFromDesktop();
        hudDragGhost.reset();
    }

    if (dragPlaceholder)
    {
        removeChildComponent (dragPlaceholder.get());
        dragPlaceholder.reset();
    }

    // determine if drop is inside HUD
    const auto hud = getHudScreenBounds();
    if (hud.contains (screenPos))
    {
        // perform reorder in ValueTree (this may rebuild `slots` and invalidate `slot`)
        reorderPanelByScreenPosition (panelId, screenPos);
    }
    else
    {
        // drop outside HUD -> detach to window and save bounds
        juce::Rectangle<int> wBounds;
        if (host)
            wBounds = juce::Rectangle<int> (screenPos.x - hudDragOffset.getX(), screenPos.y - hudDragOffset.getY(), host->getWidth(), host->getHeight());
        else
            wBounds = juce::Rectangle<int> (screenPos.x - hudDragOffset.getX(), screenPos.y - hudDragOffset.getY(), 400, 180);

        auto node = settings.getPanelById (panelId);
        if (node.isValid())
        {
            node.setProperty (AlterState::kWnd, wBounds.toString(), nullptr);
            node.setProperty (AlterState::kDetached, true, nullptr);
        }
    }

    // restore visibility using captured host pointer (slot may be invalid now)
    if (host)
        host->setVisible (true);

    resized();
}


void MainComponent::attachPanelBackToHud (int id)
{
    // zavri a zruš okno
    DBG ("attachPanelBackToHud id=" + juce::String (id));
    auto it = windows.find (id);
    if (it != windows.end())
    {
        it->second->setVisible (false);
        windows.erase (it);
    }

    auto* slot = findSlotById (id);
    if (!slot || !slot->host) return;

    // NOTE: DragOverlay not needed in HUD - mouse events work directly

    if (slot->host->getParentComponent() != this)
        addAndMakeVisible (*slot->host);

    resized();
    repaint();
}

// =======================================================
// ValueTree listener
// =======================================================
void MainComponent::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    // Handle global properties (system gain)
    if (property == AlterState::kSystemGain)
    {
        if (systemAudioSource)
        {
            const float gain = (float) settings.getTree().getProperty (AlterState::kSystemGain, 0.0f);
            systemAudioSource->setGain (gain);
        }
        return;
    }

    auto panels = settings.getPanelsRoot();
    if (!panels.isValid()) return;

    // detach/attach len na kDetached
    if (property == AlterState::kDetached)
    {
        DBG ("valueTreePropertyChanged: kDetached change detected");
        struct Item { int id; bool detached; };
        std::vector<Item> todo;
        todo.reserve ((size_t) panels.getNumChildren());

        for (int i = 0; i < panels.getNumChildren(); ++i)
        {
            auto p = panels.getChild (i);
            todo.push_back ({ (int) p.getProperty (AlterState::kId),
                              (bool) p.getProperty (AlterState::kDetached, false) });
        }

        // ✅ presun UI async (bez reentrancie/crash)
        juce::MessageManager::callAsync ([this, todo]()
        {
            for (auto& it : todo)
            {
                if (it.detached) detachPanelToWindow (it.id);
                else             attachPanelBackToHud (it.id);
            }
        });

        return;
    }

    // iné properties aplikuj do view
    for (int i = 0; i < panels.getNumChildren(); ++i)
    {
        auto p = panels.getChild (i);
        const int id = (int) p.getProperty (AlterState::kId);

        auto* slot = findSlotById (id);
        if (slot && slot->host)
        {
            // If rotation changed, trigger resized() to apply transform
            if (property == AlterState::kRotationAngle)
                slot->host->resized();

            // ── kMeasureState and kMeterView must only apply to the SPECIFIC panel
            // that changed (identified by matching the ValueTree reference).
            // Without this check every audiometer would start/stop simultaneously.
            if (property == AlterState::kMeasureState)
            {
                if (p == tree)  // only the panel whose property actually changed
                {
                    if (auto* meter = dynamic_cast<VisualAudioMeter*> (slot->host->getInner()))
                    {
                        const int measureState = (int) p.getProperty (AlterState::kMeasureState, 0);
                        if (measureState == 1)
                            meter->startMeasurement();
                        else
                            meter->stopMeasurement();
                    }
                }
                continue;  // skip full applyPanelPropsToView for all panels
            }

            // Handle view switch (Momentary <-> Trend) – also per-panel only
            if (property == AlterState::kMeterView)
            {
                if (p == tree)
                {
                    if (auto* meter = dynamic_cast<VisualAudioMeter*> (slot->host->getInner()))
                    {
                        const int view = (int) p.getProperty (AlterState::kMeterView, 0);
                        meter->setTrendMode (view == 1);
                    }
                }
                continue;
            }

            // Apply other properties to inner view
            if (slot->host->getInner())
                applyPanelPropsToView (p, *slot->host->getInner());
        }
    }
}

void MainComponent::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) { rebuildSlotsFromState(); }
void MainComponent::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) { rebuildSlotsFromState(); }
void MainComponent::valueTreeChildOrderChanged (juce::ValueTree&, int, int) { rebuildSlotsFromState(); }
void MainComponent::valueTreeParentChanged (juce::ValueTree&) { rebuildSlotsFromState(); }
void MainComponent::valueTreeRedirected (juce::ValueTree&) { rebuildSlotsFromState(); }

// =======================================================
// HUD build (sync)
// =======================================================
void MainComponent::rebuildSlotsFromState()
{
    auto panels = settings.getPanelsRoot();
    if (!panels.isValid())
        return;

    auto existsInTree = [&] (int id) -> bool
    {
        for (int i = 0; i < panels.getNumChildren(); ++i)
            if ((int) panels.getChild(i).getProperty (AlterState::kId) == id)
                return true;
        return false;
    };

    // 1) remove slots that no longer exist
    for (auto it = slots.begin(); it != slots.end(); )
    {
        if (!existsInTree (it->id))
        {
            // zruš okno ak existuje
            auto wit = windows.find (it->id);
            if (wit != windows.end())
                windows.erase (wit);

            // odpoj z parenta
            if (it->host && it->host->getParentComponent() == this)
                removeChildComponent (it->host.get());

            it = slots.erase (it);
        }
        else
        {
            ++it;
        }
    }

    // 2) build new ordered list, reusing hosts
    std::vector<PanelSlot> newOrder;
    newOrder.reserve ((size_t) panels.getNumChildren());

    auto takeExistingHost = [&] (int id, const juce::String& wantedType) -> std::unique_ptr<PanelHost>
    {
        for (auto& s : slots)
            if (s.id == id && s.type == wantedType)
                return std::move (s.host);
        return nullptr;
    };

    for (int i = 0; i < panels.getNumChildren(); ++i)
    {
        auto p = panels.getChild (i);
        const int id = (int) p.getProperty (AlterState::kId);
        const auto type = p.getProperty (AlterState::kType).toString();

        PanelSlot slot;
        slot.id = id;
        slot.type = type;
        slot.node = p;

        slot.host = takeExistingHost (id, type);

        if (!slot.host)
        {
            std::unique_ptr<juce::Component> view;

            if (type == "rms" || type == "audiometer")
            {
                auto v = std::make_unique<VisualAudioMeter> (*activeAudioSource);
                applyPanelPropsToView (p, *v);
                view = std::move (v);
            }
            else if (type == "spectrum")
            {
                auto v = std::make_unique<VisualSpectrum> (*activeAudioSource);
                applyPanelPropsToView (p, *v);
                view = std::move (v);
            }
            else if (type == "oscillator" || type == "oscilator")
            {
                auto v = std::make_unique<VisualOscilator> (*activeAudioSource);
                // apply panel properties immediately so colour/neon/smooth are set
                applyPanelPropsToView (p, *v);
                view = std::move (v);
            }
            else if (type == "synesthesia")
            {
                auto v = std::make_unique<VisualSynesthesia> (*activeAudioSource);
                applyPanelPropsToView (p, *v);
                view = std::move (v);
            }
            else if (type == "chladni")
            {
                auto v = std::make_unique<ChladniPatternMeter> (*activeAudioSource);
                applyPanelPropsToView (p, *v);
                view = std::move (v);
            }
            else
            {
                continue;
            }

            slot.host = std::make_unique<PanelHost> (*this, p, id, type, std::move (view));
        }

        newOrder.push_back (std::move (slot));
    }

    slots = std::move (newOrder);

    // 3) apply detached state (async presun rieši listener, ale tu to dáme rovno bezpečne async)
    struct Item { int id; bool detached; };
    std::vector<Item> todo;
    todo.reserve (slots.size());

    for (auto& s : slots)
        todo.push_back ({ s.id, (bool) s.node.getProperty (AlterState::kDetached, false) });

    juce::MessageManager::callAsync ([this, todo]()
    {
        for (auto& it : todo)
        {
            if (it.detached) detachPanelToWindow (it.id);
            else             attachPanelBackToHud (it.id);
        }
    });

    resized();
    repaint();
}

// =======================================================
// Apply panel props
// =======================================================
void MainComponent::applyPanelPropsToView (const juce::ValueTree& panel, juce::Component& view)
{
    const auto type = panel.getProperty (AlterState::kType).toString();

    if (type == "rms" || type == "audiometer")
    {
        auto* v = dynamic_cast<VisualAudioMeter*> (&view);
        if (v == nullptr) return;

        const int meterMode = (int) panel.getProperty (AlterState::kMeterMode, 0);
        if (meterMode == 0)      v->setMeterMode (VisualAudioMeter::MeterMode::RMS);
        else if (meterMode == 1) v->setMeterMode (VisualAudioMeter::MeterMode::TruePeak);
        else                     v->setMeterMode (VisualAudioMeter::MeterMode::LUFS);

        const int meterView = (int) panel.getProperty (AlterState::kMeterView, 0);
        v->setTrendMode (meterView == 1);

        const float smooth = (float) panel.getProperty (AlterState::kSmooth, 0.5f);
        v->setSmoothAmount (smooth);

        const int colorModeInt = (int) panel.getProperty (AlterState::kColorMode, 0);
        VisualAudioMeter::ColorMode colorMode = VisualAudioMeter::ColorMode::Standard;
        if (colorModeInt == 1) colorMode = VisualAudioMeter::ColorMode::CustomGradient;
        else if (colorModeInt == 2) colorMode = VisualAudioMeter::ColorMode::CustomSpectrum;
        v->setColorMode (colorMode);

        if (colorMode != VisualAudioMeter::ColorMode::Standard && panel.hasProperty (AlterState::kColor))
            v->setBarColour (juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor)));
        else
            v->setBarColour (juce::Colours::limegreen);

        const int rotAngle = (int) panel.getProperty (AlterState::kRotationAngle, 0);
        v->setHostRotation (rotAngle);
    }
    else if (type == "spectrum")
    {
        auto* v = dynamic_cast<VisualSpectrum*> (&view);
        if (v == nullptr) return;

        const float smooth      = (float) panel.getProperty (AlterState::kSmooth,          0.5f);
        const int   bins        = (int)   panel.getProperty (AlterState::kBins,            2048);
        const int   psycho      = (int)   panel.getProperty (AlterState::kPsychoCurve,     0);
        const bool  peakHold    = (bool)  panel.getProperty (AlterState::kPeakHold,        false);
        const int   measurement = (int)   panel.getProperty (AlterState::kMeasurementMode, 0);
        const int   phon        = (int)   panel.getProperty (AlterState::kPhon,            60);

        v->setSmoothAmount      (smooth);
        v->setDisplayBins       (bins);
        v->setPsychoacousticMode(psycho, phon);
        v->setPeakHoldEnabled   (peakHold);
        v->setMeasurementMode   (measurement == 1);

        if (panel.hasProperty (AlterState::kColor))
            v->setLineColour (juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor)));
    }
    else if (type == "oscillator" || type == "oscilator")
    {
        auto* v = dynamic_cast<VisualOscilator*> (&view);
        if (v == nullptr) return;

        if (panel.hasProperty (AlterState::kColor))
            v->setColour (juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor)));

        if (panel.hasProperty (AlterState::kSmooth))
            v->setSmoothAmount ((float) panel.getProperty (AlterState::kSmooth, 0.5f));

        if (panel.hasProperty (AlterState::kNeon))
            v->setFill ((bool) panel.getProperty (AlterState::kNeon, false));

        if (panel.hasProperty (AlterState::kDisplayMode))
        {
            const int dm = (int) panel.getProperty (AlterState::kDisplayMode, 0);
            if (dm == 1)      v->setDisplayMode (VisualOscilator::DisplayMode::Stereo);
            else if (dm == 2) v->setDisplayMode (VisualOscilator::DisplayMode::Mirror);
            else              v->setDisplayMode (VisualOscilator::DisplayMode::Mono);
        }

        if (panel.hasProperty (AlterState::kZoom))
            v->setZoom ((float) panel.getProperty (AlterState::kZoom, 0.420f));
    }
    else if (type == "synesthesia")
    {
        auto* v = dynamic_cast<VisualSynesthesia*> (&view);
        if (v == nullptr) return;

        if (panel.hasProperty (AlterState::kSmooth))
            v->setSmoothAmount ((float) panel.getProperty (AlterState::kSmooth, 0.15f));
        if (panel.hasProperty (AlterState::kSyncBPM))
            v->setSyncToBPM ((bool) panel.getProperty (AlterState::kSyncBPM, true));
        if (panel.hasProperty (AlterState::kZoom))
            v->setZoom ((float) panel.getProperty (AlterState::kZoom, 1.0f));
        if (panel.hasProperty (AlterState::kRotation))
            v->setRotation ((float) panel.getProperty (AlterState::kRotation, 0.0f));
        if (panel.hasProperty (AlterState::kSymmetry))
            v->setSymmetry ((int) panel.getProperty (AlterState::kSymmetry, 1));
        if (panel.hasProperty (AlterState::kSaturation))
            v->setSaturation ((float) panel.getProperty (AlterState::kSaturation, 1.0f));
        if (panel.hasProperty (AlterState::kBloom))
            v->setBloom ((float) panel.getProperty (AlterState::kBloom, 0.0f));
    }
    else if (type == "chladni")
    {
        auto* v = dynamic_cast<ChladniPatternMeter*> (&view);
        if (v == nullptr) return;

        v->setM             ((int)   panel.getProperty (AlterState::kChladniM,        2));
        v->setN             ((int)   panel.getProperty (AlterState::kChladniN,        3));
        v->setAspectRatio   ((float) panel.getProperty (AlterState::kChladniAR,       1.0f));
        v->setSandSharpness ((float) panel.getProperty (AlterState::kChladniSharp,    0.5f));
        v->setMaterial      ((int)   panel.getProperty (AlterState::kChladniMaterial, 0));

        if (panel.hasProperty (AlterState::kColor))
            v->setSandColour (juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor)));
    }
}

void MainComponent::forceRebuildAllModules()
{
    // Clear all slots and windows to force complete rebuild
    // This is necessary when audio source changes because visual modules
    // store audio source as reference that can't be changed after construction
    slots.clear();
    windows.clear();
    rebuildSlotsFromState();
}

// =======================================================
// Resize helpers
// =======================================================
MainComponent::PanelHost* MainComponent::findHostById (int panelId)
{
    for (auto& slot : slots)
    {
        if (slot.id == panelId && slot.host)
            return slot.host.get();
    }
    return nullptr;
}

std::vector<MainComponent::PanelHost*> MainComponent::getHudHosts()
{
    std::vector<PanelHost*> hudHosts;
    for (auto& s : slots)
    {
        if (!s.host) continue;
        const bool detached = (bool) s.node.getProperty (AlterState::kDetached, false);
        if (!detached && s.host->getParentComponent() == this)
            hudHosts.push_back (s.host.get());
    }
    return hudHosts;
}

void MainComponent::setModuleWidths (int leftId, int leftWidth, int rightId, int rightWidth)
{
    auto* leftSlot = findSlotById (leftId);
    auto* rightSlot = findSlotById (rightId);
    if (!leftSlot || !rightSlot) return;

    // Get all HUD modules to calculate total width and ratios
    auto hudHosts = getHudHosts();
    if (hudHosts.size() < 2) return;

    // Calculate total width of ALL modules
    int totalWidth = 0;
    for (auto* host : hudHosts)
        totalWidth += host->getWidth();

    if (totalWidth <= 0) return;

    // Update fixed-width (preferredWidth) panels with new pixel width
    bool leftFixed = ((int) leftSlot->node.getProperty (AlterState::kPreferredWidth, 0) > 0);
    bool rightFixed = ((int) rightSlot->node.getProperty (AlterState::kPreferredWidth, 0) > 0);

    if (leftFixed)
        leftSlot->node.setProperty (AlterState::kPreferredWidth, juce::jmax (30, leftWidth), nullptr);
    if (rightFixed)
        rightSlot->node.setProperty (AlterState::kPreferredWidth, juce::jmax (30, rightWidth), nullptr);

    // Calculate new ratios for left and right modules
    float leftNewRatio = (float) leftWidth / (float) totalWidth;
    float rightNewRatio = (float) rightWidth / (float) totalWidth;

    // Calculate sum of OTHER modules' ratios (those not being resized)
    float otherRatiosSum = 0.0f;
    for (auto* host : hudHosts)
    {
        int id = host->getId();
        if (id != leftId && id != rightId)
        {
            auto* slot = findSlotById (id);
            if (slot)
            {
                float ratio = (float) slot->node.getProperty (AlterState::kWidthRatio, 1.0f / (float) hudHosts.size());
                otherRatiosSum += ratio;
            }
        }
    }

    // Normalize: left + right + others should = 1.0
    float newRatioSum = leftNewRatio + rightNewRatio;
    float remainingRatio = 1.0f - newRatioSum;

    // Scale other modules' ratios to fit remaining space
    float scale = (otherRatiosSum > 0.0f) ? (remainingRatio / otherRatiosSum) : 1.0f;

    for (auto* host : hudHosts)
    {
        int id = host->getId();
        auto* slot = findSlotById (id);
        if (!slot) continue;

        if (id == leftId)
        {
            slot->node.setProperty (AlterState::kWidthRatio, leftNewRatio, nullptr);
        }
        else if (id == rightId)
        {
            slot->node.setProperty (AlterState::kWidthRatio, rightNewRatio, nullptr);
        }
        else
        {
            // Scale other modules proportionally
            float oldRatio = (float) slot->node.getProperty (AlterState::kWidthRatio, 1.0f / (float) hudHosts.size());
            float newRatio = oldRatio * scale;
            slot->node.setProperty (AlterState::kWidthRatio, newRatio, nullptr);
        }
    }

    // Trigger layout update
    resized();
}

void MainComponent::rebuildResizeDividers()
{
    // Clear old dividers
    for (auto& divider : resizeDividers)
        removeChildComponent (divider.get());
    resizeDividers.clear();

    // Count visible HUD modules
    std::vector<PanelHost*> hudHosts;
    for (auto& s : slots)
    {
        if (!s.host) continue;
        const bool detached = (bool) s.node.getProperty (AlterState::kDetached, false);
        if (!detached && s.host->getParentComponent() == this)
            hudHosts.push_back (s.host.get());
    }

    // Create dividers between modules (n modules = n-1 dividers)
    const int n = (int) hudHosts.size();
    if (n < 2) return;  // No dividers needed for 0 or 1 module

    for (int i = 0; i < n - 1; ++i)
    {
        auto divider = std::make_unique<ResizeDivider> (*this, hudHosts[i]->getId());
        addAndMakeVisible (*divider);
        resizeDividers.push_back (std::move (divider));
    }
}
