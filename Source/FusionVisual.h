/*
  ==============================================================================

    FusionVisual.h

    FUSION — a COMPOSITOR. It owns a stack of layers and a chain of post effects,
    and nothing else: it renders no pixels of its own and holds no picture.

    A layer is not a copy of a module, it IS one of the modules already in the
    HUD: it keeps its own node in the state tree, its own settings and its own
    render engine, it just stops being laid out on its own and lends its frames
    here instead.

    ── Where the pixels come from ─────────────────────────────────────────────
    Nowhere near the CPU. The fusion is a fragment shader in AlterGLHost (see
    FusionShader.h), evaluated per output pixel at NATIVE resolution. This class is
    only the plumbing:

      * it borrows the layer components and keeps them rendering,
      * on the GL thread it uploads each layer's finished frame into a texture,
        and only when that layer has actually produced a new one,
      * it hands the host a block of parameters.

    ── Uploads ────────────────────────────────────────────────────────────────
    A layer's frame is read through readFrontFrame / readOffscreenFrame, which
    hold the layer's own lock for the duration of the upload. Handing the image
    out instead would be a race: it is reference counted, so the pixels would stay
    alive, but the layer's worker would go on rendering into them and the upload
    would tear. Both accessors carry a frame generation, so a layer running at 30
    fps is not uploaded 60 times a second.

    ── Why the offscreen toggle is deferred ───────────────────────────────────
    Putting a GL module into offscreen mode goes through AlterGLHost::addSource /
    removeSource, and those block on a lock the GL thread holds for its WHOLE
    frame. Doing that from inside a layout pass — which is where setLayerSources
    used to be called from, synchronously, out of a ValueTree callback — is a
    deadlock waiting for a bad moment, and it found one. The switching is now
    posted to the message queue instead, so the layout finishes first and nothing
    is ever blocked mid-layout.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <vector>

#include "AlterTheme.h"
#include "AudioSourceInterface.h"
#include "FusionShader.h"
#include "AlterGLHost.h"

class FusionVisual : public juce::Component,
                      public  IFusionShaderSource,
                      public  ThemedBackground,   // so a fusion can be told to drop its own background too
                      private juce::Timer
{
public:
    static constexpr int kMaxLayers = IFusionShaderSource::kMaxLayers;

    /** One layer's settings, as the controller sets them. Mirrors FusionLayerParams
        but in the units a human dials in (degrees, not radians). */
    struct LayerSettings
    {
        int   blend   = 0;      // FusionBlend
        float opacity = 1.0f;
        int   rot     = 0;      // quarter turns
        float scale   = 1.0f;
        float offX    = 0.0f;
        float offY    = 0.0f;
        float amount  = 0.5f;   // merge bias / weave share
        int   bands   = 8;
        float angleDeg= 0.0f;
        float edge    = 0.6f;

        // This layer's OWN post chain — the same stages the whole stack gets
        // globally, asked once more of this layer alone. See FusionShader.h.
        int   mirror        = 0;      // reflection axes, 0 = none
        float mirrorAngDeg  = 0.0f;
        int   symmetry      = 1;      // wedges, 1 = no fold
        float spinDeg       = 0.0f;
        float speed         = 0.0f;   // drift, 0 = stands still
        float zoom          = 1.0f;
    };

    explicit FusionVisual (IAudioSource& src)
        : audioSource (src)
    {
        // Transparent and paints nothing: the GL host draws this module's picture
        // straight into its rectangle, exactly as it does for Synesthesia.
        setOpaque (false);
        startTimerHz (60);          // placement snapshot + audio follow
    }

    ~FusionVisual() override
    {
        stopTimer();

        // Leave the registry first — removeSource blocks until the GL thread has
        // finished any frame that could still be touching us — and only then hand
        // the textures over for deletion.
        if (currentHost != nullptr)
        {
            currentHost->removeSource (static_cast<IFusionShaderSource*> (this));
            for (auto& t : layerTex)
            {
                currentHost->scheduleTextureDelete (t);
                t = 0;
            }
            currentHost = nullptr;
        }

        releaseAllLayers();
    }

    // ── layers (message thread) ─────────────────────────────────────────────

    /** The modules this Fusion composites, bottom-up, at most kMaxLayers.

        BORROWED, never owned. MainComponent decides which panels these are and
        keeps them parented and rendering; all this class does is read frames off
        them.

        Index 0 is the BASE — what everything else is composited onto. Its own
        blend mode is never consulted, because there is nothing underneath it. */
    void setLayerSources (const std::vector<juce::Component*>& views)
    {
        std::vector<juce::Component*> leaving, arriving;

        for (int i = 0; i < kMaxLayers; ++i)
        {
            auto* wanted = (i < (int) views.size()) ? views[(size_t) i] : nullptr;
            auto* have   = layer[(size_t) i].getComponent();

            if (wanted == have)
                continue;

            // A module leaving goes back to rendering normally — unless it merely
            // moved to another slot of this same Fusion, which is why the whole
            // new list is checked and not just this slot.
            if (have != nullptr
                && std::find (views.begin(), views.end(), have) == views.end())
                leaving.push_back (have);

            layer[(size_t) i] = wanted;
            layerGen[(size_t) i] = 0;      // force an upload for whatever lands here
            layerUploaded[(size_t) i] = false;

            if (wanted != nullptr)
                arriving.push_back (wanted);
        }

        if (! leaving.empty() || ! arriving.empty())
            scheduleModeSwitch (leaving, arriving);
    }

    /** Per-layer settings, parallel to setLayerSources. */
    void setLayerSettings (int index, const LayerSettings& s) noexcept
    {
        if (index < 0 || index >= kMaxLayers) return;

        auto& d = settings[(size_t) index];
        d.blend  .store (juce::jlimit (0, 2, s.blend));
        d.opacity.store (juce::jlimit (0.0f, 1.0f, s.opacity));
        d.rot    .store (juce::jlimit (0, 3, s.rot));
        d.scale  .store (juce::jlimit (0.1f, 4.0f, s.scale));
        d.offX   .store (juce::jlimit (-1.0f, 1.0f, s.offX));
        d.offY   .store (juce::jlimit (-1.0f, 1.0f, s.offY));
        d.amount .store (juce::jlimit (0.0f, 1.0f, s.amount));
        d.bands  .store (juce::jlimit (2, 32, s.bands));
        d.angle  .store (juce::degreesToRadians (s.angleDeg));
        d.edge   .store (juce::jlimit (0.0f, 1.0f, s.edge));

        d.mirror   .store (juce::jlimit (0, 8, s.mirror));
        d.mirrorAng.store (juce::degreesToRadians (s.mirrorAngDeg));
        d.symmetry .store (juce::jlimit (1, 11, s.symmetry));
        d.spinDeg  .store (s.spinDeg);
        d.speed    .store (juce::jlimit (0.0f, 1.0f, s.speed));
        d.zoom     .store (juce::jlimit (0.25f, 4.0f, s.zoom));
    }

    // ── post chain (message thread) ─────────────────────────────────────────
    void setWarp (bool on, float amt, float swirl, float smooth, int sourceLayer,
                  float denoise = 0.0f) noexcept
    {
        warpOn     .store (on);
        warpAmt    .store (juce::jlimit (0.0f, 1.0f, amt));
        warpSwirl  .store (juce::jlimit (0.0f, 1.0f, swirl));
        warpSmooth .store (juce::jlimit (0.0f, 1.0f, smooth));
        warpDenoise.store (juce::jlimit (0.0f, 1.0f, denoise));
        warpSrc    .store (juce::jlimit (0, kMaxLayers - 1, sourceLayer));
    }

    /** WHICH layers the whole global chain (fold, tunnel, liquid, warp) touches.
        All true = folds the whole result, exactly as before. */
    void setGlobalLayers (bool l0, bool l1, bool l2) noexcept
    {
        globL0.store (l0); globL1.store (l1); globL2.store (l2);
    }

    /** LIQUID: a shared flow that melts the targeted layers together. */
    void setLiquid (bool on, float amt, float smooth, float denoise = 0.0f) noexcept
    {
        liquidOn     .store (on);
        liquidAmt    .store (juce::jlimit (0.0f, 1.0f, amt));
        liquidSmooth .store (juce::jlimit (0.0f, 1.0f, smooth));
        liquidDenoise.store (juce::jlimit (0.0f, 1.0f, denoise));
    }

    /** TUNNEL: remap the whole fusion into a radially symmetric receding tunnel. */
    void setTunnel (bool on) noexcept { tunnelOn.store (on); }

    /** How many congruent wedges the finished picture is folded into.
        1 = no fold, which is the default. */
    void setSymmetry (int wedges) noexcept { symmetry.store (juce::jlimit (1, 11, wedges)); }

    /** Reflection axes about the centre, evenly spaced, the first at
        `angleDegrees`. 0 = no fold. Independent of the wedge count — mirror
        leaves the radius alone, symmetry folds it. */
    void setMirror (int axes, float angleDegrees) noexcept
    {
        mirrorCount.store (juce::jlimit (0, 8, axes));
        mirrorAngle.store (juce::degreesToRadians (angleDegrees));
    }

    void setSpin (float degrees) noexcept  { spinDeg.store (degrees); }
    void setZoom (float z) noexcept        { zoom.store (juce::jlimit (0.25f, 4.0f, z)); }

    /** Swirl the whole picture: a turn that grows with the radius. Bipolar,
        0 = off, and it runs before the folds so the wedges stay congruent. */
    void setVortex (float v) noexcept      { vortex.store (juce::jlimit (-1.0f, 1.0f, v)); }

    /** 0 = the picture stands exactly where SPIN put it, 1 = fast rotation.
        Zero is the default, because a module that turns on its own when nobody
        asked it to is not a feature. */
    void setSpeed (float s) noexcept       { speed.store (juce::jlimit (0.0f, 1.0f, s)); }
    void setAudioDrive (float d) noexcept  { audioDrive.store (juce::jlimit (0.0f, 1.0f, d)); }

    /** Only used for the "no layers yet" placeholder. */
    void setBaseColour (juce::Colour c) noexcept { baseColour = c; }

    // ── IFusionShaderSource ────────────────────────────────────────────────────
    FusionShaderState getFusionState() const override
    {
        const juce::ScopedLock sl (stateLock);
        return snapshot;
    }

    /** The host asks for these BEFORE it draws anything, so it can give the
        GPU-backed ones a framebuffer of their own and render them into it. See
        the layer-target note in AlterGLHost. */
    void getFusionLayerComponents (juce::Component** out, int maxCount) const override
    {
        for (int i = 0; i < maxCount; ++i)
            out[i] = (i < kMaxLayers) ? layer[(size_t) i].getComponent() : nullptr;
    }

    bool fusionGlPrepare (juce::OpenGLContext& ctx, int firstTexUnit, FusionFrameData& out) override;
    void fusionGlRelease() override;

    void deliverFusionOffscreenImage (const juce::Image& img) override
    {
        {
            const juce::ScopedLock sl (imgLock);
            offscreenImg = img;
        }
        juce::Component::SafePointer<FusionVisual> sp (this);
        juce::MessageManager::callAsync ([sp]() mutable { if (sp != nullptr) sp->repaint(); });
    }

private:
    // ── Component ───────────────────────────────────────────────────────────
    void paint (juce::Graphics& g) override
    {
        if (isOffscreen)
        {
            // Detached: draw the image the HUD host rendered for us.
            const juce::ScopedLock sl (imgLock);
            if (offscreenImg.isValid())
                g.drawImageAt (offscreenImg, 0, 0);
            return;
        }

        // Inline: the host draws the fused picture straight into this rectangle,
        // so there is nothing to paint — except when there is nothing to fuse.
        if (! haveLayers())
        {
            // The "nothing to fuse yet" placeholder. On transparency it must stay
            // empty rather than stamping the theme gradient into an export.
            if (isTransparentBackground())
                return;

            AlterTheme::paintBackground (g, getLocalBounds().toFloat());
            g.setColour (baseColour.withAlpha (0.35f));
            g.drawRect (getLocalBounds(), 1);
            g.setColour (AlterTheme::textDim);
            g.setFont (juce::Font (juce::FontOptions (
                juce::jlimit (10.0f, 15.0f, (float) juce::jmin (getWidth(), getHeight()) * 0.07f))));
            g.drawText ("FUSION - pick the layers", getLocalBounds().reduced (6),
                        juce::Justification::centred, false);
        }
    }

    bool haveLayers() const noexcept
    {
        for (auto& l : layer)
            if (l.getComponent() != nullptr)
                return true;
        return false;
    }

    /** Force the FBO path even while docked in the HUD.

        Same switch the three GL modules already have, and for the same reason:
        somebody needs PIXELS from this module rather than a picture painted into
        the window behind everything. For them it was a Fusion asking; for an
        Fusion it is the transparent export. Without it a fusion contributes
        nothing at all to an alpha frame — paint() draws nothing when inline,
        because the host is what puts the fused picture on screen. */
    void setForceOffscreen (bool shouldForce)
    {
        if (forceOffscreen == shouldForce) return;
        forceOffscreen = shouldForce;
        updateHostRegistration();
        repaint();
    }

    void parentHierarchyChanged() override { updateHostRegistration(); }

    void updateHostRegistration()
    {
        auto* localHost = AlterGLHost::forComponent (this);
        AlterGLHost* target = (localHost != nullptr) ? localHost : AlterGLHost::getPrimary();
        isOffscreen = forceOffscreen ? (target != nullptr)
                                     : ((localHost == nullptr) && (target != nullptr));

        if (target != currentHost)
        {
            if (currentHost != nullptr)
            {
                currentHost->removeSource (static_cast<IFusionShaderSource*> (this));
                // The textures belong to the OLD context; hand them back to it.
                for (auto& t : layerTex)
                {
                    currentHost->scheduleTextureDelete (t);
                    t = 0;
                }
                for (auto& g : layerGen) g = 0;
                for (auto& u : layerUploaded) u = false;
            }

            currentHost = target;

            if (currentHost != nullptr)
                currentHost->addSource (static_cast<IFusionShaderSource*> (this));
        }
    }

    /** Puts modules into / out of "lend your frames to a Fusion" mode, LATER.

        Both halves of this block: setForceOffscreen goes through the GL host's
        source registry, whose lock the GL thread holds for a whole frame, and
        setTransparentBackground has to be paired with it so a module never ends up
        transparent while still being drawn in the HUD. Neither may run inside the
        layout pass that called us — see the note at the top of this file. */
    void scheduleModeSwitch (std::vector<juce::Component*> leaving,
                             std::vector<juce::Component*> arriving)
    {
        std::vector<juce::Component::SafePointer<juce::Component>> out, in;
        for (auto* c : leaving)  out.push_back (c);
        for (auto* c : arriving) in.push_back (c);

        juce::MessageManager::callAsync ([out, in]() mutable
        {
            for (auto& sp : out)
                if (auto* c = sp.getComponent())
                    setLayerMode (*c, false);

            for (auto& sp : in)
                if (auto* c = sp.getComponent())
                    setLayerMode (*c, true);
        });
    }

    void releaseAllLayers()
    {
        // Destructor path: this must happen NOW, not on the message queue, because
        // the modules outlive us and would otherwise stay stuck in layer mode.
        for (auto& l : layer)
            if (auto* c = l.getComponent())
                setLayerMode (*c, false);
    }

public:
    /** Switches one module between "normal HUD module" and "Fusion layer".

        Two things change together. A GL-backed module has to render into an FBO we
        can read instead of straight into the window, and every module drops its
        background so what reaches us in the alpha channel is real coverage rather
        than something we have to guess at.

        PUBLIC because a Fusion is no longer the only thing that needs a module's
        real coverage: the transparent (.mov + alpha) export puts the whole HUD into
        exactly this state. Sharing the one implementation is what stops the two
        from drifting apart — a module that composites correctly into a fusion is
        then, by construction, a module that exports correctly with alpha. */
    static void setLayerMode (juce::Component& c, bool isLayer);

private:

    /** Runs `fn` on a layer's finished frame, holding that module's own lock for
        the duration. Returns false when the layer has produced nothing new since
        `generation`, or when it has no frame buffer at all — see hasFrameBuffer. */
    static bool readLayerFrame (juce::Component& c, juce::uint32& generation,
                                const std::function<void (const juce::Image&)>& fn);

    /** Does this module keep a finished frame we can upload straight from?

        Most do: AsyncVisualBase double-buffers one, and the GL modules are handed
        one back by the host. But the Audio Meter, the Oscilloscope and the Tone
        Analyzer are plain Components that paint directly into whatever Graphics
        they are given — they have no frame to lend, so for them (and only them)
        the frame is produced here, on the message thread, into a buffer of ours. */
    static bool hasFrameBuffer (juce::Component& c);

    struct AtomicLayer;   // defined below

    /** GL THREAD. Everything one layer contributes, in the units the shader takes.
        One function for both paths — the uploaded frame and the module's own
        framebuffer — because the ONLY thing that differs between them is where the
        pixels live, and letting the two fill this in separately is how they would
        eventually stop agreeing about anything else. */
    static void writeLayerParams (FusionLayerParams& o, const AtomicLayer& s,
                                  juce::Component& c, double nowSecs, bool fromFbo);

    /** Did this module hand us REAL coverage in its alpha channel? */
    static bool layerHasAlpha (juce::Component& c)
    {
        if (auto* t = dynamic_cast<ThemedBackground*> (&c))
            return t->isTransparentBackground() && t->transparencyReachesPixels();
        return false;
    }

    /** Paints a bufferless layer into fallbackImg[i]. Message thread. */
    void refreshFallbackFrame (int index, juce::Component& c);

    // ── Timer: placement snapshot + audio follow (message thread) ───────────
    void timerCallback() override
    {
        if (! AlterTheme::hudFrozen.load())
        {
            // Audio only ever MODULATES what was dialled in, so silence looks like
            // the settings and a loud passage exaggerates them.
            const float rms = juce::jlimit (0.0f, 1.0f, audioSource.getLastRms() * 3.0f);
            smoothedRms += (rms - smoothedRms) * 0.18f;
        }

        // The layers that have no frame of their own are painted HERE, on the
        // message thread — see hasFrameBuffer. That is a full component paint per
        // layer, and at 60 Hz with three of them it is enough to make the
        // controller feel stuck, because the controller lives on this thread too.
        //
        // ~30 Hz instead. These are the Audio Meter, the Oscilloscope and the Tone
        // Analyzer: readouts whose picture is smooth already, and none of which
        // looks different at half the rate inside a fusion. Nothing else on this
        // thread should have to wait behind them.
        const auto nowMs = juce::Time::getMillisecondCounter();
        if (nowMs - lastFallbackMs >= 32)
        {
            lastFallbackMs = nowMs;

            for (int i = 0; i < kMaxLayers; ++i)
                if (auto* c = layer[(size_t) i].getComponent())
                    if (! hasFrameBuffer (*c))
                        refreshFallbackFrame (i, *c);
        }

        FusionShaderState st;
        st.offscreen    = isOffscreen;
        st.noBackground = isTransparentBackground();

        const float lw = juce::jmax (1.0f, (float) getWidth());
        const float lh = juce::jmax (1.0f, (float) getHeight());
        st.localAspect = lw / lh;

        if (isOffscreen)
        {
            st.x = 0; st.y = 0;
            st.w = getWidth(); st.h = getHeight();
            st.active = isShowing() && getWidth() > 1 && getHeight() > 1;
        }
        else if (currentHost != nullptr)
        {
            if (auto* hc = currentHost->getAttachedComponent())
            {
                const auto r = hc->getLocalArea (this, getLocalBounds());
                st.x = r.getX(); st.y = r.getY(); st.w = r.getWidth(); st.h = r.getHeight();
                st.active = isShowing() && st.w > 1 && st.h > 1;

                // The basis is derived from the LIVE transform, exactly as the
                // spectrogram derives its own (see Spectrogram::updateSgState).
                // That is what lets a Fusion panel be rotated 0/90/180/270 like
                // any other module: the rect the host draws into is the bounding
                // box of the rotated panel, and this maps it back into the module's
                // own space so the folds, the bands and the background all turn
                // with it instead of staying nailed to the screen.
                auto toLocalN = [this, hc, lw, lh] (juce::Point<int> p)
                {
                    const auto q = getLocalPoint (hc, p.toFloat());
                    return juce::Point<float> (q.x / lw, q.y / lh);
                };
                const auto p00 = toLocalN (r.getTopLeft());
                st.local0  = p00;
                st.localDU = toLocalN (r.getTopRight())   - p00;
                st.localDV = toLocalN (r.getBottomLeft()) - p00;
            }
        }

        {
            const juce::ScopedLock sl (stateLock);
            snapshot = st;
        }

        // Repaint every tick so the host's 2D overlay layer (the top-right info /
        // per-layer rows) re-renders — exactly as Synesthesia does. The fused
        // picture itself is drawn by the GL host, so WITHOUT this the overlay is
        // painted once and then frozen: it would not follow the layers' live values,
        // and the "Hide info" toggle would not hide it. When there are no layers this
        // also keeps the "pick the layers" placeholder alive.
        repaint();
    }

    // ── State ───────────────────────────────────────────────────────────────
    IAudioSource& audioSource;

    // SafePointer, not a raw pointer: the borrowed module belongs to MainComponent
    // and can be deleted by the user at any moment. This turns that into an empty
    // slot instead of a dangling read.
    std::array<juce::Component::SafePointer<juce::Component>, kMaxLayers> layer;
    std::array<unsigned int, kMaxLayers> layerTex     { { 0, 0, 0 } };   // GL thread only
    std::array<int, kMaxLayers>          layerTexW    { { 0, 0, 0 } };
    std::array<int, kMaxLayers>          layerTexH    { { 0, 0, 0 } };
    std::array<juce::uint32, kMaxLayers> layerGen     { { 0, 0, 0 } };   // last frame uploaded
    std::array<bool, kMaxLayers>         layerUploaded{ { false, false, false } };

    AlterGLHost* currentHost = nullptr;
    bool         isOffscreen = false;
    bool         forceOffscreen = false;   // transparent export -> render via FBO

    juce::CriticalSection stateLock;
    FusionShaderState        snapshot;

    juce::CriticalSection imgLock;
    juce::Image           offscreenImg;   // last frame rendered for a detached window

    // Frames for the layers that have none of their own (see hasFrameBuffer).
    // Written on the message thread, uploaded on the GL thread.
    juce::CriticalSection                fallbackLock;
    std::array<juce::Image, kMaxLayers>  fallbackImg;
    std::array<juce::uint32, kMaxLayers> fallbackGen { { 0, 0, 0 } };
    juce::uint32                         lastFallbackMs = 0;   // message thread only

    /** Per-layer settings. Plain atomics: a torn read costs one stale frame. */
    struct AtomicLayer
    {
        std::atomic<int>   blend   { 0 };
        std::atomic<float> opacity { 1.0f };
        std::atomic<int>   rot     { 0 };
        std::atomic<float> scale   { 1.0f };
        std::atomic<float> offX    { 0.0f };
        std::atomic<float> offY    { 0.0f };
        std::atomic<float> amount  { 0.5f };
        std::atomic<int>   bands   { 8 };
        std::atomic<float> angle   { 0.0f };
        std::atomic<float> edge    { 0.6f };

        std::atomic<int>   mirror    { 0 };
        std::atomic<float> mirrorAng { 0.0f };
        std::atomic<int>   symmetry  { 1 };
        std::atomic<float> spinDeg   { 0.0f };
        std::atomic<float> speed     { 0.0f };
        std::atomic<float> zoom      { 1.0f };
    };
    std::array<AtomicLayer, kMaxLayers> settings;

    std::atomic<bool>  warpOn      { false };
    std::atomic<float> warpAmt     { 0.5f };
    std::atomic<float> warpSwirl   { 0.6f };
    std::atomic<float> warpSmooth  { 0.0f };
    std::atomic<float> warpDenoise { 0.0f };
    std::atomic<int>   warpSrc     { 0 };

    std::atomic<int>   symmetry   { 1 };
    std::atomic<int>   mirrorCount{ 0 };
    std::atomic<float> mirrorAngle{ 0.0f };
    std::atomic<float> spinDeg    { 0.0f };
    std::atomic<float> zoom       { 1.0f };
    std::atomic<float> vortex     { 0.0f };
    std::atomic<float> speed      { 0.0f };
    std::atomic<float> audioDrive { 0.0f };

    // Global-chain layer targeting (all on = fold the whole result, as before).
    std::atomic<bool>  globL0 { true };
    std::atomic<bool>  globL1 { true };
    std::atomic<bool>  globL2 { true };

    // Liquid melt + Tunnel.
    std::atomic<bool>  liquidOn      { false };
    std::atomic<float> liquidAmt     { 0.5f };
    std::atomic<float> liquidSmooth  { 0.5f };
    std::atomic<float> liquidDenoise { 0.0f };
    std::atomic<bool>  tunnelOn      { false };

    juce::Colour baseColour { AlterTheme::accent };
    float        smoothedRms = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FusionVisual)
};

// =============================================================================
//  Implementation
// =============================================================================

#include "Synesthesia.h"
#include "GeometryVisual.h"
#include "Spectrogram.h"
#include "AsyncVisualBase.h"

inline void FusionVisual::setLayerMode (juce::Component& c, bool isLayer)
{
    // Drop the background first when becoming a layer, restore it last when
    // leaving, so the module is never transparent while it is still being drawn
    // in the HUD — that would show the desktop through it for a frame.
    // AN OPAQUE COMPONENT THAT STOPS FILLING ITS BOUNDS IS LYING.
    //
    // setOpaque(true) is a promise to JUCE that paint() covers every pixel, and
    // JUCE is entitled to skip work on the strength of it. Telling such a module to
    // drop its background breaks that promise, and the module then contributes
    // NOTHING — not a border, not a readout, nothing. Measured on a transparent
    // export: the two modules that came out completely empty were exactly the two
    // that call setOpaque, while every module that never claimed to be opaque drew
    // normally.
    //
    // The flag is parked on the component itself so leaving layer mode restores
    // what the module actually chose, rather than a guess.
    if (isLayer)
    {
        if (auto* t = dynamic_cast<ThemedBackground*> (&c))
            t->setTransparentBackground (true);

        c.getProperties().set ("alterWasOpaque", c.isOpaque());
        c.setOpaque (false);
    }
    else
    {
        c.setOpaque ((bool) c.getProperties().getWithDefault ("alterWasOpaque", false));
    }

    // The three GL modules do not share a base class, so this is a small set of
    // dynamic_casts rather than a virtual call — adding a base class purely for
    // this would touch far more code than it saves.
    if (auto* s  = dynamic_cast<VisualSynesthesia*> (&c)) s->setForceOffscreen  (isLayer);
    if (auto* gv = dynamic_cast<GeometryVisual*> (&c))    gv->setForceOffscreen (isLayer);
    if (auto* sg = dynamic_cast<SpectrogramMeter*> (&c))  sg->setForceOffscreen (isLayer);
    if (auto* al = dynamic_cast<FusionVisual*> (&c))     al->setForceOffscreen (isLayer);

    if (! isLayer)
        if (auto* t = dynamic_cast<ThemedBackground*> (&c))
            t->setTransparentBackground (false);

    c.repaint();
}

inline void FusionVisual::writeLayerParams (FusionLayerParams& o, const AtomicLayer& s,
                                            juce::Component& c, double nowSecs, bool fromFbo)
{
    // This layer's own turn: where it was dialled to, plus its own drift. Same
    // arithmetic as the global spin, because they are the same question asked at
    // two scopes and must not answer differently.
    //
    // The AUDIO is deliberately absent here and present globally: a layer that
    // reacted on its own as well would be pushed twice by the same loud passage,
    // and the stack would shear apart every time the music got busy.
    o.mirror      = (float) s.mirror.load();
    o.mirrorAngle = s.mirrorAng.load();
    o.symmetry    = (float) s.symmetry.load();
    o.zoom        = s.zoom.load();
    o.spin        = juce::degreesToRadians (s.spinDeg.load())
                  + (float) (nowSecs * s.speed.load() * 1.6);

    o.blend    = (float) s.blend.load();
    o.opacity  = s.opacity.load();
    o.rot      = (float) s.rot.load();
    o.scale    = s.scale.load();
    o.offX     = s.offX.load();
    o.offY     = s.offY.load();
    o.amount   = s.amount.load();
    o.bands    = (float) s.bands.load();
    o.angle    = s.angle.load();
    o.edge     = s.edge.load();

    o.hasAlpha = layerHasAlpha (c) ? 1.0f : 0.0f;
    o.fromFbo  = fromFbo ? 1.0f : 0.0f;
}

inline bool FusionVisual::hasFrameBuffer (juce::Component& c)
{
    return dynamic_cast<VisualSynesthesia*> (&c) != nullptr
        || dynamic_cast<GeometryVisual*> (&c)    != nullptr
        || dynamic_cast<SpectrogramMeter*> (&c)  != nullptr
        || dynamic_cast<AsyncVisualBase*> (&c)   != nullptr;
}

inline void FusionVisual::refreshFallbackFrame (int index, juce::Component& c)
{
    const int w = c.getWidth(), h = c.getHeight();
    if (w < 2 || h < 2) return;

    const juce::ScopedLock sl (fallbackLock);
    auto& img = fallbackImg[(size_t) index];

    if (! img.isValid() || img.getWidth() != w || img.getHeight() != h)
        img = juce::Image (juce::Image::ARGB, w, h, false, juce::SoftwareImageType());

    img.clear (img.getBounds(), juce::Colours::transparentBlack);
    {
        juce::Graphics g (img);
        c.paintEntireComponent (g, false);
    }

    ++fallbackGen[(size_t) index];
}

inline bool FusionVisual::readLayerFrame (juce::Component& c, juce::uint32& generation,
                                           const std::function<void (const juce::Image&)>& fn)
{
    // GL modules produce their frame through the host's FBO, 2D modules through
    // their own worker. Ask in that order: Geometry and Spectrogram are BOTH, and
    // while they are a layer it is the offscreen frame that is the real one.
    if (auto* s  = dynamic_cast<VisualSynesthesia*> (&c)) return s->readOffscreenFrame (generation, fn);
    if (auto* gv = dynamic_cast<GeometryVisual*> (&c))    return gv->readOffscreenFrame (generation, fn);
    if (auto* sg = dynamic_cast<SpectrogramMeter*> (&c))  return sg->readOffscreenFrame (generation, fn);
    if (auto* av = dynamic_cast<AsyncVisualBase*> (&c))   return av->readFrontFrame (generation, fn);

    return false;
}

inline bool FusionVisual::fusionGlPrepare (juce::OpenGLContext& ctx, int firstTexUnit,
                                         FusionFrameData& out)
{
    using namespace juce::gl;
    juce::ignoreUnused (ctx);

    // ONE clock reading for the whole frame. Every drift in this module — each
    // layer's and the stack's — is derived from it, so they cannot slide apart by
    // the microseconds it takes to walk the loop.
    const double nowSecs = juce::Time::getMillisecondCounterHiRes() * 0.001;

    int count = 0;

    for (int i = 0; i < kMaxLayers; ++i)
    {
        auto* c = layer[(size_t) i].getComponent();
        if (c == nullptr)
            continue;

        auto& tex = layerTex[(size_t) i];

        // Point at this layer's UNIT FIRST, and bind once.
        //
        // Every glBindTexture goes to whatever unit is currently active, so
        // uploading layer 1 while unit 0 was still selected rebound layer 1's
        // texture over layer 0's slot — both units ended up holding the same
        // texture and one layer vanished.
        glActiveTexture ((GLenum) (GL_TEXTURE0 + firstTexUnit + count));

        // ── the GPU path ─────────────────────────────────────────────────────
        //
        // The host has already drawn this layer into a framebuffer of its own,
        // this frame, so the pixels are on the GPU where they are needed and
        // there is nothing to copy. This is the case for every module that draws
        // itself with a shader — the fractal, the geometry, the spectrogram —
        // and it is the one that used to cost a full glReadPixels and a re-upload
        // per layer per frame.
        //
        // The 2D modules fall through to the upload below, and always will: they
        // rasterise on a worker thread, so their pixels genuinely start in main
        // memory and have to be carried across.
        const auto shared = (currentHost != nullptr)
                                ? currentHost->getFusionLayerTexture (c)
                                : AlterGLHost::LayerTexture {};

        if (shared.id != 0)
        {
            glBindTexture (GL_TEXTURE_2D, shared.id);
            // Bilinear and clamped, same as our own textures: the folds and Warp
            // read at arbitrary positions, and the shader already treats anything
            // outside the layer as empty.
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            writeLayerParams (out.layer[(size_t) count], settings[(size_t) i], *c, nowSecs,
                              /* fromFbo */ true);
            ++count;
            continue;
        }

        if (tex == 0)
        {
            glGenTextures (1, &tex);
            if (tex == 0) continue;

            glBindTexture (GL_TEXTURE_2D, tex);
            // Bilinear, clamped: the fold and Warp read at arbitrary positions, and
            // the shader already treats anything outside the layer as empty, so the
            // clamp only ever matters at the very edge.
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            layerTexW[(size_t) i] = 0;
            layerTexH[(size_t) i] = 0;
        }

        // BIND BEFORE UPLOADING, every frame, not just on the frame that created
        // the texture.
        //
        // glTexSubImage2D writes to whatever is bound to the ACTIVE unit, and the
        // host unbinds every unit at the end of each frame — so from the second
        // frame on, every upload was landing on texture 0 and going nowhere. The
        // first frame stuck because that is the one where the texture was freshly
        // generated and therefore still bound.
        //
        // That single missing bind is both of the symptoms: a layer froze on its
        // very first frame (so a Spectrum sat there not reacting to anything), and
        // a layer whose first frame had not arrived yet never received a single
        // pixel while still being counted as present — which is a fully
        // transparent layer, and looks exactly like only one layer existing.
        glBindTexture (GL_TEXTURE_2D, tex);

        // Upload only what is NEW. readLayerFrame returns false when the layer has
        // not finished a frame since the last one we took, which is what stops a
        // module running at 30 fps from being uploaded on every 60 Hz swap.
        juce::uint32& gen = layerGen[(size_t) i];

        auto upload = [&] (const juce::Image& img)
            {
                const juce::Image::BitmapData bd (img, juce::Image::BitmapData::readOnly);
                if (bd.data == nullptr || bd.width < 1 || bd.height < 1)
                    return;

                glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
                glPixelStorei (GL_UNPACK_ROW_LENGTH, bd.lineStride / juce::jmax (1, bd.pixelStride));

                if (bd.width != layerTexW[(size_t) i] || bd.height != layerTexH[(size_t) i])
                {
                    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, bd.width, bd.height, 0,
                                  GL_BGRA, GL_UNSIGNED_BYTE, bd.data);
                    layerTexW[(size_t) i] = bd.width;
                    layerTexH[(size_t) i] = bd.height;
                }
                else
                {
                    glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, bd.width, bd.height,
                                     GL_BGRA, GL_UNSIGNED_BYTE, bd.data);
                }

                glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
                layerUploaded[(size_t) i] = true;
            };

        // Nothing has ever landed here and nothing is arriving this frame either:
        // skip the slot entirely rather than count an empty texture as a layer.
        // Counting it is what used to put a fully transparent sheet into the stack.

        if (hasFrameBuffer (*c))
        {
            readLayerFrame (*c, gen, upload);
        }
        else
        {
            const juce::ScopedLock sl (fallbackLock);
            if (fallbackImg[(size_t) i].isValid() && fallbackGen[(size_t) i] != gen)
            {
                gen = fallbackGen[(size_t) i];
                upload (fallbackImg[(size_t) i]);
            }
        }

        if (! layerUploaded[(size_t) i])
            continue;

        // Settings travel with the COMPACTED index, not the slot index: an empty
        // slot A means what the user put in slot B is on texture unit 0, and the
        // shader only ever knows about units.
        writeLayerParams (out.layer[(size_t) count], settings[(size_t) i], *c, nowSecs,
                          /* fromFbo */ false);
        ++count;
    }

    if (count == 0)
        return false;

    out.layerCount = count;

    const float drive = smoothedRms * audioDrive.load();
    const float spd   = speed.load();

    out.drive = drive;

    out.warp      = warpOn.load() ? 1.0f : 0.0f;
    out.warpAmt   = warpAmt.load();
    out.warpSwirl  = warpSwirl.load();
    out.warpSmooth = warpSmooth.load();
    out.warpDenoise = warpDenoise.load();
    out.warpSrc    = (float) juce::jmin (warpSrc.load(), count - 1);

    out.symmetry    = (float) symmetry.load();
    out.mirror      = (float) mirrorCount.load();
    out.mirrorAngle = mirrorAngle.load();
    out.zoom        = zoom.load();
    out.vortex      = vortex.load();

    out.globLayer[0] = globL0.load() ? 1.0f : 0.0f;
    out.globLayer[1] = globL1.load() ? 1.0f : 0.0f;
    out.globLayer[2] = globL2.load() ? 1.0f : 0.0f;

    out.liquid       = liquidOn.load() ? 1.0f : 0.0f;
    out.liquidAmt    = liquidAmt.load();
    out.liquidSmooth = liquidSmooth.load();
    out.liquidDenoise = liquidDenoise.load();
    out.tunnel       = tunnelOn.load() ? 1.0f : 0.0f;

    // Wrapped so the float that reaches the shader keeps its precision after the
    // app has been open for hours.
    out.time         = (float) std::fmod (nowSecs, 3600.0);

    // SPIN is where the picture is turned to, SPEED is how fast it drifts from
    // there, and the audio pushes it further. At speed 0 — the default — the angle
    // is exactly what was dialled in and nothing moves on its own.
    //
    // The audio rides on the GLOBAL turn only. A layer that reacted on its own as
    // well would be pushed twice by the same loud passage, and the stack would
    // shear apart every time the music got busy.
    out.spin = juce::degreesToRadians (spinDeg.load())
             + (float) (nowSecs * spd * 1.6)
             + drive * 0.6f;

    return true;
}

inline void FusionVisual::fusionGlRelease()
{
    using namespace juce::gl;

    for (int i = 0; i < kMaxLayers; ++i)
    {
        if (layerTex[(size_t) i] != 0)
            glDeleteTextures (1, &layerTex[(size_t) i]);

        layerTex[(size_t) i]      = 0;
        layerTexW[(size_t) i]     = 0;
        layerTexH[(size_t) i]     = 0;
        layerGen[(size_t) i]      = 0;
        layerUploaded[(size_t) i] = false;
    }
}
