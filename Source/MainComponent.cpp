// MainComponent.cpp
#include "MainComponent.h"
#include "AudioMeter.h"
#include "Spectrum.h"
#include "Oscilator.h"
#include "Synesthesia.h"
#include "QspLogoData.h"

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
        // Visible BEFORE the peer exists: AppKit asks canBecomeKeyWindow while the
        // window is being ordered in, and JUCE answers NO for a component that is
        // still invisible -> "makeKeyWindow ... returned NO" in the console.
        setVisible (true);

        // Create borderless resizable window with taskbar presence (important for OpenGL!)
        addToDesktop (juce::ComponentPeer::windowHasDropShadow |
                      juce::ComponentPeer::windowIsResizable |
                      juce::ComponentPeer::windowAppearsOnTaskbar);

        // A detached window hosts a single module and does NOT need GPU
        // acceleration. Creating a second OpenGL context per detached window at
        // runtime is fragile (driver crash), so detached windows render on the
        // normal path instead. The HUD keeps its shared GL host where it matters.
        setOpaque (true);

        // Add host as child (non-owned)
        addAndMakeVisible (host);

        applyBoundsFromTree();

        // Z-order follows the controller's "Always on top" checkbox — a detached
        // module must be coverable by other apps when the option is OFF.
        setAlwaysOnTop (owner.settings.controllerAlwaysOnTop());

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
        AlterTheme::paintBackground (g, getLocalBounds().toFloat());
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

        // Decide where this module was dropped: back into a torn-off block,
        // into the HUD, or left floating. (Handled by MainComponent, which can
        // see the RowWindows — defined later in this file.)
        owner.handlePanelWindowDrop (id, getBounds().getCentre());
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
// RowWindow (1 detached window per torn-off block / layer)
// Hosts every module of that layer side-by-side.
// =======================================================
class MainComponent::RowWindow : public juce::Component
{
public:
    RowWindow (int layerIndex,
               std::vector<PanelHost*> layerHosts,
               juce::Rectangle<int> initialBounds,
               MainComponent& ownerRef)
        : layer (layerIndex), hosts (std::move (layerHosts)), owner (ownerRef)
    {
        setVisible (true);   // before the peer exists - see PanelWindow

        addToDesktop (juce::ComponentPeer::windowHasDropShadow |
                      juce::ComponentPeer::windowIsResizable |
                      juce::ComponentPeer::windowAppearsOnTaskbar);

        // Like PanelWindow: no per-window GL context (fragile). Normal render path.
        setOpaque (true);

        for (auto* h : hosts)
            if (h) addAndMakeVisible (h);

        if (initialBounds.getWidth() > 20 && initialBounds.getHeight() > 20)
            setBounds (initialBounds);
        else
            setBounds (220, 220, 640, 200);

        // follows the controller's "Always on top" checkbox (see PanelWindow)
        setAlwaysOnTop (owner.settings.controllerAlwaysOnTop());
        setVisible (true);
        toFront (true);

        DBG ("RowWindow created for layer=" + juce::String (layer)
             + " hosts=" + juce::String ((int) hosts.size()));
    }

    ~RowWindow() override
    {
        saveBoundsToNodes();
        for (auto* h : hosts)
            if (h) removeChildComponent (h);
    }

    void paint (juce::Graphics& g) override
    {
        AlterTheme::paintBackground (g, getLocalBounds().toFloat());
    }

    void resized() override
    {
        const int n = (int) hosts.size();
        if (n <= 0) return;

        auto b = getLocalBounds();
        const int w = b.getWidth() / n;
        int x = b.getX();
        for (int i = 0; i < n; ++i)
        {
            const int cw = (i == n - 1) ? (b.getRight() - x) : w;
            if (hosts[(size_t) i])
            {
                hosts[(size_t) i]->setBounds (x, b.getY(), cw, b.getHeight());
                hosts[(size_t) i]->setVisible (true);
                if (auto* inner = hosts[(size_t) i]->getInner())
                    inner->setVisible (true);
            }
            x += cw;
        }
        saveBoundsToNodes();
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        // NOTE: events are forwarded from a child PanelHost that does NOT fill the
        // window, so the event's getScreenPosition()/getPosition() are unreliable.
        // Use the real desktop mouse position and remember its offset from the
        // window's top-left, so the grab point stays exactly under the cursor.
        dragOffset = juce::Desktop::getMousePosition() - getScreenPosition();
        setAlpha (0.6f);   // see the HUD drop-preview underneath while dragging
    }

    void mouseDrag (const juce::MouseEvent&) override
    {
        const auto mouse = juce::Desktop::getMousePosition();
        setTopLeftPosition (mouse.x - dragOffset.x, mouse.y - dragOffset.y);

        // live "where will it land" preview inside the HUD
        owner.updateRowDropPreview (layer, mouse);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        setAlpha (1.0f);
        saveBoundsToNodes();
        // decide drop: reattach at the previewed slot, or stay floating
        owner.endRowDrop (layer, juce::Desktop::getMousePosition());
    }

    void moved() override { saveBoundsToNodes(); }

    void userTriedToCloseWindow() override
    {
        // Close = send the whole block back to the HUD
        for (auto* h : hosts)
            if (h) h->getNode().setProperty (AlterState::kRowDetached, false, nullptr);
    }

    const std::vector<PanelHost*>& getHosts() const { return hosts; }

    // Refresh which modules live in this block. Modules that left (separated to
    // their own window, or moved to the HUD) are dropped; new/rejoined ones are
    // adopted. Keeps the block's layout tight — no leftover empty slots.
    void setHosts (const std::vector<PanelHost*>& newHosts)
    {
        for (auto* h : hosts)
            if (h && std::find (newHosts.begin(), newHosts.end(), h) == newHosts.end())
                removeChildComponent (h);

        hosts = newHosts;

        for (auto* h : hosts)
            if (h) addAndMakeVisible (h);   // reparents into this window if needed

        resized();
    }

private:
    void saveBoundsToNodes()
    {
        const auto s = getBounds().toString();
        for (auto* h : hosts)
            if (h && h->getNode().isValid())
                h->getNode().setProperty (AlterState::kRowWnd, s, nullptr);
    }

    int layer = 0;
    std::vector<PanelHost*> hosts;   // non-owned (slots keep ownership)
    MainComponent& owner;
    juce::Point<int> dragOffset;
};

// =======================================================
// PanelHost
// =======================================================
MainComponent::PanelHost::PanelHost (MainComponent& o,
                                     juce::ValueTree panelNode,
                                     int panelId,
                                     juce::String panelType,
                                     std::unique_ptr<juce::Component> inner,
                                     std::unique_ptr<ModuleAudioSource> src)
    : owner (o), panel (panelNode), id (panelId), type (panelType),
      audioSrc (std::move (src)), view (std::move (inner))
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

        // A panel that has been lent to a Fusion is never drawn in its own
        // rectangle, so a transform here would do nothing visible — worse, it would
        // swap the inner bounds and hand Fusion a frame of the wrong shape.
        // The rotation of a layer is applied by the fusion shader instead; the
        // layer itself stays upright and is simply laid out with its width and
        // height already exchanged (see layoutFusionLayers).
        const bool isFusionLayer = (int) panel.getProperty (AlterState::kFusionHost, 0) > 0;

        if (rotationState == 0 || isOpenGLComponent || isFusionLayer)
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

// Builds the top-right info line for ONE module: its name + the parameters that
// are actually set. Split out of paintOverChildren so a Fusion can reuse it to
// label each of its layers with exactly what that module would say for itself in
// the HUD. `view` is the module's live component (may be null) — used only for the
// few readouts that are not stored in the tree (peak text, detected notes…).
static juce::String buildModuleInfoLabel (const juce::String& type,
                                          const juce::ValueTree& panel,
                                          juce::Component* view)
{
    auto pInt   = [&] (const juce::Identifier& k, int   d) { return (int)   panel.getProperty (k, d); };
    auto pBool  = [&] (const juce::Identifier& k, bool  d) { return (bool)  panel.getProperty (k, d); };
    auto pFloat = [&] (const juce::Identifier& k, float d) { return (float) panel.getProperty (k, d); };

    // global Constant-Q lives on the tree root (panel's topmost parent)
    const bool globalCq = (bool) panel.getRoot().getProperty (AlterState::kGlobalCqt, false);

    juce::String label;

    if (type == "spectrum")
    {
        if (auto* s = dynamic_cast<VisualSpectrum*> (view))
        {
            static const char* curve[] = { "Flat", "A-weight", "Fletcher-Munson" };
            static const char* ref[]   = { "", " | EDM", " | Bass", " | House", " | Hip-Hop", " | Pop", " | Rock" };
            const bool cq = pBool (AlterState::kConstantQ, false) || globalCq;
            label << "Spectrum | "
                  << (cq ? juce::String ("Constant-Q") : juce::String (s->getDisplayBins()) + " bins") << " | "
                  << curve[juce::jlimit (0, 2, pInt (AlterState::kPsychoCurve, 0))]
                  << ref  [juce::jlimit (0, 6, pInt (AlterState::kSpecReference, 0))];
            if (pInt  (AlterState::kMeasurementMode, 0) == 1) label << " | Measure";
            if (pBool (AlterState::kSpecStereo, false))       label << " | Stereo";
            if (pBool (AlterState::kPeakHold,   false))       label << " | PkHold";
            const auto pk = s->getPeakText();
            if (pk.isNotEmpty()) label << " | " << pk;
        }
    }
    else if (type == "oscillator" || type == "oscilator")
    {
        if (auto* o = dynamic_cast<VisualOscilator*> (view))
        {
            static const char* term[] = { "Short-Term Scope", "Long Waveform" };
            const int t = juce::jlimit (0, 1, pInt (AlterState::kOscLongTerm, 0));
            juce::String disp = "Mono";
            switch (o->getDisplayMode())
            {
                case VisualOscilator::DisplayMode::Stereo:       disp = "Stereo";        break;
                case VisualOscilator::DisplayMode::Mirror:       disp = "Mirror";        break;
                case VisualOscilator::DisplayMode::MirrorStereo: disp = "Mirror Stereo"; break;
                case VisualOscilator::DisplayMode::Mono:
                default:                                         disp = "Mono";          break;
            }
            label << "Oscilloscope | " << term[t] << " | " << disp;
            if (t != 0) label << " | " << juce::String (pFloat (AlterState::kOscLtWindow, 10.0f), 1) << " s";
            if (o->getDisplayMode() == VisualOscilator::DisplayMode::Stereo)
                label << (pInt (AlterState::kOscLrColor, 0) == 1 ? " | analogous" : " | complementary");
            if (pInt (AlterState::kColorMode, 0) == 1) label << " | Tone colour";
        }
    }
    else if (type == "stereoscope")
    {
        static const char* sm[] = { "Particles", "Goniometer", "Polar", "Correlation", "Correlometer" };
        const int m = juce::jlimit (0, 4, pInt (AlterState::kStereoMode, 0));
        label << "Stereoscope | " << sm[m];
        if (m == 0 || m == 1)
            label << " | density " << juce::String ((int) std::round (pFloat (AlterState::kStereoDensity, 0.5f) * 100.0f)) << "%";
        if (m == 1 && pBool (AlterState::kStereoParticles, false)) label << " | cloud";
        if (pInt (AlterState::kColorMode, 0) == 1) label << " | Tone colour";
    }
    else if (type == "spectrogram")
    {
        if (auto* sg = dynamic_cast<SpectrogramMeter*> (view))
        {
            label << "Spectrogram | 16 Hz-20 kHz | "
                  << juce::String (sg->getTimeWindow(), sg->getTimeWindow() < 10.0f ? 1 : 0) << " s";
            if (pBool (AlterState::kSpectroReassign, false)) label << " | Enhanced freq";
            else if (pBool (AlterState::kConstantQ, false) || globalCq) label << " | Constant-Q";
            if (pBool (AlterState::kSpectroMirror, false)) label << " | mirrored";
            if (pInt (AlterState::kColorMode, 0) == 2) label << " | Tone colour";
        }
    }
    else if (type == "toneanalyzer")
    {
        label << "Tone Analyzer | sens "
              << juce::String ((int) std::round (pFloat (AlterState::kToneSens, 0.5f) * 100.0f)) << "%";
        if (pBool (AlterState::kToneTuner, false)) label << " | Tuner";
        if (auto* ta = dynamic_cast<ToneAnalyzerMeter*> (view))
        {
            label << " | " << (ta->isMidiSource() ? "MIDI" : "FFT");   // where the notes come from
            const auto det = ta->getDetectedText();
            if (det.isNotEmpty()) label << " | " << det;   // live detected note/chord
        }
        if (pInt (AlterState::kColorMode, 0) == 1) label << " | Tone colour";
    }
    else if (type == "chladni")
    {
        if (auto* ch = dynamic_cast<ChladniPatternMeter*> (view))
            label << "Chladni " << ch->getInfoText();
    }
    else if (type == "synesthesia")
    {
        label << "Synesthesia | sym " << pInt (AlterState::kSymmetry, 1)
              << " | speed " << juce::String (pFloat (AlterState::kSpeed, 1.0f), 1) << "x"
              << " | " << (pInt (AlterState::kColorMode, 1) == 1 ? "Tone colour" : "Manual");
        if (pBool (AlterState::kMirror, false)) label << " | mirror";
    }
    else if (type == "geometry")
    {
        label << "Geometry | " << pInt (AlterState::kGeoComplexity, 12) << " layers | sym "
              << pInt (AlterState::kSymmetry, 1) << " | "
              << (pBool (AlterState::kMirror, false) ? "mirror | " : "")
              << (pInt (AlterState::kColorMode, 0) == 1 ? "Tone colour" : "Manual");
    }
    else if (type == "rms" || type == "audiometer")
    {
        // Used to draw NOTHING (the meter has its own numeric readout inside the
        // panel), which left it the one module with a bare badge and no name. A
        // compact mode line puts it on equal footing with the rest — and inside a
        // Fusion, where the readout is composited away, it is the only label there is.
        static const char* mm[] = { "RMS", "True Peak", "LUFS", "Level history" };
        const int m = juce::jlimit (0, 3, pInt (AlterState::kMeterMode, 0));
        label << "Audio Meter | " << mm[m];

        // Momentary / Trend applies to the three BAR modes. It used to be printed
        // only when m == 3, which is Level history — the one mode that has no such
        // view at all (applyPanelPropsToView reads it as `meterMode != 3 && ...`).
        // So the label named the view exactly where it did not exist and stayed
        // silent everywhere it did.
        const bool trend = (m != 3) && pInt (AlterState::kMeterView, 0) == 1;
        if (m != 3)
        {
            static const char* vm[] = { "Momentary", "Trend" };
            label << " | " << vm[juce::jlimit (0, 1, pInt (AlterState::kMeterView, 0))];
        }

        if (pBool (AlterState::kMeterToneColor, false))
        {
            static const char* ts[] = { "Gradient", "Complementary" };
            label << " | Tone colour | " << ts[juce::jlimit (0, 1, pInt (AlterState::kMeterToneShade, 0))];
        }
        else
        {
            static const char* cm[] = { "", " | Gradient", " | Complementary" };
            label << cm[juce::jlimit (0, 2, pInt (AlterState::kColorMode, 0))];
        }

        label << " | smooth " << juce::String ((int) std::round (pFloat (AlterState::kSmooth, 0.5f) * 100.0f)) << "%";

        // THE AVERAGE OF THE WHOLE TREND, alongside the live number the meter already
        // prints in its own strip. A trend curve answers "how did this move"; the one
        // thing it cannot be read off by eye is where it sat ON BALANCE, which for a
        // loudness pass is the number you actually deliver against. It appears only
        // in the Trend view and only once something has been captured — an "AVG" with
        // nothing behind it would be the readout lying about having measured.
        if (trend)
        {
            if (auto* am = dynamic_cast<VisualAudioMeter*> (view))
            {
                // The capture's own clock and state. It used to live in the strip
                // along the module's bottom edge; that strip is gone, and a Trend view
                // that does not say whether it is still recording is missing the one
                // thing you check while it runs.
                const int pts = am->getMeasurementPointCount();
                if (pts > 0 || am->isMeasurementActive())
                {
                    const int secs = (int) am->getMeasurementElapsedSec();
                    label << (am->isMeasurementActive() ? " | REC " : " | STOP ")
                          << juce::String::formatted ("%02d:%02d", secs / 60, secs % 60);
                }

                float avg = 0.0f;
                if (am->getTrendAverageDb (avg))
                {
                    static const char* unit[] = { " dBFS", " dBTP", " LUFS" };
                    label << " | AVG " << juce::String (avg, 1) << unit[juce::jlimit (0, 2, m)];
                }
            }
        }
    }
    else if (type == "fusion")
    {
        // The Fusion's OWN settings row: how many layers it holds, plus each global
        // post stage that is doing something (silent ones are left off so the line
        // says only what is true right now).
        const int myId = pInt (AlterState::kId, -1);
        auto panels = panel.getParent();
        int n = 0;
        if (panels.isValid())
            for (int i = 0; i < panels.getNumChildren(); ++i)
                if ((int) panels.getChild (i).getProperty (AlterState::kFusionHost, 0) == myId)
                    ++n;

        label << "Fusion | " << n << (n == 1 ? " layer" : " layers");

        const int sym = pInt (AlterState::kFusionSymmetry, 1);
        if (sym > 1) label << " | sym " << sym;
        const int mir = pInt (AlterState::kFusionMirror, 0);
        if (mir > 0) label << " | mirror " << mir;
        if (std::abs (pFloat (AlterState::kFusionVortex, 0.0f)) > 0.001f) label << " | vortex";
        const float fz = pFloat (AlterState::kFusionZoom, 1.0f);
        if (std::abs (fz - 1.0f) > 0.001f) label << " | zoom " << juce::String (fz, 2) << "x";
        if (pFloat (AlterState::kSpeed, 0.0f) > 0.001f)
            label << " | spin " << juce::String (pFloat (AlterState::kSpeed, 0.0f), 1);
        if (pBool (AlterState::kFusionWarp, false)) label << " | warp";
        if (pBool (AlterState::kFusionTunnel, false)) label << " | tunnel";
        if (pBool (AlterState::kFusionLiquid, false)) label << " | liquid";
        if (pFloat (AlterState::kFusionReact, 0.0f) > 0.001f)
            label << " | react " << juce::String ((int) std::round (pFloat (AlterState::kFusionReact, 0.0f) * 100.0f)) << "%";

        // Which layers the global chain targets — only worth saying when it is NOT
        // all of them (the default), otherwise it is just noise on every fusion.
        const bool g0 = pBool (AlterState::kFusionGlobL0, true);
        const bool g1 = pBool (AlterState::kFusionGlobL1, true);
        const bool g2 = pBool (AlterState::kFusionGlobL2, true);
        if (! (g0 && g1 && g2))
        {
            label << " | glob ";
            if (g0) label << "A";
            if (g1) label << "B";
            if (g2) label << "C";
            if (! (g0 || g1 || g2)) label << "none";
        }
    }

    return label;
}

void MainComponent::PanelHost::paintOverChildren (juce::Graphics& g)
{
    if (!view) return;

    // Cursor readout (Spectrum: freq + dB; Spectrogram: freq; Oscilloscope and the
    // meter's Level history: time + dB). The inner view gets no mouse events, so the
    // host tracks the pointer and asks the view for the text.
    //
    // The time-domain views get the crosshair too, but only the HORIZONTAL arm: the
    // vertical one would be a second, moving line across a picture that is already a
    // line, and at any zoom where the trace is dense it reads as part of the
    // waveform. The frequency views draw both because their x means something you
    // point AT. Views that have nothing to say return an empty string and nothing is
    // drawn at all — that is how Level history stays silent in the bar modes.
    // Containment is tested in the VIEW's own space for the same reason the point
    // is mapped there: getBounds() is the pre-transform rectangle, so on a
    // quarter-turned panel it is the wrong shape in the wrong place and the readout
    // would switch on and off over the wrong parts of the module.
    const juce::Point<float> viewPos = view->getLocalPoint (this, cursorPos);

    if (cursorIn && view->getLocalBounds().toFloat().contains (viewPos))
    {
        juce::String rd;

        // getLocalPoint, NOT a subtraction of the view's origin.
        //
        // A rotated panel carries an AffineTransform (see PanelHost::resized), and
        // subtracting the origin ignores it completely: at 180° the point handed to
        // the view was mirrored through the centre, so the readout reported 0 dB
        // where the scale said -60 and vice versa. At 90° it was transposed. Every
        // module with a cursor readout had this — the meter is simply where it is
        // most obvious, because its scale is printed right next to the number.
        //
        // getLocalPoint walks the hierarchy applying inverse transforms, so it is
        // correct for all four rotations and stays correct if the transform ever
        // becomes something other than a quarter turn.
        const juce::Point<float> vp = viewPos;
        bool vertical = false;
        if (auto* s  = dynamic_cast<VisualSpectrum*>   (view.get())) { rd = s->cursorText (vp); vertical = true; }
        else if (auto* sg = dynamic_cast<SpectrogramMeter*> (view.get())) { rd = sg->cursorText (vp); }
        else if (auto* o  = dynamic_cast<VisualOscilator*>  (view.get())) { rd = o->cursorText (vp); }
        else if (auto* am = dynamic_cast<VisualAudioMeter*> (view.get())) { rd = am->cursorText (vp); }

        if (rd.isNotEmpty())
        {
            auto b = getLocalBounds().toFloat();
            g.setColour (AlterTheme::iceBlue.withAlpha (0.35f));

            // The crosshair arm has to follow the MODULE's axes, not the screen's.
            // The line means "everything along here reads the same value", and at a
            // quarter turn the module's value axis runs across the screen — a
            // horizontal line there would join points of different values, which is
            // the opposite of what it is for.
            const int  rot = juce::jlimit (0, 3, (int) panel.getProperty (AlterState::kRotationAngle, 0));
            const bool quarterTurn = (rot == 1 || rot == 3);

            auto alongValueAxis = [&]
            {
                if (quarterTurn) g.drawVerticalLine   ((int) cursorPos.x, b.getY(), b.getBottom());
                else             g.drawHorizontalLine ((int) cursorPos.y, b.getX(), b.getRight());
            };
            auto acrossValueAxis = [&]
            {
                if (quarterTurn) g.drawHorizontalLine ((int) cursorPos.y, b.getX(), b.getRight());
                else             g.drawVerticalLine   ((int) cursorPos.x, b.getY(), b.getBottom());
            };

            alongValueAxis();
            if (vertical) acrossValueAxis();

            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            const float tw = (float) rd.length() * 7.0f + 10.0f, th = 17.0f;
            float bx = cursorPos.x + 10.0f, by = cursorPos.y - th - 6.0f;
            if (bx + tw > b.getRight()) bx = cursorPos.x - tw - 10.0f;
            if (by < b.getY())          by = cursorPos.y + 8.0f;
            g.setColour (juce::Colours::black.withAlpha (0.72f));
            g.fillRoundedRectangle (bx, by, tw, th, 3.0f);
            g.setColour (AlterTheme::iceBlue);
            g.drawText (rd, juce::Rectangle<float> (bx, by, tw, th), juce::Justification::centred, false);
        }
    }

    // Top-right metadata about the module (its name + the set parameters). Hidden
    // globally by the controller's "Hide info" checkbox.
    if (AlterTheme::hudInfoHidden.load()) return;

    g.setColour (juce::Colours::white.withAlpha (0.9f));
    g.setFont (juce::Font (juce::FontOptions (14.0f)));

    const auto b = getLocalBounds().reduced (6);

    // Draws one right-justified "| text" row at vertical offset `rowY`, returns the
    // measured text width so the badge can sit immediately to its left.
    auto drawInfoRow = [&g, b] (const juce::String& text, int rowY) -> float
    {
        if (text.isEmpty()) return 0.0f;
        const juce::String drawn = "| " + text;
        juce::GlyphArrangement ga;
        ga.addLineOfText (juce::Font (juce::FontOptions (14.0f)), drawn, 0.0f, 0.0f);
        const float w = ga.getBoundingBox (0, -1, true).getWidth();
        g.drawText (drawn, b.withTrimmedTop (rowY), juce::Justification::topRight, false);
        return w;
    };

    auto drawBadge = [&g, b] (float textW, int rowY, bool hasText)
    {
        if (auto* logo = qspModuleLogo())
        {
            const float lh = 12.0f, lw = lh * kQspLogoAspect;
            const float gap = hasText ? 6.0f : 0.0f;
            logo->drawWithin (g,
                              juce::Rectangle<float> ((float) b.getRight() - textW - gap - lw,
                                                      (float) b.getY() + 2.0f + (float) rowY, lw, lh),
                              juce::RectanglePlacement::centred, 0.9f);
        }
    };

    // ── FUSION: one row of its OWN settings, then one row PER LAYER stacked under
    //    it — "A · <that module's own info>". Everything a module writes for itself
    //    in the HUD it now also lends here, so a stack of three reads as four rows:
    //    the fusion, then A, B, C. ────────────────────────────────────────────────
    if (type == "fusion")
    {
        const int lineH = 18;
        int rowY = 0;

        // Row 0: the Fusion's own settings, with the brand badge (only here).
        const juce::String fusionLabel = buildModuleInfoLabel (type, panel, view.get());
        const float w0 = drawInfoRow (fusionLabel, rowY);
        drawBadge (w0, rowY, fusionLabel.isNotEmpty());
        rowY += lineH;

        // One row per layer. getFusionLayerComponents hands back the live components
        // COMPACTED (an empty slot A does not leave a gap — the shader only knows
        // consecutive units), so fusionLayerIds gives the panel ids in the SAME
        // compacted order and the two line up index for index. Row letters A/B/C
        // follow that same order, i.e. the actual base-up stack.
        juce::Component* layerComp[AlterState::kMaxFusionLayers] = {};
        if (auto* fv = dynamic_cast<FusionVisual*> (view.get()))
            fv->getFusionLayerComponents (layerComp, AlterState::kMaxFusionLayers);

        const int myId = (int) panel.getProperty (AlterState::kId, -1);
        auto panels = panel.getParent();
        const std::vector<int> ids = owner.settings.fusionLayerIds (myId);

        for (size_t k = 0; k < ids.size() && k < (size_t) AlterState::kMaxFusionLayers; ++k)
        {
            juce::ValueTree lp;
            if (panels.isValid())
                for (int i = 0; i < panels.getNumChildren(); ++i)
                    if ((int) panels.getChild (i).getProperty (AlterState::kId, -1) == ids[k])
                    { lp = panels.getChild (i); break; }
            if (! lp.isValid()) continue;

            const juce::String lt = lp.getProperty (AlterState::kType).toString();
            juce::String layerLabel = buildModuleInfoLabel (lt, lp, layerComp[k]);
            if (layerLabel.isEmpty()) layerLabel = lt;   // never leave a slot blank

            const juce::String rowText = juce::String::charToString ((juce::juce_wchar) ('A' + (int) k))
                                       + juce::String::fromUTF8 (" \xc2\xb7 ") + layerLabel;   // "A · …"
            drawInfoRow (rowText, rowY);
            rowY += lineH;
        }
        return;
    }

    // ── AUDIO METER (bar + Trend): the readout, STACKED, one item per line ──────
    //
    //  What / how much / in what — on three lines instead of one, because this is
    //  the module that is a 96 px column by default and a single line has nowhere to
    //  go in it. It replaces the strip the meter used to print along its own bottom
    //  edge, which at that width rendered as unreadable fragments.
    //
    //  MOMENTARY ONLY. The other two views are pictures over time rather than a
    //  single reading — Level history has no "the value right now" to put on the
    //  middle line at all, and Trend is read as a curve with its settings beside it,
    //  so both keep the ordinary one-row label like every other module.
    if (type == "rms" || type == "audiometer")
    {
        if (auto* am = dynamic_cast<VisualAudioMeter*> (view.get()))
        {
            if (am->getMeterMode() != VisualAudioMeter::MeterMode::LevelHistory
                && ! am->getTrendMode())
            {
                // Plain rows: no "| " lead-in here. That separator exists to hang
                // several facts off one line, and these are already on their own.
                auto drawPlainRow = [&g, b] (const juce::String& text, int rowY) -> float
                {
                    if (text.isEmpty()) return 0.0f;
                    juce::GlyphArrangement ga;
                    ga.addLineOfText (juce::Font (juce::FontOptions (14.0f)), text, 0.0f, 0.0f);
                    g.drawText (text, b.withTrimmedTop (rowY), juce::Justification::topRight, false);
                    return ga.getBoundingBox (0, -1, true).getWidth();
                };

                const int lineH = 18;
                int rowY = 0;

                const float w0 = drawPlainRow (am->getModeName(), rowY);
                drawBadge (w0, rowY, true);
                rowY += lineH;

                drawPlainRow (am->getValueText(), rowY);  rowY += lineH;
                drawPlainRow (am->getUnitText(),  rowY);
                return;
            }
        }
    }

    // ── Every other module: a single info row + badge. ──────────────────────────
    const juce::String label = buildModuleInfoLabel (type, panel, view.get());
    const float textW = drawInfoRow (label, 0);
    drawBadge (textW, 0, label.isNotEmpty());
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
    if (e.mods.isPopupMenu())   // right-click: Destroy / Hide
    {
        owner.showModuleContextMenu (id);
        return;
    }

    dragging = true;
    // Either kind of detach (single panel → PanelWindow, whole row → RowWindow)
    // means the host lives inside a floating window: forward drags to that window
    // so the WINDOW moves, instead of starting an in-HUD drag that would tear the
    // module out of its row.
    startedDetached = (bool) panel.getProperty (AlterState::kDetached, false)
                   || (bool) panel.getProperty (AlterState::kRowDetached, false);
    lastScreenPos = e.getScreenPosition();

    // If detached and in a PanelWindow/RowWindow, forward to parent for dragging
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
    const auto accent = AlterTheme::themeAccent();

    // Fill the strip; rounded on the LEFT (matches the HUD window edge), sharp on the
    // RIGHT (butts up against the modules).
    AlterTheme::paintBackground (g, b);

    const float corner = juce::jmin (7.0f, b.getWidth() * 0.5f);   // moderate left rounding
    juce::Path shape;
    shape.addRoundedRectangle (b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                               corner, corner,
                               true,  false,   // top-left rounded, top-right sharp
                               true,  false);  // bottom-left rounded, bottom-right sharp

    g.setGradientFill ({ accent.withAlpha (down ? 0.45f : over ? 0.30f : 0.16f),
                         b.getX(), b.getY(),
                         accent.withAlpha (0.04f), b.getX(), b.getBottom(), false });
    g.fillPath (shape);

    g.setColour (accent.withAlpha (over || down ? 0.95f : 0.5f));
    g.strokePath (shape, juce::PathStrokeType (1.0f));

    // vertical lettering — the brand mark on top acts as an extra "letter": "oALTER"
    const juce::String label = "ALTER";
    const int letters = label.length();
    const int n = letters + 1;                 // +1 = the logo cell at the very top
    const float pad = 10.0f;
    const float cellH = (b.getHeight() - 2.0f * pad) / juce::jmax (1, n);
    const float fontSize = juce::jmax (10.0f, juce::jmin (cellH * 0.85f, b.getWidth() - 8.0f));

    const juce::Colour ink = (over ? AlterTheme::textBright : AlterTheme::textNormal).withAlpha (0.95f);

    // Logo cell (top): (re)tint the cached drawable only when the colour actually changes.
    if (logo == nullptr || logoColour != ink)
    {
        logo = juce::Drawable::createFromImageData (kQspLogoSvg, std::strlen (kQspLogoSvg));
        if (logo != nullptr) logo->replaceColour (juce::Colours::black, ink);
        logoColour = ink;
    }
    if (logo != nullptr)
    {
        // Slightly narrower than the letters so the mark reads as its own glyph.
        const float logoW = b.getWidth() * 0.62f;
        juce::Rectangle<float> logoCell (b.getCentreX() - logoW * 0.5f, b.getY() + pad, logoW, cellH);
        logo->drawWithin (g, logoCell.reduced (0.0f, 2.0f),
                          juce::RectanglePlacement::centred
                        | juce::RectanglePlacement::onlyReduceInSize, 1.0f);
    }

    // Letters below the logo.
    g.setColour (ink);
    g.setFont (juce::Font (juce::FontOptions (fontSize)));
    for (int i = 0; i < letters; ++i)
    {
        juce::Rectangle<float> cell (b.getX(), b.getY() + pad + (i + 1) * cellH, b.getWidth(), cellH);
        g.drawFittedText (juce::String::charToString (label[i]), cell.toNearestInt(),
                          juce::Justification::centred, 1);
    }
}

// =======================================================
// WindowMoveHandle: drag to move the whole HUD window
// =======================================================
MainComponent::WindowMoveHandle::WindowMoveHandle()
{
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void MainComponent::WindowMoveHandle::mouseDown (const juce::MouseEvent& e)
{
    if (auto* top = getTopLevelComponent())
        dragger.startDraggingComponent (top, e);
}

void MainComponent::WindowMoveHandle::mouseDrag (const juce::MouseEvent& e)
{
    if (auto* top = getTopLevelComponent())
        dragger.dragComponent (top, e, nullptr);
}

void MainComponent::WindowMoveHandle::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    AlterTheme::paintBackground (g, b);

    // subtle vertical grip dots → hint that this strip moves the window
    const float cx = b.getCentreX(), cy = b.getCentreY();
    g.setColour (AlterTheme::textNormal.withAlpha (0.55f));
    for (int i = -3; i <= 3; ++i)
        g.fillEllipse (cx - 1.5f, cy + (float) i * 7.0f - 1.5f, 3.0f, 3.0f);
}

// =======================================================
// MainComponent
// =======================================================
MainComponent::MainComponent (AlterState& s)
    : settings (s)
{
    setWantsKeyboardFocus (true);   // Ctrl+Z / Ctrl+Shift+Z (undo/redo) in the HUD

    // restore saved plugin instance selection (0 = auto)
    udpSource.setSelectedInstance ((juce::uint32) (juce::int64)
        settings.getTree().getProperty (AlterState::kPluginInstance, 0));

    // restore saved theme (incl. custom colours)
    AlterTheme::customPrimary   = juce::Colour ((uint32_t)(int) settings.getTree()
                                      .getProperty (AlterState::kThemeColor1, (int) 0xFF3D96E7));
    AlterTheme::customSecondary = juce::Colour ((uint32_t)(int) settings.getTree()
                                      .getProperty (AlterState::kThemeColor2, (int) 0xFF6902D6));
    AlterTheme::setTheme ((int) settings.getTree().getProperty (AlterState::kTheme, 0));

    // Attach the shared GPU context BEFORE building modules, so each module (and
    // any Synesthesia) finds this window's GL host the moment it is added.
    glHost.attach (*this);

    rebuildSlotsFromState();

    leftCtrl   = std::make_unique<VerticalControllerButton>();
    moveHandle = std::make_unique<WindowMoveHandle>();        // right edge: drag to move the HUD
    addAndMakeVisible (*leftCtrl);
    addAndMakeVisible (*moveHandle);

    leftCtrl->onClick = [this]{ if (showController) showController(); };

    // Plugin control channel: a Control-mode plugin sends param values here.
    // The callback runs on the UDP socket thread.
    //
    // COALESCED, not marshalled one-by-one. A callAsync per packet meant one
    // message-thread wake-up, one ValueTree write and one full registry rebuild
    // for EVERY value the DAW produced — and a host emits those per audio block,
    // per moving automation lane. Collapsing on (module, parameter) and applying
    // the survivors once per timer tick costs the same picture and a fraction of
    // the message thread.
    udpSource.onControlValue = [this] (juce::uint32 moduleId, juce::uint8 paramId, float value)
    {
        const juce::ScopedLock sl (ctrlQueueLock);
        ctrlQueue[((juce::uint64) moduleId << 8) | (juce::uint64) paramId] = value;
    };

    settings.getTree().addListener (this);
    pushControlRegistry();
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
    // The background is drawn on the GPU by the AlterGLHost (and the Synesthesia
    // fractals sit in the GL layer behind this component). Painting a background
    // here would cover them, so we deliberately paint nothing WHEN modules exist.
    //
    // When the HUD is empty, however, there are no fractals to hide — so we show
    // the brand mark as a centred watermark (white on dark themes, black on light).
    if (! hudEmpty)
        return;

    if (emptyLogo == nullptr)
        refreshEmptyLogo();

    if (emptyLogo != nullptr)
    {
        auto area = getLocalBounds().toFloat();
        const float h = area.getHeight() * 0.55f;                 // watermark height
        const float w = h * (428.35f / 254.86f);                  // logo aspect
        juce::Rectangle<float> box (0, 0, w, h);
        box.setCentre (area.getCentre());
        emptyLogo->drawWithin (g, box,
                               juce::RectanglePlacement::centred
                             | juce::RectanglePlacement::onlyReduceInSize, 0.28f);
    }
}

void MainComponent::refreshEmptyLogo()
{
    // Re-parse from the (black) source, then tint for the active theme.
    //
    // The mark is treated as TEXT, not as a control: it takes the theme's bright
    // text colour rather than the accent used by buttons/chrome. In the Custom
    // theme that colour is derived from the user's colour pair (accent hue,
    // brightness picked for legibility against the chosen background), so the
    // watermark now follows the custom palette instead of being a fixed
    // white-on-dark / black-on-light mark.
    emptyLogo = juce::Drawable::createFromImageData (kQspLogoSvg, std::strlen (kQspLogoSvg));
    if (emptyLogo == nullptr) return;

    emptyLogoColour = AlterTheme::textBright;

    // Safety net: if the theme's text colour is (nearly) the background itself the
    // watermark would vanish — fall back to a plain contrasting tone.
    if (std::abs (emptyLogoColour.getPerceivedBrightness()
                  - AlterTheme::bgDeep.getPerceivedBrightness()) < 0.12f)
        emptyLogoColour = AlterTheme::bgDeep.contrasting (0.85f);

    emptyLogo->replaceColour (juce::Colours::black, emptyLogoColour);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    const int leftW   = leftCtrl   ? leftCtrl->idealWidth()   : 28;
    const int moveW   = moveHandle ? moveHandle->idealWidth() : 16;   // whole-window move grip
    const int rowGripW = RowDetachHandle::idealWidth();               // per-row tear-off grips
    const int rightW  = moveW + rowGripW;

    auto left  = area.removeFromLeft  (leftW);
    auto right = area.removeFromRight (rightW);
    if (leftCtrl) leftCtrl->setBounds (left);

    // Right strip is split: LEFT sub-column = per-row tear-off grips, RIGHT
    // sub-column = one shared grip that moves the whole HUD window.
    rowHandleColumn = right.removeFromLeft (rowGripW);
    if (moveHandle) moveHandle->setBounds (right);

    layoutActiveViews (area);
    layoutRowHandles();

    // AFTER the rows: a Fusion's working resolution follows its own size, so the
    // modules it borrows can only be sized once that size is final for this pass.
    //
    // ── WHY THIS IS DEFERRED WHILE THE WINDOW IS BEING DRAGGED ───────────────
    //
    // This is the HUD-resize freeze, and it is not one slow function — it is a
    // chain that runs at the rate Windows sends WM_SIZE, which during a drag is
    // well over a hundred times a second:
    //
    //   resized() -> layoutFusionLayers() -> setLayerSources()
    //             -> setLayerMode() -> setForceOffscreen()
    //             -> updateHostRegistration() -> addSource / removeSource
    //
    // and add/removeSource BLOCK on srcLock, which the GL thread holds for its
    // whole frame. Meanwhile every layer's setBounds changes by a pixel, so on
    // the GL side layerFB() releases and re-initialises a framebuffer per layer
    // per frame (a synchronous driver allocation), while on the worker side each
    // module throws away its back buffer and a Spectrum rebuilds its entire
    // static cache. So the GL frame gets far longer at exactly the moment the
    // message thread starts waiting on it — and the message thread is the one
    // that has to answer WM_SIZE. That is the hang, and it is why it got worse
    // with more modules and worse again with a Fusion.
    //
    // A SIZE change is deferred until the drag settles; anything structural
    // (a module joining a fusion, a reorder, a rotation) still runs at once,
    // because those are one-shot and the user is waiting to see the result.
    const auto nowSize     = getLocalBounds();
    const bool sizeChanged = (nowSize != lastLaidOutSize);
    lastLaidOutSize = nowSize;

    if (sizeChanged || deferFusionLayout)
    {
        fusionLayoutDirty = true;
        lastSizeChangeMs  = juce::Time::getMillisecondCounter();
        return;
    }

    fusionLayoutDirty = false;
    layoutFusionLayers();
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
                hudStackTargetId = -1;
                hudDragGhost.reset();
            }
        }
        catch (...)
        {
            // If anything goes wrong, safely cleanup drag state
            hudDragPanelId = -1;
            hudDragInsertIndex = -1;
            hudStackTargetId = -1;
            hudDragGhost.reset();
            DBG ("Exception in timerCallback during drag update");
        }
    }

    // Modules now each drive their own repaint via their own timer, so we no
    // longer force-repaint every module here (that was double work and the main
    // cause of stutter, especially with a large HUD / software renderer).

    // The deferred half of resized(). 120 ms after the LAST size change — i.e.
    // once the drag has stopped, or the window manager has finished its
    // animation — the layers are re-laid out exactly once, instead of once per
    // WM_SIZE. Long enough that no plausible drag rate slips through, short
    // enough to read as immediate when the mouse is released.
    if (fusionLayoutDirty
        && juce::Time::getMillisecondCounter() - lastSizeChangeMs >= 120)
    {
        fusionLayoutDirty = false;
        layoutFusionLayers();
    }

    // Plugin control channel: apply whatever automation arrived since the last
    // tick, then refresh the plugin-facing snapshot if anything changed.
    drainControlQueue();
    pushControlRegistryIfDirty();
}

// Apply the collapsed automation values. Runs at the timer rate (60 Hz), so a
// parameter that moved twenty times since the last tick costs ONE ValueTree
// write, not twenty.
void MainComponent::drainControlQueue()
{
    std::vector<std::pair<juce::uint64, float>> batch;
    {
        const juce::ScopedLock sl (ctrlQueueLock);
        if (ctrlQueue.empty())
            return;
        batch.assign (ctrlQueue.begin(), ctrlQueue.end());
        ctrlQueue.clear();
    }

    for (const auto& kv : batch)
        applyControlValue ((juce::uint32) (kv.first >> 8),
                           (juce::uint8)  (kv.first & 0xFF),
                           kv.second);
}

// Rebuild the plugin-facing registry at most every 50 ms.
//
// pushControlRegistry() walks every panel and builds a fresh descriptor — a
// String, a vector of ~25 parameters, and a ValueTree property read for each of
// them. Calling it from valueTreePropertyChanged meant paying all of that for a
// single automated float. The registry only carries VALUES the plugin already
// knows it sent (plus names and colours, which the user changes by hand), so a
// 50 ms lag is invisible; what it buys is that a moving automation lane no
// longer rebuilds the whole thing per block.
void MainComponent::pushControlRegistryIfDirty()
{
    if (! registryDirty)
        return;

    const auto now = juce::Time::getMillisecondCounter();
    if (lastRegistryPushMs != 0 && now - lastRegistryPushMs < 50)
        return;

    lastRegistryPushMs = now;
    registryDirty = false;
    pushControlRegistry();
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

// set AlwaysOnTop on ALL detached windows (regardless of selection),
// including torn-off block (row) windows
void MainComponent::setAlwaysOnTopForAllDetachedPanels (bool on)
{
    for (auto& kv : windows)
    {
        auto& win = kv.second;
        if (win)
            win->setAlwaysOnTop (on);
    }
    for (auto& kv : rowWindows)
    {
        auto& win = kv.second;
        if (win)
            win->setAlwaysOnTop (on);
    }
}

void MainComponent::showModuleContextMenu (int panelId)
{
    auto panel = settings.getPanelById (panelId);
    if (! panel.isValid()) return;

    const bool hidden = (bool) panel.getProperty (AlterState::kHidden, false);

    juce::PopupMenu m;
    m.addItem (1, "Destroy");
    m.addItem (2, hidden ? "Show" : "Hide");
    m.addItem (3, "Duplicate");
    m.showMenuAsync (juce::PopupMenu::Options(), [this, panelId] (int r)
    {
        auto p = settings.getPanelById (panelId);
        if (! p.isValid()) return;
        if (r == 1)
            settings.removePanelById (panelId);
        else if (r == 2)
            p.setProperty (AlterState::kHidden,
                           ! (bool) p.getProperty (AlterState::kHidden, false), nullptr);
        else if (r == 3)
            settings.duplicatePanel (panelId);   // copy with same settings, new id, next to it
    });
}

void MainComponent::layoutActiveViews (juce::Rectangle<int> area)
{
    // The stack handles are pooled: this pass takes as many as it needs from the
    // front and whatever is left over gets parked, on EVERY exit path (there are
    // several early returns below).
    stackDividerCursor = 0;
    struct ParkSurplus
    {
        MainComponent& mc;
        ~ParkSurplus() { mc.hideUnusedStackDividers(); }
    } parkSurplus { *this };

    std::vector<PanelHost*> hudHosts;
    hudHosts.reserve (slots.size());

    for (auto& s : slots)
    {
        if (!s.host) continue;

        const bool detached    = (bool) s.node.getProperty (AlterState::kDetached, false);
        const bool rowDetached = (bool) s.node.getProperty (AlterState::kRowDetached, false);
        const bool hidden      = (bool) s.node.getProperty (AlterState::kHidden,   false);
        if (detached || rowDetached || s.host->getParentComponent() != this) continue;

        if (hidden) { s.host->setVisible (false); continue; }   // kept in controller, gone from HUD

        // Lent to a Fusion: it keeps rendering, but somewhere else. layoutFusionLayers
        // places it; it must not claim a slot in the row.
        if ((int) s.node.getProperty (AlterState::kFusionHost, 0) > 0) continue;

        s.host->setVisible (true);
        hudHosts.push_back (s.host.get());
    }

    const int gap = 0;

    // Track empty state so paint() can show/hide the brand watermark.
    const bool nowEmpty = hudHosts.empty();
    if (nowEmpty != hudEmpty)
    {
        hudEmpty = nowEmpty;
        repaint();
    }

    // if no hosts, nothing to layout
    if (hudHosts.empty())
    {
        // HUD is empty, but a block may be hovering for reattach → fill preview
        if (rowDropActive && dragPlaceholder)
            dragPlaceholder->setBounds (area);
        return;
    }

    const int layers = settings.hudLayers();


    // MOBILE-STYLE DRAG & DROP preview (single-block layout only).
    //
    // Skipped while the ghost is hovering over a module to be STACKED on it: that
    // gesture must not reshuffle the row, it only opens a slot inside one column,
    // so the normal layout runs and the placeholder is drawn over the target's
    // upper/lower half further down.
    if (hudDragPanelId >= 0 && layers == 1 && hudStackTargetId < 0)
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

    // ── Normal layout: one horizontal block per NON-EMPTY layer ─────────────
    // Empty layers (e.g. a block that has been torn off into a RowWindow) take
    // no space, so the remaining block(s) grow to fill the whole HUD.
    std::array<std::vector<PanelHost*>, 3> rows;
    for (auto* h : hudHosts)
    {
        auto* slot = findSlotById (h->getId());
        const int layer = slot ? juce::jlimit (0, layers - 1,
                                               (int) slot->node.getProperty (AlterState::kLayer, 0))
                               : 0;
        rows[(size_t) layer].push_back (h);
    }

    // Dividers exist between horizontal neighbours within each block, plus a grip
    // on each OUTER edge of the row: {-1, first} is the leading edge, {last, -1} the
    // trailing one. Both are shown only when the row has unused space — i.e. when
    // every panel in it is fixed-width — and they are what let a lone meter be
    // widened by hand.
    //
    // Both edges, because a row of nothing but fixed panels is CENTRED: it has empty
    // space on the left exactly as much as on the right, and offering a handle on
    // only one of them means half the module's outline is decoration. Dragging
    // either one widens the module; they are the same gesture from opposite sides.
    //
    // The neighbours here are COLUMNS, not panels: a column is represented by the
    // module at its top, and a width written through that id is applied to the
    // whole stack underneath it.
    std::vector<std::pair<int,int>> pairs;
    for (int L = 0; L < layers; ++L)
    {
        auto& r = rows[(size_t) L];
        if (r.empty()) continue;

        const auto cols = buildColumns (r);
        // ORDER MATTERS: layoutRow walks this slice in exactly this sequence —
        // leading edge, then the inner seams, then the trailing edge.
        if (! cols.empty())
            pairs.push_back ({ -1, cols.front().hosts.front()->getId() });
        for (size_t i = 0; i + 1 < cols.size(); ++i)
            pairs.push_back ({ cols[i].hosts.front()->getId(), cols[i + 1].hosts.front()->getId() });
        if (! cols.empty())
            pairs.push_back ({ cols.back().hosts.front()->getId(), -1 });
    }

    if (pairs != dividerPairs)
        rebuildResizeDividers (pairs);

    // visible (non-empty) layers, in vertical order
    std::vector<int> vis;
    for (int L = 0; L < layers; ++L)
        if (! rows[(size_t) L].empty())
            vis.push_back (L);

    if (vis.empty()) return;

    const auto wts = settings.blockWeights();

    // A drop preview inserts a "gap" slot at rowDropSlot among the visible rows.
    const bool  preview = rowDropActive;
    const int   gapPos  = preview ? juce::jlimit (0, (int) vis.size(), rowDropSlot) : -1;

    float visTotal = 0.0f;
    for (int L : vis) visTotal += wts[(size_t) L];
    if (visTotal <= 0.001f) visTotal = (float) vis.size();

    float gapWeight = visTotal / (float) juce::jmax (1, (int) vis.size());
    if (gapWeight <= 0.001f) gapWeight = 1.0f;

    const float total = visTotal + (gapPos >= 0 ? gapWeight : 0.0f);

    // build the vertical order of slots (-1 = the preview gap)
    std::vector<int> order;
    order.reserve (vis.size() + 1);
    for (int i = 0; i <= (int) vis.size(); ++i)
    {
        if (i == gapPos) order.push_back (-1);
        if (i < (int) vis.size()) order.push_back (vis[(size_t) i]);
    }

    // block-height dividers: only between adjacent visible rows, and only when
    // no drop preview is active (they'd fight the transient gap otherwise)
    const int wantDiv = (preview ? 0 : juce::jmax (0, (int) vis.size() - 1));
    if ((int) blockDividers.size() != wantDiv)
    {
        blockDividers.clear();
        for (int i = 0; i < wantDiv; ++i)
        {
            auto d = std::make_unique<BlockDivider> (*this, 0, 1);
            addAndMakeVisible (*d);
            d->setAlwaysOnTop (true);
            blockDividers.push_back (std::move (d));
        }
    }
    for (auto& d : blockDividers) d->setVisible (! preview);

    int dividerIdx = 0;   // consumed by layoutRow for the per-module resize dividers
    int y = area.getY();
    int prevVisLayer = -1;
    int divPlaced = 0;
    for (size_t s = 0; s < order.size(); ++s)
    {
        const bool  last = (s + 1 == order.size());
        const int   Lentry = order[s];
        const float w = (Lentry < 0) ? gapWeight : wts[(size_t) Lentry];
        const int   hgt = last ? (area.getBottom() - y)
                               : juce::jmax (20, (int) std::round (area.getHeight() * w / total));
        const juce::Rectangle<int> band (area.getX(), y, area.getWidth(), hgt);

        if (Lentry < 0)
        {
            if (dragPlaceholder) dragPlaceholder->setBounds (band);
        }
        else
        {
            layoutRow (rows[(size_t) Lentry], band, dividerIdx);

            // horizontal divider between this visible row and the previous one
            if (! preview && prevVisLayer >= 0 && divPlaced < (int) blockDividers.size())
            {
                blockDividers[(size_t) divPlaced]->setLayers (prevVisLayer, Lentry);
                blockDividers[(size_t) divPlaced]->setBounds (area.getX(), y - 3, area.getWidth(), 6);
                ++divPlaced;
            }
            prevVisLayer = Lentry;
        }

        y += hgt;
    }

    // Stack drop preview: the ghost is over a module and will be inserted above or
    // below it inside that column. Show the slot as a horizontal band across the
    // half of the target the pointer is in, so the gesture reads as "this goes
    // here", not "this replaces that".
    if (hudStackTargetId >= 0 && dragPlaceholder != nullptr)
    {
        if (auto* target = findHostById (hudStackTargetId))
        {
            auto b = target->getBounds();
            const int bandH = juce::jmax (18, b.getHeight() / 3);
            dragPlaceholder->setBounds (hudStackBelow ? b.removeFromBottom (bandH)
                                                      : b.removeFromTop    (bandH));
            dragPlaceholder->setAlwaysOnTop (true);
        }
    }
}

void MainComponent::layoutFusionLayers()
{
    for (auto& s : slots)
    {
        if (! s.host) continue;
        if (s.node.getProperty (AlterState::kType).toString() != "fusion") continue;

        auto* fusion = dynamic_cast<FusionVisual*> (s.host->getInner());
        if (fusion == nullptr) continue;

        // The layers render at the Fusion's own size, CAPPED.
        //
        // A layer is not the picture. It is a texture the fusion folds, scales,
        // warps and composites, sampled bilinearly into the fusion's rectangle —
        // so past a point its resolution buys almost nothing while costing
        // everything, because it costs AREA. Uncapped, three layers in a
        // fullscreen HUD meant three full-screen module renders (Geometry's whole
        // bloom pyramid among them) sixty times a second, on top of the fusion's
        // own full-resolution pass. That is where enlarging the window started to
        // stutter.
        //
        // A CAP and not a fraction: below it nothing changes at all. Above it the
        // layers stop growing — which also stops the framebuffer reallocations and
        // static-cache rebuilds that every size change triggers, so dragging the
        // window bigger past the cap costs nothing at all.
        //
        // The 2D modules have always done this for themselves. This is the same
        // budget extended to the three that draw on the GPU, which had none.
        const long long layerBudget = AlterState::fusionLayerPixelBudget (
            (int) s.node.getProperty (AlterState::kFusionDetail, 1));

        // The INNER component's size, not the host's: when the Fusion panel is
        // itself rotated 90 or 270 degrees the inner is laid out with its width and
        // height exchanged, and that is the space the fusion works in.
        const auto ls = fusion->getLocalBounds();

        // SLOT-INDEXED, with a null for an empty slot. The Fusion keeps its
        // settings per slot, so slot B's module must land on slot B's settings even
        // when slot A is empty; the compaction down to consecutive texture units
        // happens later, in fusionGlPrepare, where the settings travel with it.
        std::vector<juce::Component*> views ((size_t) AlterState::kMaxFusionLayers, nullptr);

        for (int slot = 0; slot < AlterState::kMaxFusionLayers; ++slot)
        {
            const int layerId = settings.fusionLayerIdInSlot (s.id, slot);
            if (layerId <= 0) continue;

            auto* ls2 = findSlotById (layerId);
            if (! ls2 || ! ls2->host || ! ls2->host->getInner()) continue;

            // A module that is still floating in its own window has not finished
            // coming home yet (setFusionLayer clears the flags, the listener does
            // the move asynchronously). Skip it for this pass rather than steal it
            // from the window mid-flight.
            if ((bool) ls2->node.getProperty (AlterState::kDetached,    false)) continue;
            if ((bool) ls2->node.getProperty (AlterState::kRowDetached, false)) continue;

            auto* host = ls2->host.get();

            // Parked OUTSIDE the HUD's own bounds. JUCE clips children to their
            // parent, so nothing of it is ever drawn, while everything that makes
            // it produce frames — its worker, its size, isShowing() for the GL
            // modules — carries on exactly as before. Moving it out of the
            // hierarchy instead would stop all of that.
            if (host->getParentComponent() != this)
                addAndMakeVisible (*host);

            // A quarter turn swaps what "width" and "height" mean for this layer,
            // so it is laid out ALREADY EXCHANGED and the shader then relabels the
            // axes — the same two-step PanelHost performs for an ordinary rotated
            // panel, and the reason a rotated spectrum keeps its proportions
            // instead of being squashed into the module's aspect.
            //
            // The rotation is the MODULE'S OWN, read off the layer's panel. It is a
            // property of the module and it is edited where every other module
            // property is edited — a fusion does not get to hold a second, private
            // opinion about which way up a Spectrum is.
            const int turn = juce::jlimit (0, 3, (int) ls2->node.getProperty (
                AlterState::kRotationAngle, 0));
            const bool swap = (turn == 1 || turn == 3);

            host->setVisible (true);
            if (! ls.isEmpty())
            {
                int lwPx = swap ? ls.getHeight() : ls.getWidth();
                int lhPx = swap ? ls.getWidth()  : ls.getHeight();

                // Scale BOTH sides by the same factor. The fusion reads a layer
                // across its own 0..1, so a layer whose aspect stopped matching
                // would not be smaller, it would be stretched.
                if (const long long a = (long long) lwPx * (long long) lhPx;
                    layerBudget > 0 && a > layerBudget)
                {
                    const double k = std::sqrt ((double) layerBudget / (double) a);
                    lwPx = juce::jmax (2, (int) std::lround ((double) lwPx * k));
                    lhPx = juce::jmax (2, (int) std::lround ((double) lhPx * k));
                }

                // Parked on its OWN width, not the Fusion's: a swapped layer can be
                // wider than the module, and parking it at the module's width would
                // leave its right-hand edge poking back into the HUD.
                const juce::Rectangle<int> parked (-lwPx - 16, 0, lwPx, lhPx);

                // ONLY when it actually changed. This runs from resized(), which
                // fires continuously while a window edge is being dragged, and a
                // layer's setBounds is not cheap downstream: the worker throws away
                // its back buffer and a Spectrum rebuilds its whole static cache —
                // gradient, grid and some twenty text labels. Re-issuing identical
                // bounds a hundred times a second buys nothing and costs all of that.
                if (host->getBounds() != parked)
                    host->setBounds (parked);
            }

            views[(size_t) slot] = ls2->host->getInner();
        }

        fusion->setLayerSources (views);
    }
}

void MainComponent::beginBlockResize (int screenY)
{
    blockDragStartY = screenY;
    blockDragStartWeights = settings.blockWeights();
}

void MainComponent::dragBlockResize (int upperLayer, int lowerLayer, int screenY)
{
    const int layers = settings.hudLayers();
    if (upperLayer < 0 || lowerLayer < 0 || upperLayer >= layers || lowerLayer >= layers)
        return;

    auto w = blockDragStartWeights;

    // The visible height maps to the sum of the VISIBLE layer weights only, since
    // empty (torn-off) layers don't occupy any space.
    float total = 0.0f;
    for (int L : visibleLayersInHud()) total += w[(size_t) L];
    if (total <= 0.001f) return;

    const float pairW   = w[(size_t) upperLayer] + w[(size_t) lowerLayer];
    const float hudH    = (float) juce::jmax (1, getHeight());
    const float pairPix = pairW / total * hudH;
    if (pairPix < 20.0f) return;

    const float frac = (float) (screenY - blockDragStartY) / pairPix;
    const float newUpper = juce::jlimit (0.12f * pairW, 0.88f * pairW,
                                         w[(size_t) upperLayer] + frac * pairW);
    w[(size_t) upperLayer] = newUpper;
    w[(size_t) lowerLayer] = pairW - newUpper;

    settings.setBlockWeights (w);   // → property listener re-lays out
}

// Groups a block's hosts into COLUMNS.
//
// The grouping is read straight out of the panel order: a host whose stackRow is
// 0 opens a new column, one with a higher stackRow joins the column that is
// currently open. A column's width is the widest thing in it — it is flexible as
// soon as any member is flexible, because a flexible module inside a fixed column
// would have nothing to size itself against.
std::vector<MainComponent::Column>
MainComponent::buildColumns (const std::vector<PanelHost*>& rowHosts)
{
    std::vector<Column> cols;

    for (auto* h : rowHosts)
    {
        auto* slot = findSlotById (h->getId());
        const int   stackRow = slot ? (int) slot->node.getProperty (AlterState::kStackRow, 0) : 0;
        const int   prefW    = slot ? (int) slot->node.getProperty (AlterState::kPreferredWidth, 0) : 0;
        const float ratio    = slot ? (float) slot->node.getProperty (AlterState::kWidthRatio, 1.0f) : 1.0f;

        // Joining requires the rows to line up exactly: stackRow 1 belongs under a
        // column that currently holds one module. If the head of the run is hidden
        // (or torn off into its own window) the numbering no longer matches and the
        // orphan correctly becomes a column of its own instead of attaching itself
        // to whatever happened to come before it.
        const bool joins = stackRow > 0
                        && ! cols.empty()
                        && stackRow == (int) cols.back().hosts.size()
                        && (int) cols.back().hosts.size() < AlterState::kMaxStack;

        if (! joins)
            cols.push_back ({});

        auto& c = cols.back();
        c.hosts.push_back (h);

        // Widest wins, and a single flexible member makes the whole column
        // flexible (a fixed column would give it nothing to size against).
        if (prefW <= 0)
            c.anyFlexible = true;
        else
            c.fixedW = juce::jmax (c.fixedW, juce::jmax (30, prefW));

        c.ratio = juce::jmax (c.ratio, ratio);
    }

    for (auto& c : cols)
    {
        if (c.anyFlexible)
            c.fixedW = 0;
        if (c.ratio <= 0.0f)
            c.ratio = 1.0f;
    }

    return cols;
}

// Splits one column's rectangle between the modules stacked in it, and puts a
// drag handle on each seam.
void MainComponent::layoutColumn (const Column& col, juce::Rectangle<int> area)
{
    const int m = (int) col.hosts.size();
    if (m == 0) return;

    if (m == 1)
    {
        col.hosts[0]->setBounds (area);
        return;
    }

    float total = 0.0f;
    std::vector<float> w ((size_t) m, 1.0f);
    for (int i = 0; i < m; ++i)
    {
        auto* slot = findSlotById (col.hosts[(size_t) i]->getId());
        w[(size_t) i] = slot ? juce::jmax (0.05f, (float) slot->node.getProperty (AlterState::kStackWeight, 1.0f))
                             : 1.0f;
        total += w[(size_t) i];
    }
    if (total <= 0.001f) total = (float) m;

    const int handleH = 4;
    int y = area.getY();

    for (int i = 0; i < m; ++i)
    {
        const bool last = (i == m - 1);
        // The last height is a remainder, and the 24 px floor above is applied
        // m - 1 times — so in a short row it can already have claimed the whole
        // column and this goes negative. A negative size reaches setBounds and
        // then JUCE's coordsToRectangle, which asserts on it in a Debug build.
        const int  h = last ? juce::jmax (1, area.getBottom() - y)
                            : juce::jmax (24, (int) std::round (area.getHeight() * w[(size_t) i] / total));

        col.hosts[(size_t) i]->setBounds (area.getX(), y, area.getWidth(), h);

        if (! last)
        {
            auto& d = acquireStackDivider();
            d.setPanels (col.hosts[(size_t) i]->getId(), col.hosts[(size_t) (i + 1)]->getId());
            d.setBounds (area.getX(), y + h - handleH / 2, area.getWidth(), handleH);
            d.setVisible (true);
        }

        y += h;
    }
}

MainComponent::StackDivider& MainComponent::acquireStackDivider()
{
    if (stackDividerCursor >= stackDividers.size())
    {
        auto d = std::make_unique<StackDivider> (*this, -1, -1);
        addAndMakeVisible (*d);
        d->setAlwaysOnTop (true);
        stackDividers.push_back (std::move (d));
    }

    return *stackDividers[stackDividerCursor++];
}

void MainComponent::hideUnusedStackDividers()
{
    for (size_t i = stackDividerCursor; i < stackDividers.size(); ++i)
        stackDividers[i]->setVisible (false);
}

// Lays out one HUD block: fixed-width columns keep their pixels, flexible
// columns share the remainder by ratio; dividers overlay the inner edges.
void MainComponent::layoutRow (const std::vector<PanelHost*>& rowHosts,
                               juce::Rectangle<int> area, int& dividerIdx)
{
    if (rowHosts.empty()) return;

    const auto cols = buildColumns (rowHosts);
    const int  n = (int) cols.size();
    if (n == 0) return;

    const int dividerWidth = 4;
    const int availableWidth = area.getWidth();
    const int firstDivider   = dividerIdx;   // this row's slice of resizeDividers

    struct HostLayout { int fixedW; float ratio; };
    std::vector<HostLayout> layouts ((size_t) n);

    int fixedTotal = 0, flexCount = 0;
    float flexRatioSum = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        if (cols[(size_t) i].fixedW > 0)
        {
            layouts[(size_t) i] = { cols[(size_t) i].fixedW, 0.0f };
            fixedTotal += layouts[(size_t) i].fixedW;
        }
        else
        {
            layouts[(size_t) i] = { 0, cols[(size_t) i].ratio };
            flexRatioSum += cols[(size_t) i].ratio;
            ++flexCount;
        }
    }

    // Fixed-width panels are SHRUNK to fit, never grown.
    //
    // A fixed width is a deliberate choice (a meter is thin and tall on purpose;
    // the divider writes an exact pixel width when you drag it). Scaling those
    // panels UP to fill the row meant a single meter in an otherwise empty block
    // ballooned to the full HUD width, and adding a second one just split that
    // width between them — the "thin and tall" shape only survived until you
    // touched the layout. So the only case that rescales them now is genuine
    // overflow: they ask for more pixels than the row has.
    const int fixedBudget = availableWidth - 40 * flexCount;
    if (fixedTotal > 0 && fixedTotal > fixedBudget)
    {
        const float scale = (float) juce::jmax (1, fixedBudget) / (float) fixedTotal;
        fixedTotal = 0;
        for (auto& l : layouts)
            if (l.fixedW > 0)
            {
                l.fixedW = juce::jmax (30, (int) ((float) l.fixedW * scale));
                fixedTotal += l.fixedW;
            }
    }

    const int flexibleWidth = juce::jmax (0, availableWidth - fixedTotal);
    if (flexRatioSum <= 0.0f) flexRatioSum = 1.0f;

    // With no flexible panel to soak up the remainder the row would sit hard
    // against the left edge; centre the fixed group instead so a couple of
    // meters look placed rather than stranded.
    int x = area.getX();
    if (flexCount == 0 && fixedTotal < availableWidth)
        x += (availableWidth - fixedTotal) / 2;

    // Leading-edge grip, on the outside of the first column. It exists only where
    // there is room to its left — a row holding a flexible module is anchored hard
    // against the left edge and there is nothing there to grow into.
    if (dividerIdx < (int) resizeDividers.size())
    {
        auto& lead = resizeDividers[(size_t) dividerIdx++];
        const bool leftSlack = (flexCount == 0) && (x - area.getX()) > 8;
        lead->setVisible (leftSlack);
        if (leftSlack)
            lead->setBounds (x - dividerWidth / 2, area.getY(), dividerWidth, area.getHeight());
    }

    for (int i = 0; i < n; ++i)
    {
        const int w = layouts[(size_t) i].fixedW > 0
                    ? layouts[(size_t) i].fixedW
                    : (int) ((layouts[(size_t) i].ratio / flexRatioSum) * (float) flexibleWidth);

        layoutColumn (cols[(size_t) i], { x, area.getY(), w, area.getHeight() });

        if (i < n - 1 && dividerIdx < (int) resizeDividers.size())
            resizeDividers[(size_t) dividerIdx++]->setBounds (x + w - dividerWidth / 2, area.getY(),
                                                              dividerWidth, area.getHeight());
        x += w;
    }

    // Trailing-edge grip (always allocated for this row by layoutActiveViews):
    // visible only while the row leaves space unused, otherwise it would sit on
    // top of the HUD's right-hand strip.
    if (dividerIdx < (int) resizeDividers.size())
    {
        auto& edge = resizeDividers[(size_t) dividerIdx++];
        // A few pixels of rounding slack in a flexible row is not free space, so
        // require a real gap before the grip shows up.
        const bool hasSlack = (area.getRight() - x) > 8;
        edge->setVisible (hasSlack);
        if (hasSlack)
            edge->setBounds (x - dividerWidth / 2, area.getY(), dividerWidth, area.getHeight());
    }

    // Every divider in this row is told how much of the row is unused, so a drag can
    // grow a module into it instead of only trading pixels with its neighbour. A row
    // with a flexible module has no free space by definition — it already absorbed
    // the remainder — so this is 0 there and the dividers keep their 1:1 trade.
    const int rowFree = (flexCount == 0) ? juce::jmax (0, availableWidth - fixedTotal) : 0;
    for (int d = firstDivider; d < dividerIdx && d < (int) resizeDividers.size(); ++d)
    {
        resizeDividers[(size_t) d]->setRowFreeSpace (rowFree);
        resizeDividers[(size_t) d]->setCentredRow (flexCount == 0);
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
    // the dragged panel's (possibly just assigned) layer
    auto draggedNode = settings.getPanelById (panelId);
    const int targetLayer = draggedNode.isValid()
                          ? (int) draggedNode.getProperty (AlterState::kLayer, 0) : 0;

    // compute HUD hosts of the SAME layer in current order
    std::vector<PanelHost*> hudHosts;
    hudHosts.reserve (slots.size());
    for (auto& s : slots)
    {
        if (!s.host || s.id == panelId) continue;
        const bool detached = (bool) s.node.getProperty (AlterState::kDetached, false);
        const bool hidden   = (bool) s.node.getProperty (AlterState::kHidden,   false);
        const int  layer    = (int) s.node.getProperty (AlterState::kLayer, 0);
        if (!detached && !hidden && layer == targetLayer && s.host->getParentComponent() == this)
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

    // Never land INSIDE another column's run — that would cut a stack in half.
    // Slide back to the head of whatever column the target index falls in.
    while (targetIndex > 0
           && (int) panels.getChild (targetIndex).getProperty (AlterState::kStackRow, 0) > 0)
        --targetIndex;

    if (targetIndex == currentIndex) return; // no change

    // move child in ValueTree (this will trigger child order changed listener which rebuilds HUD)
    panels.moveChild (currentIndex, targetIndex, nullptr);
    settings.normaliseStacks();
}

// Drops `panelId` into the column that `targetId` belongs to, directly above or
// below it. The stack is a run in the panel order, so this is a move of the child
// to the right index plus a renumbering of the run's stackRow values.
void MainComponent::stackPanelOnto (int panelId, int targetId, bool below)
{
    auto panels = settings.getPanelsRoot();
    if (! panels.isValid() || panelId == targetId) return;

    auto moved  = settings.getPanelById (panelId);
    auto target = settings.getPanelById (targetId);
    if (! moved.isValid() || ! target.isValid()) return;

    const auto column = panelIdsInColumnOf (targetId);
    if ((int) column.size() >= AlterState::kMaxStack) return;

    // The module joins the target's block.
    moved.setProperty (AlterState::kLayer, (int) target.getProperty (AlterState::kLayer, 0), nullptr);

    auto indexOf = [&] (int id)
    {
        for (int i = 0; i < panels.getNumChildren(); ++i)
            if ((int) panels.getChild (i).getProperty (AlterState::kId, -1) == id) return i;
        return -1;
    };

    const int from = indexOf (panelId);
    int       to   = indexOf (targetId);
    if (from < 0 || to < 0) return;

    if (below) ++to;                 // insert after the target
    if (from < to) --to;             // removing the module first shifts the tail left

    if (from != to)
        panels.moveChild (from, juce::jlimit (0, panels.getNumChildren() - 1, to), nullptr);

    // The run now spans the old column plus the newcomer, and starts at whichever
    // of them sits first (dropping ABOVE the old head makes the newcomer the head).
    const int oldHeadIdx = indexOf (column.front());
    const int movedIdx   = indexOf (panelId);
    if (oldHeadIdx < 0 || movedIdx < 0) return;

    const int headIdx = juce::jmin (oldHeadIdx, movedIdx);
    const int runLen  = juce::jmin (AlterState::kMaxStack, (int) column.size() + 1);

    // A column has ONE width, and it is the column's — read it from the module
    // that was already there, not from headIdx (dropping ABOVE the old head makes
    // the newcomer the head, and it would impose its own width on the column).
    auto existing = panels.getChild (oldHeadIdx);
    const int   colPrefW = (int)   existing.getProperty (AlterState::kPreferredWidth, 0);
    const float colRatio = (float) existing.getProperty (AlterState::kWidthRatio, 1.0f);

    for (int row = 0; row < runLen && headIdx + row < panels.getNumChildren(); ++row)
    {
        auto p = panels.getChild (headIdx + row);
        p.setProperty (AlterState::kStackRow,      row,      nullptr);
        p.setProperty (AlterState::kStackWeight,   1.0f,     nullptr);
        p.setProperty (AlterState::kPreferredWidth, colPrefW, nullptr);
        p.setProperty (AlterState::kWidthRatio,     colRatio, nullptr);
    }

    settings.sortPanelsByLayer();   // keeps blocks contiguous, then repairs the runs
    resized();
}

// Decides whether the pointer is asking to STACK the dragged module onto another
// one rather than to drop it beside it.
//
// The gesture is deliberately narrow: only the top and bottom third of a module
// count, the middle third still means "put it next to this one". That way the
// common horizontal reorder is unchanged and stacking is something you aim for.
// Columns that already hold AlterState::kMaxStack modules are not offered.
bool MainComponent::computeStackDropTarget (juce::Point<int> screenPos)
{
    const int prevTarget = hudStackTargetId;
    const bool prevBelow = hudStackBelow;

    hudStackTargetId = -1;

    // Fusion is the one module that never takes part in block stacking: it
    // already stacks modules, INSIDE itself, and offering both would mean two
    // different "put this on top of that" gestures on the same module.
    {
        auto dragged = settings.getPanelById (hudDragPanelId);
        if (dragged.isValid() && dragged.getProperty (AlterState::kType).toString() == "fusion")
            return hudStackTargetId != prevTarget;
    }

    for (auto& s : slots)
    {
        if (! s.host || s.id == hudDragPanelId) continue;
        if (s.host->getParentComponent() != this) continue;
        if ((bool) s.node.getProperty (AlterState::kDetached, false)) continue;
        if ((bool) s.node.getProperty (AlterState::kRowDetached, false)) continue;
        if ((bool) s.node.getProperty (AlterState::kHidden, false)) continue;
        if (s.node.getProperty (AlterState::kType).toString() == "fusion") continue;

        const auto g = localAreaToGlobal (s.host->getBounds());
        if (! g.contains (screenPos)) continue;

        // The column this module belongs to must have room for one more.
        const auto column = panelIdsInColumnOf (s.id);
        if ((int) column.size() >= AlterState::kMaxStack) break;

        const float rel = (float) (screenPos.y - g.getY()) / (float) juce::jmax (1, g.getHeight());
        if (rel > 0.34f && rel < 0.66f) break;          // middle third = ordinary side-by-side drop

        hudStackTargetId = s.id;
        hudStackBelow    = (rel >= 0.5f);
        break;
    }

    return hudStackTargetId != prevTarget || hudStackBelow != prevBelow;
}

// Every panel of the column the given panel sits in, top to bottom.
std::vector<int> MainComponent::panelIdsInColumnOf (int panelId)
{
    auto panels = settings.getPanelsRoot();
    if (! panels.isValid()) return {};

    int index = -1;
    for (int i = 0; i < panels.getNumChildren(); ++i)
        if ((int) panels.getChild (i).getProperty (AlterState::kId, -1) == panelId) { index = i; break; }

    if (index < 0) return {};

    // walk back to the column head (stackRow == 0), then forward over its members
    int head = index;
    while (head > 0 && (int) panels.getChild (head).getProperty (AlterState::kStackRow, 0) > 0)
        --head;

    std::vector<int> out { (int) panels.getChild (head).getProperty (AlterState::kId, -1) };
    for (int i = head + 1; i < panels.getNumChildren(); ++i)
    {
        if ((int) panels.getChild (i).getProperty (AlterState::kStackRow, 0) <= 0) break;
        out.push_back ((int) panels.getChild (i).getProperty (AlterState::kId, -1));
    }

    return out;
}

void MainComponent::beginHudDrag (int panelId, juce::Point<int> screenPos)
{
    if (hudDragPanelId == panelId) return;
    hudDragPanelId = panelId;
    hudDragInsertIndex = 0;
    hudStackTargetId = -1;

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

    // Stacking takes priority: while the pointer sits in the top/bottom third of
    // another module the row must not reflow, so bail out before the horizontal
    // insertion index is recomputed.
    computeStackDropTarget (screenPos);
    if (hudStackTargetId >= 0)
    {
        resized();
        return;
    }

    // compute insertion index based on current hud host centers (excluding dragged)
    std::vector<PanelHost*> hudHosts;
    for (auto& s : slots)
    {
        if (! s.host) continue;
        const bool detached = (bool) s.node.getProperty (AlterState::kDetached, false);
        const bool hidden   = (bool) s.node.getProperty (AlterState::kHidden,   false);
        if (! detached && !hidden && s.host->getParentComponent() == this && s.id != hudDragPanelId)
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

    // Remember the stack target before the drag state is torn down.
    const int  stackTarget = hudStackTargetId;
    const bool stackBelow  = hudStackBelow;

    // IMPORTANT: Reset drag state BEFORE removing ghost to prevent timer from accessing nullptr
    hudDragPanelId = -1;
    hudDragInsertIndex = -1;
    hudStackTargetId = -1;

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
    if (hud.contains (screenPos) && stackTarget >= 0)
    {
        stackPanelOnto (panelId, stackTarget, stackBelow);
    }
    else if (hud.contains (screenPos))
    {
        // A plain in-HUD drop always leaves any stack the module was part of: it
        // becomes the head of its own column again.
        if (auto node = settings.getPanelById (panelId); node.isValid())
            node.setProperty (AlterState::kStackRow, 0, nullptr);

        // assign the block (layer) from the drop height first
        const int layers = settings.hudLayers();
        if (layers > 1)
        {
            const int layer = juce::jlimit (0, layers - 1,
                (screenPos.y - hud.getY()) * layers / juce::jmax (1, hud.getHeight()));
            auto node = settings.getPanelById (panelId);
            if (node.isValid())
            {
                node.setProperty (AlterState::kLayer, layer, nullptr);
                settings.sortPanelsByLayer();   // controller bubbles stay contiguous
            }
        }

        // perform reorder in ValueTree (this may rebuild `slots` and invalidate `slot`)
        reorderPanelByScreenPosition (panelId, screenPos);

        // Leaving a stack can orphan the module that was under this one, so repair
        // the runs even when the reorder itself was a no-op.
        settings.normaliseStacks();
    }
    else
    {
        // drop outside HUD -> detach to window and save bounds
        // (a detached module also leaves its column)
        if (auto n2 = settings.getPanelById (panelId); n2.isValid())
            n2.setProperty (AlterState::kStackRow, 0, nullptr);
        settings.normaliseStacks();

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
// Whole-row (block) tear-off
// =======================================================
std::vector<MainComponent::PanelHost*> MainComponent::getLayerHudHosts (int layer)
{
    const int layers = settings.hudLayers();
    std::vector<PanelHost*> out;
    auto panels = settings.getPanelsRoot();
    if (! panels.isValid()) return out;

    // Iterate in ValueTree (visual) order so the window matches the HUD row order.
    for (int i = 0; i < panels.getNumChildren(); ++i)
    {
        auto p = panels.getChild (i);
        const int id = (int) p.getProperty (AlterState::kId, -1);
        const bool detached = (bool) p.getProperty (AlterState::kDetached, false);
        const bool hidden   = (bool) p.getProperty (AlterState::kHidden,   false);
        const int  l        = juce::jlimit (0, layers - 1, (int) p.getProperty (AlterState::kLayer, 0));
        if (detached || hidden || l != layer) continue;

        if (auto* slot = findSlotById (id))
            if (slot->host)
                out.push_back (slot->host.get());
    }
    return out;
}

void MainComponent::beginRowDetach (int layer, juce::Point<int> dropScreenPos)
{
    auto hosts = getLayerHudHosts (layer);
    if (hosts.empty()) return;

    // window size = combined width of the row's current hosts (min height guard)
    int combinedW = 0, maxH = 0;
    for (auto* h : hosts)
    {
        combinedW += juce::jmax (1, h->getWidth());
        maxH = juce::jmax (maxH, h->getHeight());
    }
    combinedW = juce::jmax (240, combinedW);
    maxH      = juce::jmax (120, maxH);

    const juce::Rectangle<int> wBounds (dropScreenPos.getX() - combinedW / 2,
                                        dropScreenPos.getY() - maxH / 2,
                                        combinedW, maxH);

    // Mark every panel of the row; the ValueTree listener spawns the RowWindow.
    for (auto* h : hosts)
    {
        auto node = h->getNode();
        if (node.isValid())
        {
            node.setProperty (AlterState::kRowWnd, wBounds.toString(), nullptr);
            node.setProperty (AlterState::kRowDetached, true, nullptr);
        }
    }
}

// Single authority for torn-off blocks. Re-derives, from state, which modules
// belong in which RowWindow (kRowDetached && !kDetached && !kHidden, grouped by
// layer) and creates / refreshes / closes the windows accordingly. Any module
// that belongs in the HUD is re-homed there. Safe to call after ANY relevant
// state change; it converges the UI to match the tree.
void MainComponent::syncRowWindows()
{
    const int layers = settings.hudLayers();

    std::array<std::vector<PanelHost*>, 3> want;
    for (auto& s : slots)
    {
        if (! s.host) continue;
        if ((bool) s.node.getProperty (AlterState::kDetached,    false)) continue; // own window
        if (! (bool) s.node.getProperty (AlterState::kRowDetached, false)) continue;
        if ((bool) s.node.getProperty (AlterState::kHidden,      false)) continue;
        const int L = juce::jlimit (0, layers - 1, (int) s.node.getProperty (AlterState::kLayer, 0));
        want[(size_t) L].push_back (s.host.get());
    }

    for (int L = 0; L < 3; ++L)
    {
        auto it = rowWindows.find (L);
        if (want[(size_t) L].empty())
        {
            if (it != rowWindows.end())
                rowWindows.erase (it);          // block emptied → close it (no leftover strip)
        }
        else
        {
            // shared bounds from any member's saved kRowWnd
            juce::Rectangle<int> wBounds;
            const auto sTxt = want[(size_t) L].front()->getNode()
                                  .getProperty (AlterState::kRowWnd).toString();
            juce::StringArray parts;
            parts.addTokens (sTxt, " ,\t", "");
            parts.removeEmptyStrings();
            if (parts.size() >= 4)
                wBounds = juce::Rectangle<int> (parts[0].getIntValue(), parts[1].getIntValue(),
                                                parts[2].getIntValue(), parts[3].getIntValue());

            if (it == rowWindows.end())
                rowWindows[L] = std::make_unique<RowWindow> (L, want[(size_t) L], wBounds, *this);
            else
                it->second->setHosts (want[(size_t) L]);
        }
    }

    // Re-home HUD modules that were orphaned when a window dropped/closed them.
    for (auto& s : slots)
    {
        if (! s.host) continue;
        if ((bool) s.node.getProperty (AlterState::kDetached,    false)) continue;
        if ((bool) s.node.getProperty (AlterState::kRowDetached, false)) continue;
        if (s.host->getParentComponent() != this)
            addAndMakeVisible (*s.host);
    }

    resized();
    repaint();
}

// A single detached module (PanelWindow) was released: decide its new home.
void MainComponent::handlePanelWindowDrop (int id, juce::Point<int> centreScreen)
{
    auto node = settings.getPanelById (id);
    if (! node.isValid()) return;

    // 1) dropped over a torn-off block → rejoin that block
    for (auto& kv : rowWindows)
    {
        if (kv.second && kv.second->getScreenBounds().contains (centreScreen))
        {
            node.setProperty (AlterState::kLayer, kv.first, nullptr);
            node.setProperty (AlterState::kRowWnd,
                              kv.second->getScreenBounds().toString(), nullptr);
            node.setProperty (AlterState::kRowDetached, true,  nullptr);
            node.setProperty (AlterState::kDetached,    false, nullptr); // listener → syncRowWindows
            return;
        }
    }

    // 2) dropped over the HUD → live in the HUD
    if (getHudScreenBounds().contains (centreScreen))
    {
        node.setProperty (AlterState::kRowDetached, false, nullptr);
        node.setProperty (AlterState::kDetached,    false, nullptr);
        return;
    }

    // 3) otherwise stay floating (bounds already saved)
}

// Distinct layer indices that currently have at least one module living in the HUD
std::vector<int> MainComponent::visibleLayersInHud()
{
    const int layers = settings.hudLayers();
    std::array<bool, 3> present { { false, false, false } };
    for (auto& s : slots)
    {
        if (! s.host || s.host->getParentComponent() != this) continue;
        if ((bool) s.node.getProperty (AlterState::kDetached,    false)) continue;
        if ((bool) s.node.getProperty (AlterState::kRowDetached, false)) continue;
        if ((bool) s.node.getProperty (AlterState::kHidden,      false)) continue;
        const int L = juce::jlimit (0, layers - 1, (int) s.node.getProperty (AlterState::kLayer, 0));
        present[(size_t) L] = true;
    }
    std::vector<int> vis;
    for (int L = 0; L < layers; ++L) if (present[(size_t) L]) vis.push_back (L);
    return vis;
}

// Which insertion slot (0..N) a screen-Y maps to, using the visible rows' centres
int MainComponent::computeRowDropSlot (int screenY)
{
    auto vis = visibleLayersInHud();

    std::vector<int> centres;
    centres.reserve (vis.size());
    for (int L : vis)
    {
        // representative host of this layer → its vertical screen centre
        for (auto& s : slots)
        {
            if (! s.host || s.host->getParentComponent() != this) continue;
            const int sl = juce::jlimit (0, settings.hudLayers() - 1,
                                         (int) s.node.getProperty (AlterState::kLayer, 0));
            if (sl != L) continue;
            const auto g = localAreaToGlobal (s.host->getBounds());
            centres.push_back (g.getCentreY());
            break;
        }
    }

    int slot = (int) centres.size();
    for (size_t i = 0; i < centres.size(); ++i)
        if (screenY < centres[i]) { slot = (int) i; break; }
    return slot;
}

void MainComponent::updateRowDropPreview (int layer, juce::Point<int> screenPos)
{
    const auto hud = getHudScreenBounds();

    if (! hud.contains (screenPos))
    {
        // left the HUD → drop the preview
        if (rowDropActive)
        {
            rowDropActive = false;
            if (dragPlaceholder) { removeChildComponent (dragPlaceholder.get()); dragPlaceholder.reset(); }
            resized();
        }
        return;
    }

    if (! dragPlaceholder)
    {
        dragPlaceholder = std::make_unique<DragPlaceholder>();
        addAndMakeVisible (*dragPlaceholder);
    }

    rowDropActive = true;
    rowDropLayer  = layer;
    rowDropSlot   = computeRowDropSlot (screenPos.y);
    resized();
}

void MainComponent::endRowDrop (int layer, juce::Point<int> screenPos)
{
    const bool wasPreview = rowDropActive;
    rowDropActive = false;
    if (dragPlaceholder) { removeChildComponent (dragPlaceholder.get()); dragPlaceholder.reset(); }

    const auto hud = getHudScreenBounds();
    if (! hud.contains (screenPos))
    {
        resized();   // stays floating; kRowWnd already saved by the window
        return;
    }

    // ── Reattach at the chosen vertical slot ──
    auto vis = visibleLayersInHud();                    // in-HUD blocks (excludes the dragged one)
    const int slot = wasPreview ? juce::jlimit (0, (int) vis.size(), rowDropSlot)
                                : computeRowDropSlot (screenPos.y);

    // desired top-to-bottom order of block groups (old layer keys; dragged = `layer`)
    std::vector<int> order = vis;
    order.insert (order.begin() + juce::jlimit (0, (int) order.size(), slot), layer);

    // old layer key -> new contiguous index
    std::map<int, int> remap;
    for (int i = 0; i < (int) order.size(); ++i) remap[order[(size_t) i]] = i;

    auto panels = settings.getPanelsRoot();
    if (panels.isValid())
    {
        for (int i = 0; i < panels.getNumChildren(); ++i)
        {
            auto p = panels.getChild (i);
            if ((bool) p.getProperty (AlterState::kDetached, false)) continue; // individual windows untouched

            const bool rowDet = (bool) p.getProperty (AlterState::kRowDetached, false);
            const int  pl     = juce::jlimit (0, settings.hudLayers() - 1,
                                              (int) p.getProperty (AlterState::kLayer, 0));

            if (rowDet)
            {
                // only the block being dropped (its original layer key == `layer`);
                // any OTHER torn-off block must stay detached and untouched.
                if (pl != layer) continue;
                p.setProperty (AlterState::kLayer, remap[layer], nullptr);
                p.setProperty (AlterState::kRowDetached, false, nullptr); // triggers async re-attach
            }
            else
            {
                auto it = remap.find (pl);
                if (it == remap.end()) continue;        // hidden-only layer etc. → leave as is
                p.setProperty (AlterState::kLayer, it->second, nullptr);
            }
        }
    }

    resized();
    repaint();
}

// =======================================================
// RowDetachHandle (per-row grip in the right strip)
// =======================================================
MainComponent::RowDetachHandle::RowDetachHandle (MainComponent& o, int l)
    : mainComp (o), layer (l)
{
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void MainComponent::RowDetachHandle::mouseDown (const juce::MouseEvent& e)
{
    dragging   = true;
    dragActive = false;
    startScreenPos = e.getScreenPosition();
}

void MainComponent::RowDetachHandle::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging) return;
    const auto now = e.getScreenPosition();
    if (! dragActive
        && (std::abs (now.getX() - startScreenPos.getX()) > 6
            || std::abs (now.getY() - startScreenPos.getY()) > 6))
    {
        dragActive = true;
        repaint();
    }
}

void MainComponent::RowDetachHandle::mouseUp (const juce::MouseEvent&)
{
    if (! dragging) return;
    dragging = false;
    if (! dragActive) return;
    dragActive = false;
    repaint();

    const auto dropPos = juce::Desktop::getMousePosition();
    if (! mainComp.getHudScreenBounds().contains (dropPos))
        mainComp.beginRowDetach (layer, dropPos);
}

void MainComponent::RowDetachHandle::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    AlterTheme::paintBackground (g, b);

    const auto accent = AlterTheme::themeAccent();
    const bool active = dragActive;

    // subtle vertical grip: a short column of dots hints "grab this row"
    g.setColour (accent.withAlpha (active ? 0.95f : 0.45f));
    const float cx = b.getCentreX();
    const float cy = b.getCentreY();
    const float r  = 1.2f;
    for (int i = -2; i <= 2; ++i)
    {
        const float yy = cy + (float) i * 5.0f;
        g.fillEllipse (cx - r, yy - r, r * 2.0f, r * 2.0f);
    }

    // faint separator on the right edge (between row grip and window-move grip)
    g.setColour (accent.withAlpha (0.18f));
    g.drawLine (b.getRight() - 0.5f, b.getY() + 3.0f, b.getRight() - 0.5f, b.getBottom() - 3.0f, 1.0f);
}

void MainComponent::layoutRowHandles()
{
    // One grip per block ACTUALLY VISIBLE in the HUD — not per configured layer.
    // The old per-hudLayers() version left stale grips behind (e.g. 3 grips with
    // only 2 docked blocks) after tearing rows off / re-docking / moving modules,
    // because kHudLayers stays at its configured maximum. The grips now mirror
    // exactly the rows the layout paints, with matching heights.
    const auto vis  = visibleLayersInHud();
    const int  nVis = (int) vis.size();

    // keep the handle count in sync with the visible block count
    if ((int) rowHandles.size() != nVis)
    {
        rowHandles.clear();
        for (int i = 0; i < nVis; ++i)
        {
            auto h = std::make_unique<RowDetachHandle> (*this, vis[(size_t) i]);
            addAndMakeVisible (*h);
            h->setAlwaysOnTop (true);
            rowHandles.push_back (std::move (h));
        }
    }

    if (rowHandleColumn.isEmpty() || nVis == 0) return;

    const auto wts = settings.blockWeights();
    float total = 0.0f;
    for (int L : vis) total += wts[(size_t) L];
    if (total <= 0.001f) total = (float) nVis;

    int y = rowHandleColumn.getY();
    for (int i = 0; i < nVis; ++i)
    {
        const int L   = vis[(size_t) i];
        const int hgt = (i == nVis - 1)
                          ? (rowHandleColumn.getBottom() - y)
                          : juce::jmax (20, (int) std::round (rowHandleColumn.getHeight()
                                                              * wts[(size_t) L] / total));
        rowHandles[(size_t) i]->setLayer (L);
        rowHandles[(size_t) i]->setBounds (rowHandleColumn.getX(), y,
                                           rowHandleColumn.getWidth(), hgt);
        y += hgt;
    }
}

// =======================================================
// ValueTree listener
// =======================================================
void MainComponent::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    // ── PURE BOOKKEEPING: a floating window's own rectangle ──────────────────
    //
    // kWnd and kRowWnd record where a detached window or a torn-off block sits.
    // Nothing reacts to them; they are read back when a window is restored and
    // never in between. But they are WRITTEN from resized() and moved(), so
    // while a floating block is being dragged they change at the rate the window
    // manager sends WM_SIZE / WM_MOVE — and until now every one of those writes
    // fell through this whole function to applyPanelPropsToView, which re-applies
    // every property of the module and repaints it, once per host in the block.
    //
    // Dropping out here is not an optimisation of that work, it is the removal of
    // work that never had any effect in the first place.
    if (property == AlterState::kWnd || property == AlterState::kRowWnd)
        return;

    // Keep the plugin-facing module registry (values/colours/names) fresh — but
    // only MARK it, and let the timer rebuild it at most every 50 ms. Rebuilding
    // it inline made every automated float walk every panel and every one of its
    // ~25 properties, on the message thread that paints the HUD.
    registryDirty = true;

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

    // Global visual theme (incl. the custom-theme colour pair)
    if (property == AlterState::kTheme
        || property == AlterState::kThemeColor1
        || property == AlterState::kThemeColor2)
    {
        AlterTheme::customPrimary   = juce::Colour ((uint32_t)(int) settings.getTree()
                                          .getProperty (AlterState::kThemeColor1, (int) 0xFF3D96E7));
        AlterTheme::customSecondary = juce::Colour ((uint32_t)(int) settings.getTree()
                                          .getProperty (AlterState::kThemeColor2, (int) 0xFF6902D6));
        AlterTheme::setTheme ((int) settings.getTree().getProperty (AlterState::kTheme, 0));
        refreshEmptyLogo();   // white-on-dark / black-on-light watermark follows the theme
        repaint();
        return;
    }

    // Global FFT resolution: push the new order to plugins + local capture.
    if (property == AlterState::kMaxFftBins)
    {
        computeAndPushNeeds();
        return;
    }


    // Stacking inside a block only ever moves pixels around: no window to
    // reconcile, no audio needs to recompute. Keep it out of the heavier block
    // handler below — kStackWeight in particular changes on every frame of a
    // divider drag, and firing syncRowWindows() at that rate would be absurd.
    if (property == AlterState::kStackRow    || property == AlterState::kStackWeight
        || property == AlterState::kPreferredWidth || property == AlterState::kWidthRatio)
    {
        // The two WIDTH keys are here for a bug, not a speed-up. They only ever
        // moved pixels around, so they fell through to the per-panel fast path
        // below, which re-applies the module's settings and returns — no layout.
        // That was invisible while the only thing writing them was the divider
        // drag, because setModuleWidth calls resized() itself. Anything ELSE that
        // sets a width — the controller, a preset, an undo — changed the state and
        // nothing moved until the next unrelated re-layout.
        // Same deferral as a window drag, for the same reason. kStackWeight moves
        // on every frame of a divider drag, and a divider drag resizes modules
        // exactly as dragging the window edge does — so running the fusion layer
        // pass inline would block the message thread on the GL frame at the rate
        // the mouse moves. resized() itself is cheap and stays synchronous, so
        // the drag tracks the cursor; only the layer pass waits for it to stop.
        //
        // The HUD's own bounds have NOT changed here — only a divider inside it —
        // so resized() would otherwise take its "size is stable" branch and run
        // the pass inline. This asks it to defer anyway.
        const juce::ScopedValueSetter<bool> defer (deferFusionLayout, true);
        resized();
        return;
    }

    // A module joining or leaving a Fusion changes who is laid out where, and
    // the audio a module needs does not change by moving — but a released module
    // has to get its HUD slot back, so this is a full re-layout.
    if (property == AlterState::kFusionHost)
    {
        resized();
        repaint();
        return;
    }

    // HUD blocks: layer assignment / block count / hide change → re-layout
    if (property == AlterState::kLayer || property == AlterState::kHudLayers
        || property == AlterState::kBlockWeights || property == AlterState::kHidden)
    {
        // if the block count shrank, send any now-orphaned torn-off rows back
        if (property == AlterState::kHudLayers)
        {
            const int layers = settings.hudLayers();
            std::vector<int> orphaned;
            for (auto& kv : rowWindows)
                if (kv.first >= layers)
                    orphaned.push_back (kv.first);

            for (int l : orphaned)
                if (auto panels2 = settings.getPanelsRoot(); panels2.isValid())
                    for (int i = 0; i < panels2.getNumChildren(); ++i)
                    {
                        auto p = panels2.getChild (i);
                        if ((int) p.getProperty (AlterState::kLayer, 0) == l)
                            p.setProperty (AlterState::kRowDetached, false, nullptr);
                    }
        }

        // reconcile torn-off blocks with the new layer set. ASYNC: this path can
        // run synchronously from a RowWindow's own mouse event (via endRowDrop),
        // and syncRowWindows may destroy that very window — defer to be safe.
        juce::MessageManager::callAsync ([this]() { syncRowWindows(); });
        resized();
        repaint();
        computeAndPushNeeds();   // hidden modules stop requesting their audio data
        return;
    }

    // A fusion's LAYER DETAIL changes the size its layers are laid out at, so it
    // needs the same re-layout a resize does. Async for the same reason the
    // rotation path is: layoutFusionLayers reparents components and can reach the
    // GL host's source registry, whose lock the GL thread holds for a whole frame.
    if (property == AlterState::kFusionDetail)
    {
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::MessageManager::callAsync ([sp]() mutable
        {
            if (sp != nullptr) sp->layoutFusionLayers();
        });
        return;
    }

    // per-module audio instance override (auto/global vs a specific plugin instance)
    if (property == AlterState::kAudioInstance)
    {
        const int pid = (int) tree.getProperty (AlterState::kId, -1);
        const auto inst = (juce::uint32) (juce::int64) tree.getProperty (AlterState::kAudioInstance, 0);
        if (auto* slot = findSlotById (pid))
            if (slot->host && slot->host->getAudioSource())
                slot->host->getAudioSource()->setOverrideInstance (inst);
        computeAndPushNeeds();   // routing changed -> recompute per-instance needs
        return;
    }

    // Switching the meter to/from Level history changes whether it needs the
    // waveform stream — recompute per-instance UDP needs (then still apply below).
    if (property == AlterState::kMeterMode)
        computeAndPushNeeds();

    // Colour by tone starts (or stops) needing the FFT stream. Both keys, because
    // the meter carries tone colour on kMeterToneColor while everyone else uses
    // kColorMode — see the note on kMeterToneColor in AlterState. Cheap to run for
    // a kColorMode change on a module that has no tone mode: the table just
    // recomputes the same bits.
    if (property == AlterState::kMeterToneColor || property == AlterState::kColorMode)
        computeAndPushNeeds();

    // Constant-Q on/off changes whether the (expensive) CQT stream is needed.
    if (property == AlterState::kConstantQ)
        computeAndPushNeeds();

    // Stereo overlay on/off changes the waveform need AND (with Constant-Q) the
    // per-channel L/R CQT streams. Without this recompute the stereo+CQ display
    // only worked when Stereo was toggled BEFORE Constant-Q (order-dependent).
    if (property == AlterState::kSpecStereo)
        computeAndPushNeeds();

    // Enhanced-frequency (reassignment) needs the raw waveform stream — recompute.
    if (property == AlterState::kSpectroReassign)
        computeAndPushNeeds();

    // Global Constant-Q: recompute needs + re-apply to every Spectrum/Spectrogram.
    if (property == AlterState::kGlobalCqt)
    {
        computeAndPushNeeds();
        auto root = settings.getPanelsRoot();
        for (int i = 0; root.isValid() && i < root.getNumChildren(); ++i)
        {
            auto pp = root.getChild (i);
            if (auto* slot = findSlotById ((int) pp.getProperty (AlterState::kId, -1)))
                if (slot->host && slot->host->getInner())
                    applyPanelPropsToView (pp, *slot->host->getInner());
        }
        return;
    }

    auto panels = settings.getPanelsRoot();
    if (!panels.isValid()) return;

    // whole-row tear-off: spawn/dissolve one RowWindow per layer
    if (property == AlterState::kRowDetached)
    {
        juce::MessageManager::callAsync ([this]() { syncRowWindows(); });
        return;
    }

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
            // a module may have (re)joined/left a torn-off block → reconcile
            syncRowWindows();
        });

        return;
    }

    // ── FAST PATH: the property belongs to ONE panel ─────────────────────────
    // This is the case for every automated parameter, and it used to walk EVERY
    // panel and re-apply EVERY one of its properties — so with six modules open,
    // one automated float cost six full property sweeps. The changed node tells
    // us exactly which panel it was; apply it there and stop.
    if (tree.getParent() == panels)
    {
        auto* slot = findSlotById ((int) tree.getProperty (AlterState::kId, -1));
        if (slot == nullptr || slot->host == nullptr)
            return;

        // Rotation changes the panel's transform, so the host must re-lay-out
        // before the new properties are applied. It also changes the SHAPE a layer
        // is laid out at and the quarter turn handed to the fusion shader, so any
        // Fusion has to be told about it too — rotating a layer would otherwise do
        // nothing until the next full HUD layout.
        //
        // ASYNC, and it has to be. layoutFusionLayers reparents components and,
        // through setLayerSources -> setLayerMode -> updateHostRegistration,
        // reaches AlterGLHost::addSource/removeSource — which block on a lock the
        // GL thread holds for its whole frame, while that frame is taking the layer
        // locks this thread may already be inside. Running it straight from a
        // ValueTree callback is a deadlock waiting to happen, and the same reason
        // the detach/attach path above defers its work.
        // A layer's quarter turn is the MODULE'S OWN rotation, and it changes the
        // SHAPE that layer is laid out at, so a fusion holding it needs the same
        // re-layout an ordinary rotated panel does.
        if (property == AlterState::kRotationAngle)
        {
            slot->host->resized();

            // And the fusion has to be TOLD, because a layer is never drawn in a
            // rectangle of its own: the panel's transform never reaches it and the
            // shader does the turn instead. Re-applying the host fusion's settings
            // is what carries the new angle down to it.
            const int hostId = (int) tree.getProperty (AlterState::kFusionHost, 0);

            juce::Component::SafePointer<MainComponent> sp (this);
            juce::MessageManager::callAsync ([sp, hostId]() mutable
            {
                if (sp == nullptr) return;

                sp->layoutFusionLayers();

                if (hostId > 0)
                    if (auto* fs = sp->findSlotById (hostId))
                        if (fs->host && fs->host->getInner())
                            sp->applyPanelPropsToView (fs->node, *fs->host->getInner());
            });
        }

        if (property == AlterState::kMeasureState)
        {
            if (auto* meter = dynamic_cast<VisualAudioMeter*> (slot->host->getInner()))
            {
                if ((int) tree.getProperty (AlterState::kMeasureState, 0) == 1)
                    meter->startMeasurement();
                else
                    meter->stopMeasurement();
            }
            return;
        }

        if (property == AlterState::kMeterView)
        {
            if (auto* meter = dynamic_cast<VisualAudioMeter*> (slot->host->getInner()))
                meter->setTrendMode ((int) tree.getProperty (AlterState::kMeterView, 0) == 1);
            return;
        }

        if (slot->host->getInner())
            applyPanelPropsToView (tree, *slot->host->getInner());
        return;
    }

    // ── SLOW PATH: a property on some other node — sweep everything ───────────
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

    // Same reason as the fast path, deferred for the same reason: a rotation
    // changes the shape a layer is laid out at and the quarter turn the fusion
    // shader is given, and the re-layout must not run inside this callback.
    if (property == AlterState::kRotationAngle)
    {
        juce::Component::SafePointer<MainComponent> sp (this);
        juce::MessageManager::callAsync ([sp]() mutable
        {
            if (sp != nullptr) sp->layoutFusionLayers();
        });
    }
}

void MainComponent::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) { rebuildSlotsFromState(); pushControlRegistry(); }
void MainComponent::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) { rebuildSlotsFromState(); pushControlRegistry(); }
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

    // A RowWindow holds raw PanelHost pointers. Slots (and thus hosts) may be
    // removed/rebuilt below, so dissolve all row windows first (their destructors
    // safely detach the still-alive hosts) and re-create them from state at the end.
    rowWindows.clear();

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

            // per-module audio routing: auto (0) -> global source, else a specific instance
            auto src = std::make_unique<ModuleAudioSource> (udpSource);
            src->setGlobalSource (activeAudioSource);
            src->setOverrideInstance ((juce::uint32) (juce::int64)
                                      p.getProperty (AlterState::kAudioInstance, 0));
            IAudioSource& as = *src;

            if (type == "rms" || type == "audiometer")
            {
                auto v = std::make_unique<VisualAudioMeter> (as);
                applyPanelPropsToView (p, *v);
                // persist mouse-wheel zoom of the level-history window
                v->onLevelHistoryWindowChanged = [p] (float seconds) mutable
                { p.setProperty (AlterState::kOscLtWindow, seconds, nullptr); };
                view = std::move (v);
            }
            else if (type == "spectrum")
            {
                auto v = std::make_unique<VisualSpectrum> (as);
                v->setAssumedSampleRate (as.getSampleRate());   // real device rate → correct bin→Hz
                applyPanelPropsToView (p, *v);
                view = std::move (v);
            }
            else if (type == "oscillator" || type == "oscilator")
            {
                auto v = std::make_unique<VisualOscilator> (as);
                v->setAssumedSampleRate (as.getSampleRate());
                // apply panel properties immediately so colour/neon/smooth are set
                applyPanelPropsToView (p, *v);
                // persist mouse-wheel zoom of the long-term window
                v->onLongTermWindowChanged = [p] (float seconds) mutable
                { p.setProperty (AlterState::kOscLtWindow, seconds, nullptr); };
                view = std::move (v);
            }
            else if (type == "synesthesia")
            {
                auto v = std::make_unique<VisualSynesthesia> (as);
                applyPanelPropsToView (p, *v);
                view = std::move (v);
            }
            else if (type == "chladni")
            {
                auto v = std::make_unique<ChladniPatternMeter> (as);
                applyPanelPropsToView (p, *v);
                view = std::move (v);
            }
            else if (type == "toneanalyzer")
            {
                auto v = std::make_unique<ToneAnalyzerMeter> (as);
                applyPanelPropsToView (p, *v);
                view = std::move (v);
            }
            else if (type == "spectrogram")
            {
                auto v = std::make_unique<SpectrogramMeter> (as);
                applyPanelPropsToView (p, *v);
                view = std::move (v);
            }
            else if (type == "stereoscope")
            {
                auto v = std::make_unique<VisualStereoscope> (as);
                v->setAssumedSampleRate (as.getSampleRate());
                applyPanelPropsToView (p, *v);
                view = std::move (v);
            }
            else if (type == "geometry")
            {
                auto v = std::make_unique<GeometryVisual> (as);
                applyPanelPropsToView (p, *v);
                view = std::move (v);
            }
            else if (type == "fusion")
            {
                auto v = std::make_unique<FusionVisual> (as);
                applyPanelPropsToView (p, *v);
                view = std::move (v);
            }
            else
            {
                continue;
            }

            slot.host = std::make_unique<PanelHost> (*this, p, id, type, std::move (view), std::move (src));
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
        // 3b) restore torn-off blocks (whole-block windows) from state
        syncRowWindows();
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
        else if (meterMode == 2) v->setMeterMode (VisualAudioMeter::MeterMode::LUFS);
        else                     v->setMeterMode (VisualAudioMeter::MeterMode::LevelHistory);

        // Level-history params (display layout + time window + colour) — reuse the shared keys
        {
            const int dm = juce::jlimit (0, 3, (int) panel.getProperty (AlterState::kDisplayMode, 0));
            v->setLhDisplayMode (static_cast<VisualAudioMeter::DisplayMode> (dm));
            v->setLevelHistoryWindow ((float) panel.getProperty (AlterState::kOscLtWindow, 10.0f));
            // The L/R colour relationship is one more key this view borrows from the
            // Oscilloscope it was ported out of, alongside the window and the layout.
            v->setLhStereoColourMode ((int) panel.getProperty (AlterState::kOscLrColor, 0));
            if (panel.hasProperty (AlterState::kColor))
                v->setLhColour (juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor)));
            else
                v->setLhColour (juce::Colours::violet);
        }

        v->setClipZone ((bool) panel.getProperty (AlterState::kClipZone, true));
        v->setTwoBars   ((bool) panel.getProperty (AlterState::kMeterTwoBars, true));

        // Trend view applies only to RMS/TruePeak/LUFS (not Level history)
        const int meterView = (int) panel.getProperty (AlterState::kMeterView, 0);
        v->setTrendMode (meterMode != 3 && meterView == 1);

        const float smooth = (float) panel.getProperty (AlterState::kSmooth, 0.5f);
        v->setSmoothAmount (smooth);

        const int colorModeInt = (int) panel.getProperty (AlterState::kColorMode, 0);
        VisualAudioMeter::ColorMode colorMode = VisualAudioMeter::ColorMode::Standard;
        if (colorModeInt == 1) colorMode = VisualAudioMeter::ColorMode::CustomGradient;
        else if (colorModeInt == 2) colorMode = VisualAudioMeter::ColorMode::CustomComplementary;
        v->setColorMode (colorMode);

        // Colour by tone has its OWN key here: on this module kColorMode is already
        // spent on how the loudness zones are shaded, which is an independent
        // question from where the colour comes from. Both are applied — the meter
        // reads whichever pair applies.
        const bool meterTone = (bool) panel.getProperty (AlterState::kMeterToneColor, false);
        v->setColourByTone (meterTone);
        v->setToneTwist    ((bool)  panel.getProperty (AlterState::kToneTwist,  false));
        v->setToneSmooth   ((float) panel.getProperty (AlterState::kToneSmooth, 0.5f));
        v->setToneShade    (static_cast<VisualAudioMeter::ToneShade>
                            (juce::jlimit (0, 1, (int) panel.getProperty (AlterState::kMeterToneShade, 0))));

        // In tone mode the picked colour is not used, but it is still WRITTEN back —
        // switching tone colour off must return the meter to the colour the user
        // chose, not to the green default.
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
        const int   measurement = (int)   panel.getProperty (AlterState::kMeasurementMode, 0);
        const int   phon        = (int)   panel.getProperty (AlterState::kPhon,            60);

        v->setSmoothAmount      (smooth);
        v->setDisplayBins       (bins);
        v->setPsychoacousticMode(psycho, phon);
        v->setMeasurementMode   (measurement == 1);
        v->setReferenceGenre    ((int)  panel.getProperty (AlterState::kSpecReference, 0));
        v->setStereoMode        ((bool) panel.getProperty (AlterState::kSpecStereo,    false));
        v->setStereoColourMode  ((int)  panel.getProperty (AlterState::kSpecLrColor,   0));
        v->setPeakHold          ((bool) panel.getProperty (AlterState::kPeakHold,      false));
        v->setMirrorFreq        ((bool) panel.getProperty (AlterState::kSpecMirror,    false));
        v->setConstantQ         ((bool) panel.getProperty (AlterState::kConstantQ, false) || settings.globalCqt());

        v->setColourByTone ((int)   panel.getProperty (AlterState::kColorMode,  0) == 1);
        v->setToneTwist    ((bool)  panel.getProperty (AlterState::kToneTwist,  false));
        v->setToneSmooth   ((float) panel.getProperty (AlterState::kToneSmooth, 0.5f));

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
            else if (dm == 3) v->setDisplayMode (VisualOscilator::DisplayMode::MirrorStereo);
            else              v->setDisplayMode (VisualOscilator::DisplayMode::Mono);
        }

        if (panel.hasProperty (AlterState::kZoom))
            v->setZoom ((float) panel.getProperty (AlterState::kZoom, 0.420f));

        // Term combobox now: 0 = Short term, 1 = Long wave (Level history moved to
        // the Audio Meter). Map to the Oscilloscope's internal term ids (2 = long wave).
        const int oscTerm = (int) panel.getProperty (AlterState::kOscLongTerm, 0);
        v->setTermMode (oscTerm == 1 ? 2 : 0);
        v->setLongTermWindow ((float) panel.getProperty (AlterState::kOscLtWindow, 10.0f));

        v->setStereoColourMode ((int)  panel.getProperty (AlterState::kOscLrColor, 0));
        v->setClipZone          ((bool) panel.getProperty (AlterState::kClipZone,   true));

        v->setColourByTone ((int)   panel.getProperty (AlterState::kColorMode,  0) == 1);
        v->setToneTwist    ((bool)  panel.getProperty (AlterState::kToneTwist,  false));
        v->setToneSmooth   ((float) panel.getProperty (AlterState::kToneSmooth, 0.5f));
    }
    else if (type == "synesthesia")
    {
        auto* v = dynamic_cast<VisualSynesthesia*> (&view);
        if (v == nullptr) return;

        if (panel.hasProperty (AlterState::kSmooth))
            v->setSmoothAmount ((float) panel.getProperty (AlterState::kSmooth, 0.15f));
        v->setSyncToBPM   ((bool)  panel.getProperty (AlterState::kSynBpmSync, false));
        v->setBpm         ((float) panel.getProperty (AlterState::kSynBpm, 120.0f));
        v->setBeatDivision ((int)  panel.getProperty (AlterState::kSynBeatDiv, 5));
        if (panel.hasProperty (AlterState::kZoom))
            v->setZoom ((float) panel.getProperty (AlterState::kZoom, 1.0f));
        if (panel.hasProperty (AlterState::kRotation))
            v->setRotation ((float) panel.getProperty (AlterState::kRotation, 0.0f));
        if (panel.hasProperty (AlterState::kSymmetry))
            v->setSymmetry ((int) panel.getProperty (AlterState::kSymmetry, 1));
        if (panel.hasProperty (AlterState::kSaturation))
            v->setSaturation ((float) panel.getProperty (AlterState::kSaturation, 1.0f));
        v->setBrightness ((float) panel.getProperty (AlterState::kSynBrightness, 1.0f));
        if (panel.hasProperty (AlterState::kBloom))
            v->setBloom ((float) panel.getProperty (AlterState::kBloom, 0.0f));
        if (panel.hasProperty (AlterState::kSpeed))
            v->setShaderSpeed ((float) panel.getProperty (AlterState::kSpeed, 1.0f));
        v->setReactivity ((float) panel.getProperty (AlterState::kSynReact, 0.0f));

        // colour mode (1 = tone/pitch hue, 0 = manual base colour) + 'Change' morph
        v->setColourMode ((int) panel.getProperty (AlterState::kColorMode, 1) == 1);
        v->setToneTwist  ((bool) panel.getProperty (AlterState::kToneTwist, false));
        if (panel.hasProperty (AlterState::kColor))
            v->setBaseColour (juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor)));
        v->setVariation ((float) panel.getProperty (AlterState::kFragment, 0.0f));
        v->setTransmute ((float) panel.getProperty (AlterState::kTransmute, 0.0f));
        v->setMirror ((bool) panel.getProperty (AlterState::kMirror, false));
        v->setClear ((float) panel.getProperty (AlterState::kClear, 0.0f));
        v->setDenoise ((float) panel.getProperty (AlterState::kDenoise, 0.0f));
        v->setTunnel  ((float) panel.getProperty (AlterState::kSynTunnel, 0.0f));
        v->setVortex  ((float) panel.getProperty (AlterState::kSynVortex, 0.0f));
        v->setCurveSmooth ((float) panel.getProperty (AlterState::kCurveSmooth, 0.0f));
        v->setSubBinInterp (true);   // always on (cheap, strictly more precise)
    }
    else if (type == "chladni")
    {
        auto* v = dynamic_cast<ChladniPatternMeter*> (&view);
        if (v == nullptr) return;

        v->setAudioReactive  ((bool)  panel.getProperty (AlterState::kChladniReactive, true));
        v->setModeShift      ((int)   panel.getProperty (AlterState::kChladniShift,    0));
        v->setM              ((int)   panel.getProperty (AlterState::kChladniM,        2));
        v->setN              ((int)   panel.getProperty (AlterState::kChladniN,        3));
        v->setAspectRatio    ((float) panel.getProperty (AlterState::kChladniAR,       1.0f));
        v->setSandSmoothness ((float) panel.getProperty (AlterState::kChladniSharp,    0.5f));
        v->setParticleCount  ((int)   panel.getProperty (AlterState::kChladniParticles, 5000));
        v->setMaterial       ((int)   panel.getProperty (AlterState::kChladniMaterial, 0));
        v->setColourByTone   ((int)   panel.getProperty (AlterState::kColorMode,       1) == 1);
        v->setToneTwist      ((bool)  panel.getProperty (AlterState::kToneTwist,   false));
        v->setToneSmooth     ((float) panel.getProperty (AlterState::kToneSmooth,  0.374f));
        v->setSubBinInterp   (true);   // always on

        if (panel.hasProperty (AlterState::kColor))
            v->setSandColour (juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor)));
    }
    else if (type == "toneanalyzer")
    {
        auto* v = dynamic_cast<ToneAnalyzerMeter*> (&view);
        if (v == nullptr) return;

        v->setSensitivity ((float) panel.getProperty (AlterState::kToneSens, 0.5f));
        v->setTunerMode ((bool) panel.getProperty (AlterState::kToneTuner, false));

        v->setColourByTone ((int)   panel.getProperty (AlterState::kColorMode,  0) == 1);
        v->setToneTwist    ((bool)  panel.getProperty (AlterState::kToneTwist,  false));
        v->setToneSmooth   ((float) panel.getProperty (AlterState::kToneSmooth, 0.5f));

        if (panel.hasProperty (AlterState::kColor))
            v->setAccentColour (juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor)));
    }
    else if (type == "spectrogram")
    {
        auto* v = dynamic_cast<SpectrogramMeter*> (&view);
        if (v == nullptr) return;

        v->setSmoothAmount ((float) panel.getProperty (AlterState::kSmooth, 0.35f));
        v->setTimeWindow   ((float) panel.getProperty (AlterState::kSpectroWindow, 30.0f));
        v->setMirrored     ((bool)  panel.getProperty (AlterState::kSpectroMirror, false));
        v->setLineFill     ((float) panel.getProperty (AlterState::kSpectroLineFill, 0.0f));
        v->setReassign     ((bool)  panel.getProperty (AlterState::kSpectroReassign, false));
        v->setConstantQ    ((bool)  panel.getProperty (AlterState::kConstantQ, false) || settings.globalCqt());
        v->setColourMode   ((int)   panel.getProperty (AlterState::kColorMode, 0));   // 0=theme, 1=custom, 2=by tone
        v->setToneTwist    ((bool)  panel.getProperty (AlterState::kToneTwist,  false));
        v->setToneSmooth   ((float) panel.getProperty (AlterState::kToneSmooth, 0.5f));
        if (panel.hasProperty (AlterState::kColor))
            v->setCustomColour (juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor)));
        if (panel.hasProperty (AlterState::kColor2))
            v->setCustomColour2 (juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor2)));
    }
    else if (type == "stereoscope")
    {
        auto* v = dynamic_cast<VisualStereoscope*> (&view);
        if (v == nullptr) return;

        v->setStereoMode   ((int)   panel.getProperty (AlterState::kStereoMode, 0));
        v->setSmoothAmount ((float) panel.getProperty (AlterState::kSmooth, 0.5f));
        v->setParticles    ((bool)  panel.getProperty (AlterState::kStereoParticles, false));
        v->setDensity      ((float) panel.getProperty (AlterState::kStereoDensity, 0.5f));
        v->setUseControllerBins ((bool) panel.getProperty (AlterState::kStereoCtrlBins, false));
        v->setControllerBins    (settings.maxFftBins());
        v->setBrightness        ((float) panel.getProperty (AlterState::kStereoBright, 0.5f));
        // Defaulted to 0.5 (neutral), so a panel saved before these existed reads
        // back as the sizes it was drawn with.
        v->setLineWidth         ((float) panel.getProperty (AlterState::kStereoLineW,     0.5f));
        v->setPointSize         ((float) panel.getProperty (AlterState::kStereoPointSize, 0.5f));
        v->setColourByTone ((int)   panel.getProperty (AlterState::kColorMode,  0) == 1);
        v->setToneTwist    ((bool)  panel.getProperty (AlterState::kToneTwist,  false));
        v->setToneSmooth   ((float) panel.getProperty (AlterState::kToneSmooth, 0.5f));
        if (panel.hasProperty (AlterState::kColor))
            v->setColour (juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor)));
    }
    else if (type == "geometry")
    {
        auto* v = dynamic_cast<GeometryVisual*> (&view);
        if (v == nullptr) return;

        v->setSmoothAmount ((float) panel.getProperty (AlterState::kSmooth, 0.15f));
        v->setZoom         ((float) panel.getProperty (AlterState::kZoom, 1.0f));
        v->setRotation       ((float) panel.getProperty (AlterState::kRotation, 0.0f));      // per-object spin
        v->setGlobalRotation ((float) panel.getProperty (AlterState::kGeoGlobalRot, 0.0f));  // whole-module rotation
        v->setSymmetry     ((int)   panel.getProperty (AlterState::kSymmetry, 6));
        v->setMirror       ((bool)  panel.getProperty (AlterState::kMirror, false));
        v->setSaturation   ((float) panel.getProperty (AlterState::kSaturation, 1.0f));
        v->setBrightness   ((float) panel.getProperty (AlterState::kSynBrightness, 1.0f)); // shared key (like kBloom)
        v->setBloom        ((float) panel.getProperty (AlterState::kBloom, 0.4f));
        v->setShaderSpeed  ((float) panel.getProperty (AlterState::kSpeed, 1.0f));
        v->setShapeWeights ((float) panel.getProperty (AlterState::kGeoTri, 0.5f),
                            (float) panel.getProperty (AlterState::kGeoSquare, 0.5f),
                            (float) panel.getProperty (AlterState::kGeoCircle, 0.5f));
        v->setComplexity   ((int)   panel.getProperty (AlterState::kGeoComplexity, 12));
        v->setRandomization ((float) panel.getProperty (AlterState::kGeoRandom, 1.0f));
        v->setReactivity   ((float) panel.getProperty (AlterState::kGeoReact, 0.0f));
        v->setDepth        ((float) panel.getProperty (AlterState::kGeoDepth, 0.7f));
        v->setTunnel       ((float) panel.getProperty (AlterState::kGeoTunnel, 0.0f));
        v->setAperture     ((float) panel.getProperty (AlterState::kGeoAperture, 0.8f));
        v->setBpmSync      ((bool)  panel.getProperty (AlterState::kGeoBpmSync, false));
        v->setBeatDivision ((int)   panel.getProperty (AlterState::kGeoBeatDiv, 5));
        v->setBpm          ((float) panel.getProperty (AlterState::kBpm, 100.0f));
        v->setToneDependent ((int)  panel.getProperty (AlterState::kColorMode, 1) == 1);
        v->setToneTwist     ((bool) panel.getProperty (AlterState::kToneTwist, false));
        v->setSubBinInterp  (true);   // always on
        if (panel.hasProperty (AlterState::kColor))
            v->setBaseColour (juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor)));
    }
    else if (type == "fusion")
    {
        auto* v = dynamic_cast<FusionVisual*> (&view);
        if (v == nullptr) return;

        // The layers themselves are not set here — they are panels, and which ones
        // belong to this Fusion is decided by layoutFusionLayers. What IS set
        // here is each slot's own settings, because those live on the Fusion.
        for (int i = 0; i < AlterState::kMaxFusionLayers; ++i)
        {
            FusionVisual::LayerSettings s;
            s.blend    = (int)   panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLayBlend,   i), 0);
            s.opacity  = (float) panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLayOpacity, i), 1.0f);
            // ROTATION belongs to the MODULE, not to the slot. A fusion layer is
            // never drawn in a rectangle of its own, so the AffineTransform that
            // turns an ordinary panel never reaches it and the shader has to do the
            // turn instead — but WHAT to turn by is still the module's own setting,
            // read off the module's own panel and edited in the module's own editor.
            const int fusionId = (int) panel.getProperty (AlterState::kId, -1);
            if (const int layerId = settings.fusionLayerIdInSlot (fusionId, i); layerId > 0)
                if (auto lp = settings.getPanelById (layerId); lp.isValid())
                    s.rot = juce::jlimit (0, 3, (int) lp.getProperty (AlterState::kRotationAngle, 0));
            s.scale    = (float) panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLayScale,   i), 1.0f);
            s.offX     = (float) panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLayOffX,    i), 0.0f);
            s.offY     = (float) panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLayOffY,    i), 0.0f);
            s.amount   = (float) panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLayAmount,  i), 0.5f);
            s.bands    = (int)   panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLayBands,   i), 8);
            s.angleDeg = (float) panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLayAngle,   i), 0.0f);
            s.edge     = (float) panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLayEdge,    i), 0.6f);

            // The layer's own post chain. Defaults are the inert ones, so a preset
            // saved before these existed loads as the picture it was saved as.
            s.mirror       = (int)   panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLayMirror,    i), 0);
            s.mirrorAngDeg = (float) panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLayMirrorAng, i), 0.0f);
            s.symmetry     = (int)   panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLaySymmetry,  i), 1);
            s.spinDeg      = (float) panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLaySpin,      i), 0.0f);
            s.speed        = (float) panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLaySpeed,     i), 0.0f);
            s.zoom         = (float) panel.getProperty (AlterState::fusionLayerProp (AlterState::kFusionLayZoom,      i), 1.0f);

            v->setLayerSettings (i, s);
        }

        v->setWarp ((bool)  panel.getProperty (AlterState::kFusionWarp,        false),
                    (float) panel.getProperty (AlterState::kFusionWarpAmt,     0.5f),
                    (float) panel.getProperty (AlterState::kFusionWarpSwirl,   0.6f),
                    (float) panel.getProperty (AlterState::kFusionWarpSmooth,  0.0f),
                    (int)   panel.getProperty (AlterState::kFusionWarpSrc,     0),
                    (float) panel.getProperty (AlterState::kFusionWarpDenoise, 0.0f));

        v->setSymmetry   ((int)   panel.getProperty (AlterState::kFusionSymmetry, 1));
        // A COUNT of reflection axes now, not a switch. An old preset stored a
        // bool here; var converts false to 0 and true to 1, which are exactly the
        // two values that meant the same thing before, so nothing needs migrating.
        v->setMirror     ((int)   panel.getProperty (AlterState::kFusionMirror,      0),
                          (float) panel.getProperty (AlterState::kFusionMirrorAngle, 0.0f));
        v->setSpin       ((float) panel.getProperty (AlterState::kFusionSpin,  0.0f));
        v->setZoom       ((float) panel.getProperty (AlterState::kFusionZoom,  1.0f));
        v->setVortex     ((float) panel.getProperty (AlterState::kFusionVortex, 0.0f));
        v->setAudioDrive ((float) panel.getProperty (AlterState::kFusionReact, 0.0f));
        v->setSpeed      ((float) panel.getProperty (AlterState::kSpeed,    0.0f));

        v->setGlobalLayers ((bool) panel.getProperty (AlterState::kFusionGlobL0, true),
                            (bool) panel.getProperty (AlterState::kFusionGlobL1, true),
                            (bool) panel.getProperty (AlterState::kFusionGlobL2, true));
        v->setLiquid ((bool)  panel.getProperty (AlterState::kFusionLiquid,        false),
                      (float) panel.getProperty (AlterState::kFusionLiquidAmt,     0.5f),
                      (float) panel.getProperty (AlterState::kFusionLiquidSmooth,  0.5f),
                      (float) panel.getProperty (AlterState::kFusionLiquidDenoise, 0.0f));
        v->setTunnel ((bool)  panel.getProperty (AlterState::kFusionTunnel, false));

        if (panel.hasProperty (AlterState::kColor))
            v->setBaseColour (juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor)));
    }
}

void MainComponent::forceRebuildAllModules()
{
    // Clear all slots and windows to force complete rebuild
    // This is necessary when audio source changes because visual modules
    // store audio source as reference that can't be changed after construction
    //
    // ORDER IS CRITICAL: RowWindows (torn-off blocks) and PanelWindows hold RAW
    // non-owning PanelHost pointers as child components. Destroying the slots
    // (and thus the hosts) FIRST left those windows pointing at freed memory —
    // their destructors then touched dangling hosts (use-after-free), which is
    // what froze the app when switching the audio input with a detached block.
    rowWindows.clear();   // dissolve torn-off blocks while their hosts are alive
    windows.clear();      // dissolve detached panel windows likewise
    slots.clear();        // NOW the hosts may die
    rebuildSlotsFromState();
}

// =======================================================
// Plugin control channel
// =======================================================
void MainComponent::pushControlRegistry()
{
    std::vector<ControlModuleDesc> reg;

    // read-only access: never create the node from inside a listener callback
    auto panels = settings.getTree().getChildWithName (AlterState::kPanelsNode);
    if (panels.isValid())
    {
        for (int i = 0; i < panels.getNumChildren(); ++i)
        {
            auto p = panels.getChild (i);
            const juce::String type = p.getProperty (AlterState::kType).toString();
            const int id = (int) p.getProperty (AlterState::kId);

            ControlModuleDesc d;
            d.id     = (juce::uint32) id;
            d.colour = p.hasProperty (AlterState::kColor)
                         ? (juce::uint32) (int) p.getProperty (AlterState::kColor)
                         : 0xff808080u;

            // The "Color..." picker travels as H/S/B so a DAW can automate it.
            const juce::Colour baseCol { (juce::uint32) d.colour };
            const float colHue = baseCol.getHue();
            const float colSat = baseCol.getSaturation();
            const float colBri = baseCol.getBrightness();

            if (type == "synesthesia")
            {
                d.type = AlterCtrl::TypeSynesthesia;
                d.name = "Synesthesia #" + juce::String (id);
                d.params = {
                    { AlterCtrl::SynSmooth,     (float)       p.getProperty (AlterState::kSmooth,     0.5f) },
                    { AlterCtrl::SynZoom,       (float)       p.getProperty (AlterState::kZoom,       1.0f) },
                    { AlterCtrl::SynRotation,   (float)       p.getProperty (AlterState::kRotation,   0.0f) },
                    { AlterCtrl::SynSymmetry,   (float) (int) p.getProperty (AlterState::kSymmetry,   1)    },
                    { AlterCtrl::SynSaturation, (float)       p.getProperty (AlterState::kSaturation, 1.0f) },
                    { AlterCtrl::SynBrightness, (float)       p.getProperty (AlterState::kSynBrightness, 1.0f) },
                    { AlterCtrl::SynBloom,      (float)       p.getProperty (AlterState::kBloom,      0.0f) },
                    { AlterCtrl::SynSpeed,      (float)       p.getProperty (AlterState::kSpeed,      1.0f) },
                    { AlterCtrl::SynReact,      (float)       p.getProperty (AlterState::kSynReact,   0.0f) },
                    { AlterCtrl::SynFragment,   (float)       p.getProperty (AlterState::kFragment,   0.0f) },
                    { AlterCtrl::SynTransmute,  (float)       p.getProperty (AlterState::kTransmute,  0.0f) },
                    { AlterCtrl::SynMirror,     (bool)        p.getProperty (AlterState::kMirror,     false) ? 1.0f : 0.0f },
                    { AlterCtrl::SynClear,      (float)       p.getProperty (AlterState::kClear,      0.0f) },
                    { AlterCtrl::SynDenoise,    (float)       p.getProperty (AlterState::kDenoise,    0.0f) },
                    { AlterCtrl::SynCurveSmooth,(float)       p.getProperty (AlterState::kCurveSmooth,0.0f) },
                    { AlterCtrl::SynBpmSync,    (bool)        p.getProperty (AlterState::kSynBpmSync, false) ? 1.0f : 0.0f },
                    { AlterCtrl::SynBpm,        (float)       p.getProperty (AlterState::kSynBpm,     120.0f) },
                    { AlterCtrl::SynBeatDiv,    (float) (int) p.getProperty (AlterState::kSynBeatDiv, 5)    },
                    { AlterCtrl::SynTone,       (float) (int) p.getProperty (AlterState::kColorMode,  0)    },
                    { AlterCtrl::SynTwist,      (bool)        p.getProperty (AlterState::kToneTwist, false) ? 1.0f : 0.0f },
                    { AlterCtrl::SynTunnel,     (float)       p.getProperty (AlterState::kSynTunnel,  0.0f) },
                    { AlterCtrl::SynVortex,     (float)       p.getProperty (AlterState::kSynVortex,  0.0f) },
                    { AlterCtrl::SynColorHue,   colHue },
                    { AlterCtrl::SynColorSat,   colSat },
                    { AlterCtrl::SynColorBri,   colBri }
                };
            }
            else if (type == "chladni")
            {
                d.type = AlterCtrl::TypeChladni;
                d.name = "Chladni #" + juce::String (id);
                d.params = {
                    { AlterCtrl::ChShift,      (float) (int) p.getProperty (AlterState::kChladniShift,     0)    },
                    { AlterCtrl::ChAspect,     (float)       p.getProperty (AlterState::kChladniAR,        1.0f) },
                    { AlterCtrl::ChSandSmooth, (float)       p.getProperty (AlterState::kChladniSharp,     0.5f) },
                    { AlterCtrl::ChParticles,  (float) (int) p.getProperty (AlterState::kChladniParticles, 5000) },
                    { AlterCtrl::ChMaterial,   (float) (int) p.getProperty (AlterState::kChladniMaterial,  1)    },
                    { AlterCtrl::ChReactive,   (bool)        p.getProperty (AlterState::kChladniReactive,  true) ? 1.0f : 0.0f },
                    { AlterCtrl::ChM,          (float) (int) p.getProperty (AlterState::kChladniM,         2)    },
                    { AlterCtrl::ChN,          (float) (int) p.getProperty (AlterState::kChladniN,         3)    },
                    { AlterCtrl::ChTone,       (float) (int) p.getProperty (AlterState::kColorMode,        1)    },
                    { AlterCtrl::ChTwist,      (bool)        p.getProperty (AlterState::kToneTwist,   false) ? 1.0f : 0.0f },
                    { AlterCtrl::ChPreset,     (float) AlterCtrl::chladniPresetIndex (
                                                   (int) p.getProperty (AlterState::kChladniM, 2),
                                                   (int) p.getProperty (AlterState::kChladniN, 3)) },
                    { AlterCtrl::ChColorHue,   colHue },
                    { AlterCtrl::ChColorSat,   colSat },
                    { AlterCtrl::ChColorBri,   colBri }
                };
            }
            else if (type == "geometry")
            {
                d.type = AlterCtrl::TypeGeometry;
                d.name = "Geometry #" + juce::String (id);
                d.params = {
                    { AlterCtrl::GeoSmooth,     (float)       p.getProperty (AlterState::kSmooth,        0.15f) },
                    { AlterCtrl::GeoZoom,       (float)       p.getProperty (AlterState::kZoom,          1.0f)  },
                    { AlterCtrl::GeoRotation,   (float)       p.getProperty (AlterState::kRotation,      0.0f)  },
                    { AlterCtrl::GeoSymmetry,   (float) (int) p.getProperty (AlterState::kSymmetry,      1)     },
                    { AlterCtrl::GeoMirror,     (bool)        p.getProperty (AlterState::kMirror,       false) ? 1.0f : 0.0f },
                    { AlterCtrl::GeoSaturation, (float)       p.getProperty (AlterState::kSaturation,    1.0f)  },
                    { AlterCtrl::GeoBrightness, (float)       p.getProperty (AlterState::kSynBrightness, 1.0f)  },
                    { AlterCtrl::GeoBloom,      (float)       p.getProperty (AlterState::kBloom,         0.2f)  },
                    { AlterCtrl::GeoSpeed,      (float)       p.getProperty (AlterState::kSpeed,         1.0f)  },
                    { AlterCtrl::GeoTri,        (float)       p.getProperty (AlterState::kGeoTri,        0.5f)  },
                    { AlterCtrl::GeoSquare,     (float)       p.getProperty (AlterState::kGeoSquare,     0.5f)  },
                    { AlterCtrl::GeoCircle,     (float)       p.getProperty (AlterState::kGeoCircle,     0.5f)  },
                    { AlterCtrl::GeoComplexity, (float) (int) p.getProperty (AlterState::kGeoComplexity, 12)    },
                    { AlterCtrl::GeoRandom,     (float)       p.getProperty (AlterState::kGeoRandom,     1.0f)  },
                    { AlterCtrl::GeoReact,      (float)       p.getProperty (AlterState::kGeoReact,      0.0f)  },
                    { AlterCtrl::GeoDepth,      (float)       p.getProperty (AlterState::kGeoDepth,      0.7f)  },
                    { AlterCtrl::GeoTone,       (float) (int) p.getProperty (AlterState::kColorMode,     0)     },
                    { AlterCtrl::GeoTwist,      (bool)        p.getProperty (AlterState::kToneTwist, false) ? 1.0f : 0.0f },
                    { AlterCtrl::GeoTunnel,     (float)       p.getProperty (AlterState::kGeoTunnel,     0.0f)  },
                    { AlterCtrl::GeoAperture,   (float)       p.getProperty (AlterState::kGeoAperture,   0.8f)  },
                    { AlterCtrl::GeoGlobalRot,  (float)       p.getProperty (AlterState::kGeoGlobalRot,  0.0f)  },
                    { AlterCtrl::GeoBpmSync,    (bool)        p.getProperty (AlterState::kGeoBpmSync,    false) ? 1.0f : 0.0f },
                    { AlterCtrl::GeoBpm,        (float)       p.getProperty (AlterState::kBpm,           100.0f) },
                    { AlterCtrl::GeoBeatDiv,    (float) (int) p.getProperty (AlterState::kGeoBeatDiv,    5)     },
                    { AlterCtrl::GeoColorHue,   colHue },
                    { AlterCtrl::GeoColorSat,   colSat },
                    { AlterCtrl::GeoColorBri,   colBri }
                };
            }
            else
                continue;   // only synesthesia, chladni & geometry are controllable

            reg.push_back (std::move (d));
        }
    }

    udpSource.setControlRegistry (std::move (reg));

    // Forward the active visual theme so control-mode plugins can match it. The
    // CUSTOM theme (index 2) is generated entirely from the user's two colours, so
    // the index on its own says nothing — send the pair alongside it.
    udpSource.setControlTheme ((int) settings.getTree().getProperty (AlterState::kTheme, 0));
    udpSource.setControlThemeColours (
        (juce::uint32) (int) settings.getTree().getProperty (AlterState::kThemeColor1, (int) 0xFF3D96E7),
        (juce::uint32) (int) settings.getTree().getProperty (AlterState::kThemeColor2, (int) 0xFF6902D6));

    computeAndPushNeeds();
}

// Tell each plugin instance which packet types are actually consumed, so unused
// data (and unused instances) stop streaming over UDP.
void MainComponent::setAlphaCaptureMode (bool on)
{
    // FusionVisual::setLayerMode already does exactly this, in the right order
    // (drop the background before going offscreen, restore it after coming back,
    // so the module is never transparent while still being drawn in the HUD).
    // Reusing it means alpha capture and Fusion layers can never drift apart.
    //
    // BUT layer mode is not ours alone to switch. A module a Fusion has borrowed
    // is ALREADY in it, for reasons that have nothing to do with recording, and
    // blindly clearing the flag afterwards would hand that module its background
    // back and drop it off the FBO path — quietly breaking the fusion, minutes
    // after the recording everyone has stopped thinking about. So we only ever
    // restore what we ourselves turned on, and leave everything else alone.
    // Every one of these switches goes through AlterGLHost::addSource/removeSource,
    // which block on a lock the GL thread holds for its WHOLE frame. Cheap when a
    // frame is 16 ms; the difference between "works" and "the app is hung" when a
    // frame is half a second. So each one is timed and a slow one says which module
    // it was — if a freeze comes back, the log names it instead of us guessing.
    auto switchOne = [] (juce::Component& c, bool layer)
    {
       #if JUCE_DEBUG
        const double t0 = juce::Time::getMillisecondCounterHiRes();
       #endif
        FusionVisual::setLayerMode (c, layer);
       #if JUCE_DEBUG
        if (const double ms = juce::Time::getMillisecondCounterHiRes() - t0; ms > 20.0)
            DBG ("ALTER ALPHA: switching " << c.getName() << " took "
                 // Plain hyphen, not an em dash: this is a const char* going into a
                 // juce::String, which asserts on any byte above 127 — and it would
                 // fire in exactly the debug session where this warning is what you
                 // came to read.
                 << juce::String (ms, 1) << " ms - blocked on the GL thread");
       #endif
    };

    if (on)
    {
        alphaTouched.clear();

        for (auto& s : slots)
        {
            if (s.host == nullptr) continue;
            auto* inner = s.host->getInner();
            if (inner == nullptr) continue;

            // A module a Fusion has borrowed is ALREADY in layer mode, and it is
            // not drawn in the HUD either — the fusion is what shows it. Switching
            // it would buy a second full offscreen render and glReadPixels every
            // frame for pixels nobody looks at, on top of the fusion's own.
            if (auto* t = dynamic_cast<ThemedBackground*> (inner))
                if (t->isTransparentBackground())
                    continue;

            // Hidden modules are not in the frame either, for the same reason.
            if (! inner->isVisible())
                continue;

            switchOne (*inner, true);
            alphaTouched.push_back (inner);
        }

        DBG ("ALTER ALPHA: capture ON, " << (int) alphaTouched.size()
             << " of " << (int) slots.size() << " modules switched");

       #if JUCE_DEBUG
        // WHERE each module will be painted, in the HUD's own coordinates. If two
        // of these rectangles overlap, the export cannot help but overlap too — and
        // it says so here rather than after a minute of recording and an import.
        {
            std::vector<std::pair<juce::String, juce::Rectangle<int>>> placed;

            for (auto& s : slots)
            {
                auto* inner = s.host != nullptr ? s.host->getInner() : nullptr;
                if (inner == nullptr || ! inner->isVisible()) continue;

                // What is actually VISIBLE of this module, after every ancestor has
                // had its say — the same rectangle grabAlphaFrame paints into. The
                // module's own bounds are bigger on purpose and overlapping those
                // means nothing.
                auto vis = inner->getLocalBounds();
                for (auto* c = inner; c != nullptr && c != this; c = c->getParentComponent())
                    if (auto* parent = c->getParentComponent())
                        vis = vis.getIntersection (inner->getLocalArea (parent,
                                                      parent->getLocalBounds()));

                if (vis.isEmpty()) continue;
                const auto r = getLocalArea (inner, vis);
                const auto name = s.node.getProperty (AlterState::kType).toString()
                                    + " " + juce::String ((int) s.id);

                // Only shout about an overlap worth seeing. Neighbouring panels
                // routinely share an edge and the rectangles come out of integer
                // rounding, so a pixel or two of overlap is the normal case — and a
                // warning that fires every time teaches you to ignore it.
                for (const auto& prev : placed)
                {
                    const auto common = prev.second.getIntersection (r);
                    if (common.getWidth() > 4 && common.getHeight() > 4)
                        DBG ("ALTER ALPHA: *** OVERLAP *** " << name << " " << r.toString()
                             << "  vs  " << prev.first << " " << prev.second.toString()
                             << "  (" << common.toString() << ")");
                }

                DBG ("ALTER ALPHA: place " << name << " -> " << r.toString());
                placed.push_back ({ name, r });
            }
        }
       #endif
    }
    else
    {
        for (auto& c : alphaTouched)
            if (auto* inner = c.getComponent())
                switchOne (*inner, false);

        alphaTouched.clear();
        DBG ("ALTER ALPHA: capture OFF");
    }
}

std::vector<MainComponent::ExportableModule> MainComponent::getExportableModules() const
{
    std::vector<ExportableModule> out;

    for (auto& s : slots)
    {
        auto* inner = s.host != nullptr ? s.host->getInner() : nullptr;
        if (inner == nullptr || ! inner->isVisible() || inner->getWidth() < 2)
            continue;

        // A module a Fusion has borrowed is not laid out in the HUD and is not a
        // picture of its own — it is one ingredient of the fusion, which exports as
        // a single module. Exporting it separately would hand back a fragment
        // nobody asked for, so the same test that decides "is this somebody's
        // layer" decides "is this exportable".
        if (auto* t = dynamic_cast<const ThemedBackground*> (inner))
            if (t->isTransparentBackground())
                continue;

        ExportableModule m;
        m.view = inner;
        m.name = s.node.getProperty (AlterState::kType).toString();
        if (m.name.isEmpty()) m.name = "module";
        m.name += " " + juce::String ((int) s.id);
        m.audioInstance = (juce::uint32) (juce::int64)
                              s.node.getProperty (AlterState::kAudioInstance, 0);

        // Physical pixels, like everywhere else the recorder is told about a size.
        double scale = 1.0;
        if (auto* d = juce::Desktop::getInstance().getDisplays()
                          .getDisplayForRect (inner->getScreenBounds()))
            scale = d->scale;

        const auto r = inner->getScreenBounds();
        m.screenArea = juce::Rectangle<int> (juce::roundToInt (r.getX()     * scale),
                                             juce::roundToInt (r.getY()     * scale),
                                             juce::roundToInt (r.getWidth() * scale),
                                             juce::roundToInt (r.getHeight()* scale));
        out.push_back (m);
    }

    return out;
}

juce::Image MainComponent::grabAlphaFrame (juce::Component* onlyThis, int outW, int outH)
{
   #if JUCE_DEBUG
    const double t0 = juce::Time::getMillisecondCounterHiRes();
   #endif

    // `true` clears the image to transparent black. Everything the modules do not
    // cover therefore stays genuinely empty rather than becoming black pixels with
    // alpha 255 — the difference between a usable key and a black rectangle.
    // One module on its own is drawn in ITS bounds, not in a HUD-sized canvas with
    // everything else left transparent — an editor wants a clip the size of the
    // picture, not a mostly-empty frame it has to crop.
    const int srcW = juce::jmax (1, onlyThis != nullptr ? onlyThis->getWidth()  : getWidth());
    const int srcH = juce::jmax (1, onlyThis != nullptr ? onlyThis->getHeight() : getHeight());

    const int w = outW >= 2 ? outW : srcW;
    const int h = outH >= 2 ? outH : srcH;

    juce::Image img (juce::Image::ARGB, juce::jmax (2, w), juce::jmax (2, h), true);
    juce::Graphics g (img);

    // Scale during the paint, not after it. The modules mostly blit a cached frame,
    // so drawing them smaller is genuinely less work — where a full-size paint plus
    // a high-quality resample was two passes over the larger of the two sizes.
    const float sx = (float) w / (float) srcW;
    const float sy = (float) h / (float) srcH;

    // ONE combined transform per module, never a global scale plus setOrigin.
    //
    // setOrigin composes with whatever transform is already in place, and which
    // side it composes on decides whether a module's position is scaled with
    // everything else or applied raw. Get that backwards and every module lands at
    // its unscaled offset — correct at the top left, drifting further out the
    // further across the HUD it sits, which is exactly the "it shifts" symptom.
    // Building the transform explicitly leaves nothing to compose wrongly.
    auto placeFor = [sx, sy] (juce::Point<int> origin)
    {
        return juce::AffineTransform::translation ((float) origin.x, (float) origin.y)
                   .followedBy (juce::AffineTransform::scale (sx, sy));
    };

    if (onlyThis != nullptr)
    {
        if (onlyThis->isVisible() && onlyThis->getWidth() > 0)
        {
            juce::Graphics::ScopedSaveState keep (g);
            g.addTransform (placeFor ({ 0, 0 }));
            g.reduceClipRegion (0, 0, onlyThis->getWidth(), onlyThis->getHeight());
            onlyThis->paintEntireComponent (g, false);
        }

        return img;
    }

    // Only the module views are drawn: PanelHost::paintOverChildren (frames,
    // labels, the HUD's own chrome) is deliberately skipped, because what this
    // export is for is the visual on its own, ready to sit over other footage.
    for (auto& s : slots)
    {
        auto* inner = s.host != nullptr ? s.host->getInner() : nullptr;
        if (inner == nullptr || ! inner->isVisible() || inner->getWidth() < 1)
            continue;

        const auto origin = getLocalPoint (inner, juce::Point<int> (0, 0));
        juce::Graphics::ScopedSaveState keep (g);
        g.addTransform (placeFor (origin));

        // CLIP THE WAY THE REAL PAINT DOES: through every ancestor.
        //
        // Two earlier attempts got this wrong for the same reason — each guessed at
        // ONE rectangle. Clipping to the module hid nothing, because modules are
        // deliberately larger than their slot (the placement log shows meters 122 px
        // tall on a 95 px pitch) and overscan so glow and bloom have room. Clipping
        // to the panel then hid whole modules, because a module's view is not always
        // a child of its panel, and for those the panel's rectangle sits somewhere
        // else entirely.
        //
        // There is no single rectangle to pick. When JUCE paints the hierarchy it
        // intersects the clip with EVERY ancestor on the way down, and that is what
        // decides how much of a module is ever visible. So walk the same chain.
        {
            auto visible = inner->getLocalBounds();

            for (auto* c = inner; c != nullptr && c != this; c = c->getParentComponent())
                if (auto* parent = c->getParentComponent())
                    visible = visible.getIntersection (inner->getLocalArea (parent,
                                                          parent->getLocalBounds()));

            if (visible.isEmpty()) continue;      // nothing of it was ever on screen
            g.reduceClipRegion (visible);
        }

        inner->paintEntireComponent (g, false);
    }

   #if JUCE_DEBUG
    // This runs on the message thread 30 times a second, so it has a hard budget:
    // much past ~15 ms and the UI starts to feel dead even though nothing is
    // actually blocked. Reported once a second, not per frame.
    {
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        alphaGrabMs += ms;
        ++alphaGrabCount;

        if (const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
            now - alphaGrabLogSec > 1.0)
        {
            alphaGrabLogSec = now;
            DBG ("ALTER ALPHA: grab " << juce::String (alphaGrabMs / juce::jmax (1, alphaGrabCount), 1)
                 << " ms/frame over " << alphaGrabCount << " frames, "
                 << img.getWidth() << "x" << img.getHeight());
            alphaGrabMs = 0.0;
            alphaGrabCount = 0;
        }
    }
   #endif

    return img;
}

void MainComponent::computeAndPushNeeds()
{
    // Fusion asks for nothing itself: its layers are ordinary panels in this same
    // list, so their needs are already counted where they stand.
    // COLOUR BY TONE IS AN FFT CONSUMER, whatever else the module is.
    //
    // The pitch does not come from what the module already receives — a meter gets
    // RMS/peak/LUFS and a scope gets the waveform, and neither of those contains a
    // note. PitchUtils::ToneHueTracker reads getLastFft(), so a module in tone mode
    // has to ASK for the FFT stream or the tracker sees zero bins on every frame,
    // never updates, and reports its initial hue forever: a scope drawn in flat red
    // and a meter whose bar is red at every level. Which is exactly what "colour by
    // tone does not work" looked like.
    //
    // Conditional, not unconditional: the FFT is the most expensive stream a plugin
    // sends, and a meter that is not in tone mode has no use for it.
    auto toneNeedsFft = [] (const juce::String& t, const juce::ValueTree& p) -> bool
    {
        if (t == "rms" || t == "audiometer")
            return (bool) p.getProperty (AlterState::kMeterToneColor, false);
        if (t == "oscillator" || t == "oscilator" || t == "stereoscope")
            return (int) p.getProperty (AlterState::kColorMode, 0) == 1;
        return false;   // spectrum / spectrogram / toneanalyzer already ask for it
    };

    auto needsForType = [&toneNeedsFft] (const juce::String& t, const juce::ValueTree& p) -> juce::uint8
    {
        using N = UdpReceiver;

        const juce::uint8 tone = toneNeedsFft (t, p) ? (juce::uint8) N::NeedFft : (juce::uint8) 0;

        if (t == "rms" || t == "audiometer")  return (juce::uint8) (N::NeedRms | N::NeedPeak | N::NeedLufs | tone
                                                     | ((int) p.getProperty (AlterState::kMeterMode, 0) == 3 ? N::NeedWave : 0));
        if (t == "spectrum")                  return (juce::uint8) (N::NeedFft
                                                     | ((bool) p.getProperty (AlterState::kSpecStereo, false) ? N::NeedWave : 0)
                                                     | ((bool) p.getProperty (AlterState::kConstantQ,  false) ? N::NeedCqt  : 0));
        if (t == "spectrogram")               return (juce::uint8) (N::NeedFft
                                                     | ((bool) p.getProperty (AlterState::kConstantQ,      false) ? N::NeedCqt  : 0)
                                                     | ((bool) p.getProperty (AlterState::kSpectroReassign, false) ? N::NeedWave : 0));
        if (t == "toneanalyzer")              return (juce::uint8) N::NeedFft;
        if (t == "oscillator" || t == "oscilator") return (juce::uint8) (N::NeedWave | tone);
        if (t == "stereoscope")               return (juce::uint8) (N::NeedWave | tone);
        if (t == "chladni")                   return (juce::uint8) (N::NeedRms | N::NeedFft);
        if (t == "synesthesia")               return (juce::uint8) (N::NeedRms | N::NeedFft);
        if (t == "geometry")                  return (juce::uint8) (N::NeedRms | N::NeedFft);
        return 0;
    };

    juce::uint8 globalNeeds = 0;
    std::map<juce::uint32, juce::uint8> overrideNeeds;

    auto panels = settings.getPanelsRoot();
    for (int i = 0; i < panels.getNumChildren(); ++i)
    {
        auto p = panels.getChild (i);
        if ((bool) p.getProperty (AlterState::kHidden, false)) continue;   // hidden -> not visualising -> no data

        const juce::uint8 n = needsForType (p.getProperty (AlterState::kType).toString(), p);
        if (n == 0) continue;

        const auto inst = (juce::uint32) (juce::int64) p.getProperty (AlterState::kAudioInstance, 0);
        if (inst == 0) globalNeeds = (juce::uint8) (globalNeeds | n);
        else           overrideNeeds[inst] = (juce::uint8) (overrideNeeds[inst] | n);
    }

    if (settings.globalCqt())
        globalNeeds = (juce::uint8) (globalNeeds | UdpReceiver::NeedCqt);   // global Constant-Q

    udpSource.setInstanceNeeds (globalNeeds, std::move (overrideNeeds));

    // Global FFT resolution → plugins (via 'B') + local system-audio capture.
    const int order = AlterState::fftOrderForBins (settings.maxFftBins());
    udpSource.setFftOrder (order);
    if (systemAudioSource != nullptr)
    {
        systemAudioSource->setFftOrder (order);
        systemAudioSource->setCqtNeeded ((globalNeeds & UdpReceiver::NeedCqt) != 0);   // local CQT on demand

        // TRUE stereo Constant-Q (per-channel L/R analysis) costs extra CPU, so it
        // runs only while some Spectrum actually shows the stereo overlay in CQ mode.
        bool stereoCqt = false;
        {
            auto ps = settings.getPanelsRoot();
            if (ps.isValid())
                for (int i = 0; i < ps.getNumChildren(); ++i)
                {
                    auto p = ps.getChild (i);
                    if (p.getProperty (AlterState::kType).toString() == "spectrum"
                        && (bool) p.getProperty (AlterState::kSpecStereo, false)
                        && ((bool) p.getProperty (AlterState::kConstantQ, false) || settings.globalCqt()))
                        { stereoCqt = true; break; }
                }
        }
        systemAudioSource->setCqtStereoNeeded (stereoCqt);
    }
}

void MainComponent::applyControlValue (juce::uint32 moduleId, juce::uint8 paramId, float value)
{
    auto panel = settings.getPanelById ((int) moduleId);
    if (! panel.isValid()) return;

    const juce::String type = panel.getProperty (AlterState::kType).toString();
    const bool isSyn     = (type == "synesthesia");
    const bool isChladni = (type == "chladni");
    const bool isGeo     = (type == "geometry");
    juce::UndoManager* undo = nullptr;

    // ── base colour ("Color..." picker) ────────────────────────────────────────
    // Hue/Sat/Bright arrive as three independent params, so each one is a
    // read-modify-write on the packed ARGB colour the module already stores.
    // component: 0 = hue, 1 = saturation, 2 = brightness.
    auto setColourComponent = [&panel, undo] (int component, float v)
    {
        const juce::Colour cur { panel.hasProperty (AlterState::kColor)
                                    ? (juce::uint32) (int) panel.getProperty (AlterState::kColor)
                                    : 0xff808080u };
        float h = cur.getHue(), s = cur.getSaturation(), b = cur.getBrightness();
        v = juce::jlimit (0.0f, 1.0f, v);
        if      (component == 0) h = v;
        else if (component == 1) s = v;
        else                     b = v;

        // HSB is packed back into an ARGB int, and a fully grey/black colour has
        // no hue to read back — automating Sat or Bright through 0 would silently
        // reset the hue to red. A 1/255 floor keeps the round-trip stable while
        // staying visually identical to pure grey/black.
        const float floorV = 1.0f / 255.0f;
        const auto next = juce::Colour::fromHSV (h, juce::jmax (s, floorV),
                                                 juce::jmax (b, floorV), 1.0f);
        panel.setProperty (AlterState::kColor, (int) next.getARGB(), undo);
    };

    switch (paramId)
    {
        case AlterCtrl::SynSmooth:     if (isSyn) panel.setProperty (AlterState::kSmooth,     value, undo); break;
        case AlterCtrl::SynZoom:       if (isSyn) panel.setProperty (AlterState::kZoom,       value, undo); break;
        case AlterCtrl::SynRotation:   if (isSyn) panel.setProperty (AlterState::kRotation,   value, undo); break;
        case AlterCtrl::SynSymmetry:   if (isSyn) panel.setProperty (AlterState::kSymmetry,   (int) std::lround (value), undo); break;
        case AlterCtrl::SynSaturation: if (isSyn) panel.setProperty (AlterState::kSaturation, value, undo); break;
        case AlterCtrl::SynBrightness: if (isSyn) panel.setProperty (AlterState::kSynBrightness, value, undo); break;
        case AlterCtrl::SynBloom:      if (isSyn) panel.setProperty (AlterState::kBloom,      value, undo); break;
        case AlterCtrl::SynSpeed:      if (isSyn) panel.setProperty (AlterState::kSpeed,      value, undo); break;
        case AlterCtrl::SynReact:      if (isSyn) panel.setProperty (AlterState::kSynReact,   value, undo); break;
        case AlterCtrl::SynFragment:   if (isSyn) panel.setProperty (AlterState::kFragment,   value, undo); break;
        case AlterCtrl::SynTransmute:  if (isSyn) panel.setProperty (AlterState::kTransmute,  value, undo); break;
        case AlterCtrl::SynMirror:     if (isSyn) panel.setProperty (AlterState::kMirror,     value >= 0.5f, undo); break;
        case AlterCtrl::SynClear:      if (isSyn) panel.setProperty (AlterState::kClear,      value, undo); break;
        case AlterCtrl::SynDenoise:    if (isSyn) panel.setProperty (AlterState::kDenoise,    value, undo); break;
        case AlterCtrl::SynCurveSmooth:if (isSyn) panel.setProperty (AlterState::kCurveSmooth,value, undo); break;
        case AlterCtrl::SynBpmSync:    if (isSyn) panel.setProperty (AlterState::kSynBpmSync, value >= 0.5f, undo); break;
        case AlterCtrl::SynBpm:        if (isSyn) panel.setProperty (AlterState::kSynBpm,     value, undo); break;
        case AlterCtrl::SynBeatDiv:    if (isSyn) panel.setProperty (AlterState::kSynBeatDiv, (int) std::lround (value), undo); break;
        case AlterCtrl::SynTone:       if (isSyn) panel.setProperty (AlterState::kColorMode,  (int) std::lround (value), undo); break;
        case AlterCtrl::SynTwist:      if (isSyn) panel.setProperty (AlterState::kToneTwist,  value >= 0.5f, undo); break;
        case AlterCtrl::SynTunnel:     if (isSyn) panel.setProperty (AlterState::kSynTunnel,  value, undo); break;
        case AlterCtrl::SynVortex:     if (isSyn) panel.setProperty (AlterState::kSynVortex,  value, undo); break;
        case AlterCtrl::SynColorHue:   if (isSyn) setColourComponent (0, value); break;
        case AlterCtrl::SynColorSat:   if (isSyn) setColourComponent (1, value); break;
        case AlterCtrl::SynColorBri:   if (isSyn) setColourComponent (2, value); break;

        case AlterCtrl::ChShift:       if (isChladni) panel.setProperty (AlterState::kChladniShift,     (int) std::lround (value), undo); break;
        case AlterCtrl::ChAspect:      if (isChladni) panel.setProperty (AlterState::kChladniAR,        value, undo); break;
        case AlterCtrl::ChSandSmooth:  if (isChladni) panel.setProperty (AlterState::kChladniSharp,     value, undo); break;
        case AlterCtrl::ChParticles:   if (isChladni) panel.setProperty (AlterState::kChladniParticles, (int) std::lround (value), undo); break;
        case AlterCtrl::ChMaterial:    if (isChladni) panel.setProperty (AlterState::kChladniMaterial,  (int) std::lround (value), undo); break;
        case AlterCtrl::ChReactive:    if (isChladni) panel.setProperty (AlterState::kChladniReactive,  value >= 0.5f, undo); break;
        case AlterCtrl::ChM:           if (isChladni) panel.setProperty (AlterState::kChladniM,         (int) std::lround (value), undo); break;
        case AlterCtrl::ChN:           if (isChladni) panel.setProperty (AlterState::kChladniN,         (int) std::lround (value), undo); break;
        case AlterCtrl::ChTone:        if (isChladni) panel.setProperty (AlterState::kColorMode,        (int) std::lround (value), undo); break;
        case AlterCtrl::ChTwist:       if (isChladni) panel.setProperty (AlterState::kToneTwist,        value >= 0.5f, undo); break;
        case AlterCtrl::ChColorHue:    if (isChladni) setColourComponent (0, value); break;
        case AlterCtrl::ChColorSat:    if (isChladni) setColourComponent (1, value); break;
        case AlterCtrl::ChColorBri:    if (isChladni) setColourComponent (2, value); break;
        case AlterCtrl::ChPreset:
            // 0 = Custom (leave m/n alone); 1..12 pick a classic figure and, like
            // the controller's combo, switch the module to manual (non-reactive).
            if (isChladni)
            {
                const int sel = (int) std::lround (value);
                if (sel >= 1 && sel <= 12)
                {
                    panel.setProperty (AlterState::kChladniReactive, false, undo);
                    panel.setProperty (AlterState::kChladniM, AlterCtrl::kChladniPresets[sel - 1][0], undo);
                    panel.setProperty (AlterState::kChladniN, AlterCtrl::kChladniPresets[sel - 1][1], undo);
                }
            }
            break;

        case AlterCtrl::GeoSmooth:     if (isGeo) panel.setProperty (AlterState::kSmooth,        value, undo); break;
        case AlterCtrl::GeoZoom:       if (isGeo) panel.setProperty (AlterState::kZoom,          value, undo); break;
        case AlterCtrl::GeoRotation:   if (isGeo) panel.setProperty (AlterState::kRotation,      value, undo); break;
        case AlterCtrl::GeoSymmetry:   if (isGeo) panel.setProperty (AlterState::kSymmetry,      (int) std::lround (value), undo); break;
        case AlterCtrl::GeoMirror:     if (isGeo) panel.setProperty (AlterState::kMirror,        value >= 0.5f, undo); break;
        case AlterCtrl::GeoSaturation: if (isGeo) panel.setProperty (AlterState::kSaturation,    value, undo); break;
        case AlterCtrl::GeoBloom:      if (isGeo) panel.setProperty (AlterState::kBloom,         value, undo); break;
        case AlterCtrl::GeoBrightness: if (isGeo) panel.setProperty (AlterState::kSynBrightness, value, undo); break;
        case AlterCtrl::GeoSpeed:      if (isGeo) panel.setProperty (AlterState::kSpeed,         value, undo); break;
        case AlterCtrl::GeoTri:        if (isGeo) panel.setProperty (AlterState::kGeoTri,        value, undo); break;
        case AlterCtrl::GeoSquare:     if (isGeo) panel.setProperty (AlterState::kGeoSquare,     value, undo); break;
        case AlterCtrl::GeoCircle:     if (isGeo) panel.setProperty (AlterState::kGeoCircle,     value, undo); break;
        case AlterCtrl::GeoComplexity: if (isGeo) panel.setProperty (AlterState::kGeoComplexity, (int) std::lround (value), undo); break;
        case AlterCtrl::GeoRandom:     if (isGeo) panel.setProperty (AlterState::kGeoRandom,     value, undo); break;
        case AlterCtrl::GeoReact:      if (isGeo) panel.setProperty (AlterState::kGeoReact,      value, undo); break;
        case AlterCtrl::GeoDepth:      if (isGeo) panel.setProperty (AlterState::kGeoDepth,      value, undo); break;
        case AlterCtrl::GeoTone:       if (isGeo) panel.setProperty (AlterState::kColorMode,     (int) std::lround (value), undo); break;
        case AlterCtrl::GeoTwist:      if (isGeo) panel.setProperty (AlterState::kToneTwist,     value >= 0.5f, undo); break;
        case AlterCtrl::GeoTunnel:     if (isGeo) panel.setProperty (AlterState::kGeoTunnel,     value, undo); break;
        case AlterCtrl::GeoAperture:   if (isGeo) panel.setProperty (AlterState::kGeoAperture,   value, undo); break;
        case AlterCtrl::GeoGlobalRot:  if (isGeo) panel.setProperty (AlterState::kGeoGlobalRot,  value, undo); break;
        case AlterCtrl::GeoBpmSync:    if (isGeo) panel.setProperty (AlterState::kGeoBpmSync,    value >= 0.5f, undo); break;
        case AlterCtrl::GeoBpm:        if (isGeo) panel.setProperty (AlterState::kBpm,           value, undo); break;
        case AlterCtrl::GeoBeatDiv:    if (isGeo) panel.setProperty (AlterState::kGeoBeatDiv,    (int) std::lround (value), undo); break;
        case AlterCtrl::GeoColorHue:   if (isGeo) setColourComponent (0, value); break;
        case AlterCtrl::GeoColorSat:   if (isGeo) setColourComponent (1, value); break;
        case AlterCtrl::GeoColorBri:   if (isGeo) setColourComponent (2, value); break;

        default: break;
    }
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
    auto* leftSlot  = findSlotById (leftId);
    auto* rightSlot = findSlotById (rightId);
    if (!leftSlot || !rightSlot) return;

    // Hand-accurate: the divider sets EXACT pixel widths of the two adjacent
    // columns (they become fixed-width). No ratio renormalisation – nothing
    // else moves, the edge follows the mouse 1:1.
    applyColumnWidth (leftId,  leftWidth);
    applyColumnWidth (rightId, rightWidth);

    resized();
}

// Trailing-edge grip: the panel has no right-hand neighbour to trade pixels
// with, so it simply takes (or gives back) the row's free space.
void MainComponent::setModuleWidth (int panelId, int width)
{
    applyColumnWidth (panelId, width);
    resized();
}

// One width per COLUMN: a module stacked under another shares its horizontal
// slot, so a width written for any member is written for all of them. Without
// this a stack would disagree with itself about how wide the column is and the
// layout would pick whichever member it happened to read first.
void MainComponent::applyColumnWidth (int panelId, int width)
{
    const int w = juce::jmax (30, width);

    for (int id : panelIdsInColumnOf (panelId))
        if (auto node = settings.getPanelById (id); node.isValid())
            node.setProperty (AlterState::kPreferredWidth, w, nullptr);
}

// Vertical split inside one column, in the same "the grabbed edge follows the
// mouse" spirit as the horizontal divider: the pair's combined height is kept,
// only the share moves.
void MainComponent::setStackHeights (int upperId, int upperHeight, int lowerId, int lowerHeight)
{
    auto upper = settings.getPanelById (upperId);
    auto lower = settings.getPanelById (lowerId);
    if (! upper.isValid() || ! lower.isValid()) return;

    const float uw = (float) upper.getProperty (AlterState::kStackWeight, 1.0f);
    const float lw = (float) lower.getProperty (AlterState::kStackWeight, 1.0f);
    const float pairWeight = juce::jmax (0.01f, uw + lw);
    const int   pairPix    = juce::jmax (1, upperHeight + lowerHeight);

    const float newUpper = juce::jlimit (0.12f * pairWeight, 0.88f * pairWeight,
                                         pairWeight * (float) upperHeight / (float) pairPix);

    // No resized() here: the property write already re-enters the layout through
    // the ValueTree listener, and doing it twice per drag frame is pure waste.
    upper.setProperty (AlterState::kStackWeight, newUpper, nullptr);
    lower.setProperty (AlterState::kStackWeight, pairWeight - newUpper, nullptr);
}

void MainComponent::rebuildResizeDividers (const std::vector<std::pair<int,int>>& pairs)
{
    for (auto& divider : resizeDividers)
        removeChildComponent (divider.get());
    resizeDividers.clear();

    for (const auto& pr : pairs)
    {
        auto divider = std::make_unique<ResizeDivider> (*this, pr.first, pr.second);
        addAndMakeVisible (*divider);
        resizeDividers.push_back (std::move (divider));
    }

    dividerPairs = pairs;
}
