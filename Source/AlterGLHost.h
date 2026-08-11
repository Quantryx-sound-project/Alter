/*
  ==============================================================================

    AlterGLHost.h
    One per top-level window. Attaches a single OpenGL context to a top-level
    component so that:
      * all 2D component painting (Spectrum, Chladni, Stereoscope, meters...) is
        GPU-accelerated and composited on the GPU instead of the CPU/GDI path;
      * the theme background is drawn on the GPU;
      * every VisualSynesthesia under this window is rendered with ONE shared
        fractal shader into its own viewport rectangle;
      * every SpectrogramMeter under this window is rendered with ONE shared
        spectrogram shader straight from its history texture (see SgShader.h);
      * every GeometryVisual under this window has its shapes expanded, stroked,
        glowed and bloomed by ONE shared instanced shader plus a small blur
        pyramid, straight from its instance texture (see GeoShader.h).

    This is what lets the HUD stay smooth under heavy load: the heavy 2D
    rasterisation already happens on worker threads (AsyncVisualBase) and the
    final composite/present now happens on the GPU.

    Rendering order each frame (on the GL thread):
      1. renderOpenGL(): clear to the theme background, then draw each registered
         Synesthesia's fractal and each registered Spectrogram into its scissored
         viewport.
      2. JUCE paints the component tree ON TOP (opaque 2D modules overwrite their
         rectangles; transparent regions — gaps, Synesthesia tiles and the
         spectrogram's grid overlay — keep the GL output). For this to work the
         host component must be NON-opaque and must not fill its own background.

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <vector>
#include <map>
#include <memory>
#include <set>
#include <iterator>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include "AlterTheme.h"
#include "SynShader.h"
#include "SgShader.h"
#include "GeoShader.h"
#include "FusionShader.h"
#include "AlterPresentClock.h"

class AlterGLHost : private juce::OpenGLRenderer
{
public:
    AlterGLHost() = default;
    ~AlterGLHost() override { detach(); }

    /** Attach a GPU context to a top-level component. Call once, after the
        component exists. The component should be setOpaque(false) and paint
        nothing (or only overlays), so the GL background shows through. */
    void attach (juce::Component& topLevel)
    {
        if (attached) return;
        comp = &topLevel;

        {
            const juce::ScopedLock sl (mapLock());
            hostMap()[comp] = this;
        }

        if (primaryRef() == nullptr)
            primaryRef() = this;          // the HUD host renders offscreen fractals for detached windows

        context.setOpenGLVersionRequired (juce::OpenGLContext::OpenGLVersion::openGL3_2);
        context.setRenderer (this);
        context.setComponentPaintingEnabled (true);
        context.setContinuousRepainting (true);
        // Explicit vsync: this context is the app's presentation clock
        // (AlterPresentClock), so its cadence must be the panel's.
        context.setSwapInterval (1);
        context.attachTo (topLevel);
        attached = true;
    }

    void detach()
    {
        if (! attached) return;
        attached = false;
        context.detach();

        if (primaryRef() == this)
            primaryRef() = nullptr;

        const juce::ScopedLock sl (mapLock());
        if (comp != nullptr)
            hostMap().erase (comp);
        comp = nullptr;
    }

    /** The primary (HUD) host. Detached Synesthesias register here to be rendered
        offscreen, since their own window has no GL context. */
    static AlterGLHost* getPrimary() { return primaryRef(); }

    juce::OpenGLContext& getContext() noexcept { return context; }

    /** The component the context is attached to (Synesthesia computes its
        rectangle in this component's coordinate space). */
    juce::Component* getAttachedComponent() const noexcept { return comp; }

    // ── Synesthesia registry (called on the message thread) ──────────────────
    void addSource (ISynShaderSource* s)
    {
        if (s == nullptr) return;
        const juce::ScopedLock sl (srcLock);
        if (std::find (sources.begin(), sources.end(), s) == sources.end())
            sources.push_back (s);
    }

    void removeSource (ISynShaderSource* s)
    {
        const juce::ScopedLock sl (srcLock);
        sources.erase (std::remove (sources.begin(), sources.end(), s), sources.end());
        scheduleOffscreenDelete (s);
    }

    // ── Spectrogram registry (called on the message thread) ──────────────────
    void addSource (ISgShaderSource* s)
    {
        if (s == nullptr) return;
        const juce::ScopedLock sl (srcLock);
        if (std::find (sgSources.begin(), sgSources.end(), s) == sgSources.end())
            sgSources.push_back (s);
    }

    void removeSource (ISgShaderSource* s)
    {
        // The GL thread holds srcLock for its WHOLE frame, so by the time this
        // returns the source is guaranteed not to be in use and may safely be
        // destroyed.
        const juce::ScopedLock sl (srcLock);
        sgSources.erase (std::remove (sgSources.begin(), sgSources.end(), s), sgSources.end());
        // Safe under srcLock: the GL thread holds it for its whole frame, so it
        // cannot be inside renderSgOffscreen touching this map right now.
        sgOffscreenLastMs.erase (s);
        scheduleOffscreenDelete (s);
    }

    // ── Geometry registry (called on the message thread) ─────────────────────
    void addSource (IGeoShaderSource* s)
    {
        if (s == nullptr) return;
        const juce::ScopedLock sl (srcLock);
        if (std::find (geoSources.begin(), geoSources.end(), s) == geoSources.end())
            geoSources.push_back (s);
    }

    void removeSource (IGeoShaderSource* s)
    {
        // The GL thread holds srcLock for its WHOLE frame, so by the time this
        // returns the source is guaranteed not to be in use and may safely be
        // destroyed.
        const juce::ScopedLock sl (srcLock);
        geoSources.erase (std::remove (geoSources.begin(), geoSources.end(), s), geoSources.end());
        geoOffscreenLastMs.erase (s);
        scheduleOffscreenDelete (s);
    }

    // ── Fusion registry (called on the message thread) ──────────────────────
    void addSource (IFusionShaderSource* s)
    {
        if (s == nullptr) return;
        const juce::ScopedLock sl (srcLock);
        if (std::find (fusionSources.begin(), fusionSources.end(), s) == fusionSources.end())
            fusionSources.push_back (s);
    }

    void removeSource (IFusionShaderSource* s)
    {
        // The GL thread holds srcLock for its WHOLE frame, so by the time this
        // returns the source is guaranteed not to be in use and may safely be
        // destroyed.
        const juce::ScopedLock sl (srcLock);
        fusionSources.erase (std::remove (fusionSources.begin(), fusionSources.end(), s), fusionSources.end());
        fusionOffscreenLastMs.erase (s);
        scheduleOffscreenDelete (s);
    }

    /** True once the instanced geometry shader has compiled and linked on SOME
        context. GeometryVisual asks this to decide whether it may hand its work
        to the GPU at all; when it is false the module keeps rasterising on the
        worker thread exactly as before, so a driver that cannot build the shader
        degrades to the old behaviour instead of to a black rectangle. */
    static bool geoGpuAvailable() noexcept { return geoReady().load(); }

    /** A source being destroyed hands its texture names here instead of deleting
        them itself: only the GL thread has a current context, and blocking the
        message thread on it would risk deadlocking against JUCE's own
        component-painting message lock. They are freed at the top of the next
        frame. Harmless if the context has already gone (the driver dropped them). */
    void scheduleTextureDelete (unsigned int tex)
    {
        if (tex == 0) return;
        const juce::ScopedLock sl (deadTexLock);
        deadTextures.push_back (tex);
    }

    /** A source is going away: drop the offscreen framebuffer kept for it.
        NOT deleted here. removeSource runs on the MESSAGE thread, and an
        OpenGLFrameBuffer destructor makes GL calls — without a current context
        those are undefined behaviour, which is the classic way an app dies while
        rebuilding its modules rather than while rendering them. The GL thread
        collects it at the top of its next frame, exactly like dead textures. */
    void scheduleOffscreenDelete (const void* sourceKey)
    {
        const juce::ScopedLock sl (deadTexLock);
        deadOffscreenKeys.push_back (sourceKey);
    }

    /** Largest history a spectrogram may allocate, in columns. This is the GPU's
        GL_MAX_TEXTURE_SIZE (the history texture is bufferCols TALL), published
        once the context exists so the worker thread can clamp its buffer to
        something the driver will actually accept. Conservative until then. */
    static std::atomic<int>& maxHistoryColumns()
    {
        static std::atomic<int> v { 2048 };
        return v;
    }

    /** Find the host whose attached component is an ancestor of `leaf`
        (so a Synesthesia can locate the host of whatever window it now lives in). */
    static AlterGLHost* forComponent (juce::Component* leaf)
    {
        const juce::ScopedLock sl (mapLock());
        for (auto* c = leaf; c != nullptr; c = c->getParentComponent())
        {
            auto it = hostMap().find (c);
            if (it != hostMap().end())
                return it->second;
        }
        return nullptr;
    }

private:
    // ── OpenGLRenderer ───────────────────────────────────────────────────────
    void newOpenGLContextCreated() override
    {
        buildQuad();
        buildEmptyVao();
        buildShader();
        buildSgShader();
        buildFusionShader();
        buildGeoShader();
        buildGeoBlurShader();
        buildGeoCompositeShader();

        // Publish the real texture limit: the history texture is bufferCols TALL,
        // so this is the hard ceiling on how long a history a spectrogram may keep.
        GLint maxTex = 0;   // GL typedefs are global; only the entry points are namespaced
        juce::gl::glGetIntegerv (juce::gl::GL_MAX_TEXTURE_SIZE, &maxTex);
        if (maxTex > 0)
            maxHistoryColumns().store (juce::jmax (256, (int) maxTex));
    }

    void openGLContextClosing() override
    {
        {
            // Let every spectrogram / geometry drop its textures while the
            // context is still alive.
            const juce::ScopedLock sl (srcLock);
            for (auto* s : sgSources)
                s->sgGlRelease();
            for (auto* s : geoSources)
                s->geoGlRelease();
            for (auto* s : fusionSources)
                s->fusionGlRelease();
        }

        // Geometry: the shader is gone, so every GeometryVisual must fall back to
        // the CPU renderer from its next frame on. Publish that BEFORE dropping
        // the programs, so no module can observe a live-but-unusable host.
        geoReady().store (false);
        geoShader.reset();
        uGeoInst.reset(); uGeoInstTexW.reset(); uGeoModuleSize.reset();
        uGeoSegs.reset(); uGeoAaPad.reset();
        geoBlurShader.reset();
        uGbTex.reset(); uGbRes.reset(); uGbOffset.reset(); uGbStep.reset();
        geoCompShader.reset();
        uGcScene.reset(); uGcVeil.reset();
        uGcRes.reset(); uGcOffset.reset(); uGcBloomGain.reset(); uGcVeilOpacity.reset();
        uGcVeilTexel.reset(); uGcNoBg.reset();
        uGcBgIn.reset(); uGcBgMid.reset(); uGcBgOut.reset();
        geoSceneFB.release();
        geoDown0.release();
        geoDown1.release();
        geoVeilTmp.release();
        geoVeil.release();
        if (geoVAO != 0) { juce::gl::glDeleteVertexArrays (1, &geoVAO); geoVAO = 0; }

        fusionShader.reset();
        uFusionRes.reset(); uFusionOffset.reset(); uFusionL0.reset(); uFusionL1.reset(); uFusionL2.reset();
        uFusionCount.reset();
        uFusionLocal0.reset(); uFusionLocalDU.reset(); uFusionLocalDV.reset();
        uFusionBgIn.reset(); uFusionBgMid.reset(); uFusionBgOut.reset(); uFusionAspect.reset();
        uFusionNoBg.reset();
        for (int i = 0; i < IFusionShaderSource::kMaxLayers; ++i)
        { uFusionLayA[i].reset(); uFusionLayB[i].reset(); uFusionLayC[i].reset(); uFusionLayD[i].reset(); }
        uFusionWarp.reset(); uFusionWarpAmt.reset(); uFusionWarpSwirl.reset();
        uFusionWarpSmooth.reset(); uFusionWarpDenoise.reset(); uFusionWarpSrc.reset(); uFusionVortex.reset();
        uFusionSymmetry.reset(); uFusionMirror.reset(); uFusionMirrorAngle.reset();
        uFusionSpin.reset(); uFusionZoom.reset(); uFusionDrive.reset();
        uFusionGlobLayers.reset(); uFusionLiquid.reset(); uFusionLiquidAmt.reset();
        uFusionLiquidSmooth.reset(); uFusionLiquidDenoise.reset(); uFusionTunnel.reset(); uFusionTime.reset();

        sgShader.reset();
        uSgRes.reset(); uSgOffset.reset(); uSgHistory.reset(); uSgLut.reset();
        uSgLocal0.reset(); uSgLocalDU.reset(); uSgLocalDV.reset();
        uSgHead.reset(); uSgVisCols.reset(); uSgBufCols.reset(); uSgFilled.reset();
        uSgRowSpan.reset(); uSgYTaps.reset();
        uSgBgIn.reset(); uSgBgMid.reset(); uSgBgOut.reset(); uSgNoBg.reset();

        if (quadVBO != 0) { juce::gl::glDeleteBuffers (1, &quadVBO); quadVBO = 0; }
        if (quadVAO != 0) { juce::gl::glDeleteVertexArrays (1, &quadVAO); quadVAO = 0; }
        uRes.reset(); uTime.reset(); uPitch.reset(); uOctave.reset(); uRMS.reset();
        uZoom.reset(); uRot.reset(); uSym.reset(); uSat.reset(); uBright.reset(); uBloom.reset();
        uBgIn.reset(); uBgOut.reset(); uOffset.reset(); uShake.reset();
        uManual.reset(); uBaseHue.reset(); uBaseSat.reset(); uBaseVal.reset();
        uVariation.reset(); uTransmute.reset();
        uMirror.reset(); uClear.reset(); uDenoise.reset(); uTunnel.reset(); uVortex.reset(); uBeatPulse.reset();
        uSynNoBg.reset();
        shader.reset();
        offscreenFBs.clear();

        // The layer targets belong to this context and must go with it.
        layerTargets.clear();
        layerSet.clear();
    }

    void renderOpenGL() override
    {
        using namespace juce::gl;

        // PHASE LOCK. One call per buffer swap, vsync-paced by the driver —
        // the app's real presentation clock. Releasing the render workers from
        // here (before the component tree is composited, so they get a full
        // frame period) keeps producer and consumer in one phase.
        AlterPresentClock::get().tick();

        // Free textures handed over by sources that have since been destroyed.
        {
            const juce::ScopedLock sl (deadTexLock);
            if (! deadTextures.empty())
            {
                glDeleteTextures ((GLsizei) deadTextures.size(), deadTextures.data());
                deadTextures.clear();
            }

            // Framebuffers of sources that have been removed. Held until now so the
            // destructor runs HERE, on the thread that owns the context.
            for (auto* key : deadOffscreenKeys)
                offscreenFBs.erase (key);
            deadOffscreenKeys.clear();
        }

        if (comp == nullptr)
        {
            glDisable (GL_SCISSOR_TEST);
            juce::OpenGLHelpers::clear (AlterTheme::bgVoid);
            return;
        }

        // Hold the lock for the WHOLE frame: this serialises with add/removeSource,
        // so a source can never be destroyed while we call getSynState() /
        // getSgState() / sgGlPrepare() on it (its destructor's removeSource()
        // blocks until this frame finishes).
        const juce::ScopedLock sl (srcLock);

        // ── pass 0: fusion layers -> their OWN framebuffers, and no further ──
        //
        // A layer that draws itself on the GPU used to make a full round trip
        // through main memory every frame: rendered into the shared offscreen FBO,
        // pulled back with glReadPixels, wrapped in an image, handed to the
        // fusion, and uploaded again as a texture. glReadPixels stalls the
        // pipeline until the GPU has caught up, so three layers meant three stalls
        // per frame in the HUD's own context — which is why a third layer did not
        // merely cost more, it froze everything including the controller.
        //
        // Now each layer keeps a framebuffer of its own, is drawn into it once
        // here, and the fusion samples that texture where it already is. Nothing
        // crosses the bus, and the cost of a third layer is one more small draw.
        collectFusionLayers();
        renderFusionLayerTargets();

        // ── pass 1: offscreen tiles (detached windows) -> FBO -> image ───────
        //
        // Skipping anything pass 0 already handled: a fusion layer needs pixels on
        // the GPU, not an image, and nobody is waiting for one.
        if (shader != nullptr)
            for (auto* s : sources)
            {
                if (isFusionLayer (s)) continue;
                const SynShaderState st = s->getSynState();
                if (st.offscreen && st.active && st.w > 1 && st.h > 1)
                    renderOffscreen (s, st);
            }

        if (sgShader != nullptr)
            for (auto* s : sgSources)
            {
                if (isFusionLayer (s)) continue;
                const SgShaderState st = s->getSgState();
                if (st.offscreen && st.active && st.w > 1 && st.h > 1)
                    renderSgOffscreen (s, st);
            }

        if (geoShader != nullptr)
            for (auto* s : geoSources)
            {
                if (isFusionLayer (s)) continue;
                const GeoShaderState st = s->getGeoState();
                if (st.offscreen && st.active && st.w > 1 && st.h > 1)
                    renderGeoOffscreen (s, st);
            }

        if (fusionShader != nullptr)
            for (auto* s : fusionSources)
            {
                const FusionShaderState st = s->getFusionState();
                if (st.offscreen && st.active && st.w > 1 && st.h > 1)
                    renderFusionOffscreen (s, st);
            }

        // back to the window's framebuffer for the on-screen pass
        glBindFramebuffer (GL_FRAMEBUFFER, context.getFrameBufferID());

        // full-window clear to the theme background (gaps + Synesthesia base)
        glDisable (GL_SCISSOR_TEST);
        juce::OpenGLHelpers::clear (AlterTheme::bgVoid);

        const float scale = (float) context.getRenderingScale();
        const int   hostH = comp->getHeight();

        // ── pass 2: inline fractals drawn straight into their tiles ──────────
        if (shader != nullptr)
        {
            shader->use();
            glDisable (GL_BLEND);          // fractal is opaque; overwrite its tile
            glEnable  (GL_SCISSOR_TEST);

            for (auto* s : sources)
            {
                const SynShaderState st = s->getSynState();
                if (st.offscreen || ! st.active || st.w <= 0 || st.h <= 0)
                    continue;

                const int vx = juce::roundToInt (st.x * scale);
                const int vy = juce::roundToInt ((hostH - (st.y + st.h)) * scale);   // GL origin = bottom-left
                const int vw = juce::roundToInt (st.w * scale);
                const int vh = juce::roundToInt (st.h * scale);
                if (vw <= 0 || vh <= 0) continue;

                glViewport (vx, vy, vw, vh);
                glScissor  (vx, vy, vw, vh);
                setFractalUniforms (st, vx, vy, vw, vh);

                glBindVertexArray (quadVAO);
                glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
                glBindVertexArray (0);
            }
        }

        // ── pass 3: inline spectrograms, straight from their history textures ─
        if (sgShader != nullptr)
        {
            sgShader->use();
            glDisable (GL_BLEND);          // opaque; overwrites its tile
            glEnable  (GL_SCISSOR_TEST);

            for (auto* s : sgSources)
            {
                const SgShaderState st = s->getSgState();
                if (st.offscreen || ! st.active || st.w <= 0 || st.h <= 0)
                    continue;

                const int vx = juce::roundToInt (st.x * scale);
                const int vy = juce::roundToInt ((hostH - (st.y + st.h)) * scale);
                const int vw = juce::roundToInt (st.w * scale);
                const int vh = juce::roundToInt (st.h * scale);
                if (vw <= 0 || vh <= 0) continue;

                // How many framebuffer pixels the module's frequency axis covers:
                // the rect's height normally, its width when the panel is rotated
                // by 90/270 degrees (then local-y runs across the screen).
                const float lyPx = (std::abs (st.localDV.y) >= std::abs (st.localDU.y))
                                       ? (float) vh : (float) vw;

                SgFrameData fd;
                if (! s->sgGlPrepare (context, 0, 1, lyPx, fd))
                    continue;

                glViewport (vx, vy, vw, vh);
                glScissor  (vx, vy, vw, vh);
                setSgUniforms (st, fd, vx, vy, vw, vh);

                glBindVertexArray (quadVAO);
                glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
                glBindVertexArray (0);
            }

            // leave the sampler state as JUCE expects to find it
            glActiveTexture (GL_TEXTURE1); glBindTexture (GL_TEXTURE_2D, 0);
            glActiveTexture (GL_TEXTURE0); glBindTexture (GL_TEXTURE_2D, 0);
        }

        // ── pass 3b: inline Fusion, straight from the layer textures ─────────
        //
        // One fullscreen draw per module at NATIVE resolution: no render pixel
        // budget, so no upscale and no blocks. The layers were uploaded by
        // fusionGlPrepare, which also decides whether anything actually changed.
        if (fusionShader != nullptr)
        {
            fusionShader->use();
            glDisable (GL_BLEND);          // opaque; overwrites its tile
            glEnable  (GL_SCISSOR_TEST);

            for (auto* s : fusionSources)
            {
                const FusionShaderState st = s->getFusionState();
                if (st.offscreen || ! st.active || st.w <= 0 || st.h <= 0)
                    continue;

                const int vx = juce::roundToInt (st.x * scale);
                const int vy = juce::roundToInt ((hostH - (st.y + st.h)) * scale);
                const int vw = juce::roundToInt (st.w * scale);
                const int vh = juce::roundToInt (st.h * scale);
                if (vw <= 0 || vh <= 0) continue;

                FusionFrameData fd;
                if (! s->fusionGlPrepare (context, 0, fd) || fd.layerCount <= 0)
                    continue;

                glViewport (vx, vy, vw, vh);
                glScissor  (vx, vy, vw, vh);
                setFusionUniforms (st, fd, vx, vy, vw, vh, 0);

                glBindVertexArray (quadVAO);
                glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
                glBindVertexArray (0);
            }

            for (int i = IFusionShaderSource::kMaxLayers - 1; i >= 0; --i)
            {
                glActiveTexture ((GLenum) (GL_TEXTURE0 + i));
                glBindTexture (GL_TEXTURE_2D, 0);
            }
            glActiveTexture (GL_TEXTURE0);
        }

        // ── pass 4: inline geometry (shapes -> glow -> bloom -> composite) ────
        if (geoShader != nullptr)
        {
            for (auto* s : geoSources)
            {
                const GeoShaderState st = s->getGeoState();
                if (st.offscreen || ! st.active || st.w <= 0 || st.h <= 0)
                    continue;

                const int vx = juce::roundToInt (st.x * scale);
                const int vy = juce::roundToInt ((hostH - (st.y + st.h)) * scale);
                const int vw = juce::roundToInt (st.w * scale);
                const int vh = juce::roundToInt (st.h * scale);
                if (vw <= 0 || vh <= 0) continue;

                drawGeoModule (s, st, vw, vh, context.getFrameBufferID(), vx, vy);
            }

            glActiveTexture (GL_TEXTURE2); glBindTexture (GL_TEXTURE_2D, 0);
            glActiveTexture (GL_TEXTURE1); glBindTexture (GL_TEXTURE_2D, 0);
            glActiveTexture (GL_TEXTURE0); glBindTexture (GL_TEXTURE_2D, 0);
        }

        // restore state for JUCE's component painting pass
        glDisable (GL_SCISSOR_TEST);
        glEnable  (GL_BLEND);
        glViewport (0, 0, juce::roundToInt (comp->getWidth()  * scale),
                          juce::roundToInt (comp->getHeight() * scale));
    }

    // ═══════════════════════ FUSION LAYER TARGETS ════════════════════════════
    //
    // One framebuffer per module that is currently somebody's fusion layer, kept
    // alive between frames and never read back. See the note in renderOpenGL for
    // why the round trip through main memory had to go.
    //
    // Keyed by COMPONENT and not by shader-source interface, because a fusion
    // knows its layers as components and the three GL modules implement three
    // different interfaces. The map is touched only on the GL thread, under the
    // source lock the whole frame runs inside.

    /** GL THREAD. The texture holding this layer's last frame, or 0 if this
        module is not being rendered as a layer (a 2D module, or one whose
        framebuffer could not be allocated). Asked by FusionVisual, which uploads
        a frame only when this comes back empty. */
public:
    struct LayerTexture { unsigned int id = 0; int w = 0, h = 0; };

    LayerTexture getFusionLayerTexture (juce::Component* c) const noexcept
    {
        if (c == nullptr) return {};
        const auto it = layerTargets.find (c);
        if (it == layerTargets.end() || it->second.fb == nullptr || ! it->second.fb->isValid())
            return {};
        return { it->second.fb->getTextureID(),
                 it->second.fb->getWidth(), it->second.fb->getHeight() };
    }

private:
    struct LayerTarget { std::unique_ptr<juce::OpenGLFrameBuffer> fb; };

    std::map<juce::Component*, LayerTarget> layerTargets;   // GL thread only
    std::set<juce::Component*>              layerSet;       // rebuilt every frame

    /** Is this shader source one of somebody's fusion layers? Every GL module is
        also a Component, which is the one thing all three have in common and the
        only key a fusion could have named them by. */
    template <typename SourceT>
    bool isFusionLayer (SourceT* s) const noexcept
    {
        auto* c = dynamic_cast<juce::Component*> (s);
        return c != nullptr && layerSet.find (c) != layerSet.end();
    }

    /** GL THREAD. Who is a layer this frame, and drop the framebuffers of anyone
        who has stopped being one — a module pulled back into the HUD must not go
        on holding a full-size target nobody samples. */
    void collectFusionLayers()
    {
        layerSet.clear();

        for (auto* f : fusionSources)
        {
            const FusionShaderState st = f->getFusionState();
            if (! st.active) continue;

            juce::Component* ls[IFusionShaderSource::kMaxLayers] = {};
            f->getFusionLayerComponents (ls, IFusionShaderSource::kMaxLayers);

            for (auto* c : ls)
                if (c != nullptr)
                    layerSet.insert (c);
        }

        for (auto it = layerTargets.begin(); it != layerTargets.end(); )
            it = (layerSet.find (it->first) == layerSet.end()) ? layerTargets.erase (it)
                                                              : std::next (it);
    }

    /** GL THREAD. A layer's framebuffer at the size it wants, created on demand
        and resized only when the module's own size changes. */
    juce::OpenGLFrameBuffer* layerFB (juce::Component* c, int w, int h)
    {
        if (c == nullptr || w < 1 || h < 1) return nullptr;

        auto& t = layerTargets[c];
        if (t.fb == nullptr)
            t.fb = std::make_unique<juce::OpenGLFrameBuffer>();

        if (! ensureFB (*t.fb, context, w, h))
            return nullptr;

        return t.fb.get();
    }

    /** GL THREAD. Draw every GPU-backed layer into its own target.
        The three modules do not share a base class, so this is three small
        branches rather than a virtual call — the same trade FusionVisual makes
        when it asks which kind of module it is holding. */
    void renderFusionLayerTargets()
    {
        using namespace juce::gl;

        for (auto* c : layerSet)
        {
            if (auto* s = dynamic_cast<ISynShaderSource*> (c))
            {
                if (shader == nullptr) continue;
                const SynShaderState st = s->getSynState();
                // A layer is always offscreen (setLayerMode forces it). Insisting on it
                // keeps st.w/st.h meaning the module's OWN size and not a rectangle
                // in the host's coordinates.
                if (! st.offscreen || ! st.active || st.w < 2 || st.h < 2) continue;

                auto* fb = layerFB (c, st.w, st.h);
                if (fb == nullptr) continue;

                fb->makeCurrentRenderingTarget();
                glViewport (0, 0, st.w, st.h);
                glDisable (GL_SCISSOR_TEST);
                clearOffscreen (st.noBackground);

                shader->use();
                glDisable (GL_BLEND);
                setFractalUniforms (st, 0, 0, st.w, st.h);
                glBindVertexArray (quadVAO);
                glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
                glBindVertexArray (0);
                fb->releaseAsRenderingTarget();
                continue;
            }

            if (auto* s = dynamic_cast<IGeoShaderSource*> (c))
            {
                if (geoShader == nullptr) continue;
                const GeoShaderState st = s->getGeoState();
                if (! st.offscreen || ! st.active || st.w < 2 || st.h < 2) continue;

                auto* fb = layerFB (c, st.w, st.h);
                if (fb == nullptr) continue;

                // Geometry binds several intermediate framebuffers of its own on
                // the way, so it is handed the target's id and puts itself back.
                fb->makeCurrentRenderingTarget();
                drawGeoModule (s, st, st.w, st.h, fb->getFrameBufferID(), 0, 0);
                fb->releaseAsRenderingTarget();
                continue;
            }

            if (auto* s = dynamic_cast<ISgShaderSource*> (c))
            {
                if (sgShader == nullptr) continue;
                const SgShaderState st = s->getSgState();
                if (! st.offscreen || ! st.active || st.w < 2 || st.h < 2) continue;

                const float lyPx = (std::abs (st.localDV.y) >= std::abs (st.localDU.y))
                                       ? (float) st.h : (float) st.w;

                SgFrameData fd;
                if (! s->sgGlPrepare (context, 0, 1, lyPx, fd))
                    continue;

                auto* fb = layerFB (c, st.w, st.h);
                if (fb == nullptr) continue;

                fb->makeCurrentRenderingTarget();
                glViewport (0, 0, st.w, st.h);
                glDisable (GL_SCISSOR_TEST);
                clearOffscreen (st.noBackground);

                sgShader->use();
                glDisable (GL_BLEND);
                setSgUniforms (st, fd, 0, 0, st.w, st.h);
                glBindVertexArray (quadVAO);
                glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
                glBindVertexArray (0);

                glActiveTexture (GL_TEXTURE1); glBindTexture (GL_TEXTURE_2D, 0);
                glActiveTexture (GL_TEXTURE0); glBindTexture (GL_TEXTURE_2D, 0);
                fb->releaseAsRenderingTarget();
            }
        }
    }

    /** Clears the offscreen FBO for one module's frame.

        A DETACHED window wants the theme background, exactly as it looks in the
        HUD. An FUSION LAYER wants nothing at all: whatever is here survives into
        the fusion's alpha channel, so a background-coloured clear would arrive as
        opaque background everywhere the module happened not to draw — which is the
        very thing the layer is trying to stop doing. */
    static void clearOffscreen (bool noBackground)
    {
        if (noBackground)
        {
            juce::gl::glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
            juce::gl::glClear (juce::gl::GL_COLOR_BUFFER_BIT);
            return;
        }

        juce::OpenGLHelpers::clear (AlterTheme::bgVoid);
    }

    // Render one spectrogram into the offscreen FBO and hand the image back.
    void renderSgOffscreen (ISgShaderSource* s, const SgShaderState& st)
    {
        using namespace juce::gl;
        const int w = st.w, h = st.h;

        // THROTTLED to ~35 Hz. This path ends in a glReadPixels, which stalls the
        // GL pipeline for as long as the readback takes — and this is the HUD's
        // context, so paying that on every swap would stutter every OTHER module
        // to serve one detached window. A detached spectrogram at 35 Hz is
        // indistinguishable; the HUD staying at full refresh is not.
        {
            const auto now = juce::Time::getMillisecondCounter();
            auto& last = sgOffscreenLastMs[s];
            if (last != 0 && now - last < 28) return;
            last = now;
        }

        const float lyPx = (std::abs (st.localDV.y) >= std::abs (st.localDU.y))
                               ? (float) h : (float) w;

        SgFrameData fd;
        if (! s->sgGlPrepare (context, 0, 1, lyPx, fd))
            return;

        auto* fb = offscreenFor (s, w, h);
        if (fb == nullptr)
            return;

        fb->makeCurrentRenderingTarget();
        glViewport (0, 0, w, h);
        glDisable (GL_SCISSOR_TEST);
        // A layer's frame has to START empty, or the clear colour would arrive in
        // the fusion as opaque background everywhere the module drew nothing.
        clearOffscreen (st.noBackground);

        sgShader->use();
        glDisable (GL_BLEND);
        setSgUniforms (st, fd, 0, 0, w, h);
        glBindVertexArray (quadVAO);
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray (0);

        glActiveTexture (GL_TEXTURE1); glBindTexture (GL_TEXTURE_2D, 0);
        glActiveTexture (GL_TEXTURE0); glBindTexture (GL_TEXTURE_2D, 0);

        fb->releaseAsRenderingTarget();

        std::vector<juce::PixelARGB> buf ((size_t) w * (size_t) h);
        fb->readPixels (buf.data(), juce::Rectangle<int> (0, 0, w, h),
                                juce::OpenGLFrameBuffer::RowOrder::fromTopDown);

        juce::Image img (juce::Image::ARGB, w, h, false, juce::SoftwareImageType());
        {
            juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < h; ++y)
                std::memcpy (bd.getLinePointer (y),
                             buf.data() + (size_t) y * (size_t) w,
                             sizeof (juce::PixelARGB) * (size_t) w);
        }
        s->deliverSgOffscreenImage (img);
    }

    void setSgUniforms (const SgShaderState& st, const SgFrameData& fd,
                        int offX, int offY, int resW, int resH)
    {
        if (uSgRes     != nullptr) uSgRes->set     ((float) resW, (float) resH);
        if (uSgOffset  != nullptr) uSgOffset->set  ((float) offX, (float) offY);
        if (uSgHistory != nullptr) uSgHistory->set (0);
        if (uSgLut     != nullptr) uSgLut->set     (1);
        if (uSgLocal0  != nullptr) uSgLocal0->set  (st.local0.x,  st.local0.y);
        if (uSgLocalDU != nullptr) uSgLocalDU->set (st.localDU.x, st.localDU.y);
        if (uSgLocalDV != nullptr) uSgLocalDV->set (st.localDV.x, st.localDV.y);
        if (uSgHead    != nullptr) uSgHead->set    (fd.headCol);
        if (uSgVisCols != nullptr) uSgVisCols->set (fd.visCols);
        if (uSgBufCols != nullptr) uSgBufCols->set (fd.bufCols);
        if (uSgFilled  != nullptr) uSgFilled->set  (fd.filled);
        if (uSgRowSpan != nullptr) uSgRowSpan->set (fd.rowSpan);
        if (uSgYTaps   != nullptr) uSgYTaps->set   (fd.yTaps);

        // same vertical gradient AlterTheme::paintBackground draws, so the
        // not-yet-filled part of the window matches every other module exactly
        const juce::Colour mid = AlterTheme::bgDeep.interpolatedWith (AlterTheme::bgVoid, 0.5f);
        if (uSgBgIn  != nullptr) uSgBgIn->set  (AlterTheme::bgDeep.getFloatRed(),
                                                AlterTheme::bgDeep.getFloatGreen(),
                                                AlterTheme::bgDeep.getFloatBlue());
        if (uSgBgMid != nullptr) uSgBgMid->set (mid.getFloatRed(), mid.getFloatGreen(), mid.getFloatBlue());
        if (uSgBgOut != nullptr) uSgBgOut->set (AlterTheme::bgVoid.getFloatRed(),
                                                AlterTheme::bgVoid.getFloatGreen(),
                                                AlterTheme::bgVoid.getFloatBlue());

        // ANDed with offscreen on purpose: a layer always renders through the FBO,
        // so this can never make the module transparent while it is still being
        // drawn inline in the HUD.
        if (uSgNoBg != nullptr) uSgNoBg->set ((st.noBackground && st.offscreen) ? 1.0f : 0.0f);
    }

    // ═══════════════════════ FUSION (fuses other modules) ═══════════════════
    //
    // One fullscreen pass at NATIVE resolution. The source hands over its layer
    // textures and a block of parameters; the whole chain — background knock-out,
    // warp, merge, weave, mirror — lives in the fragment shader. See FusionShader.h
    // for why this belongs on the GPU at all.
    void buildFusionShader()
    {
        auto sp = std::make_unique<juce::OpenGLShaderProgram> (context);

        if (sp->addVertexShader (FusionShaderSource::vertex())
            && sp->addFragmentShader (FusionShaderSource::fragment())
            && sp->link())
        {
            fusionShader = std::move (sp);

            // A uniform the compiled shader does not actually reference has no
            // location, and JUCE's Uniform constructor asserts on that — an easy
            // way to crash the app by deleting one line of GLSL. Looking the name
            // up first turns that into a null pointer, which every setter below
            // already tolerates.
            auto u = [this] (const char* name) -> std::unique_ptr<juce::OpenGLShaderProgram::Uniform>
            {
                if (juce::gl::glGetUniformLocation (fusionShader->getProgramID(), name) < 0)
                    return {};

                return std::unique_ptr<juce::OpenGLShaderProgram::Uniform>
                       (new juce::OpenGLShaderProgram::Uniform (*fusionShader, name));
            };

            uFusionRes         = u ("iResolution");
            uFusionOffset      = u ("iOffset");
            uFusionL0          = u ("uLayer0");
            uFusionL1          = u ("uLayer1");
            uFusionL2          = u ("uLayer2");
            uFusionCount       = u ("uLayerCount");
            uFusionLocal0      = u ("uLocal0");
            uFusionLocalDU     = u ("uLocalDU");
            uFusionLocalDV     = u ("uLocalDV");
            uFusionNoBg        = u ("uNoBg");
            uFusionBgIn        = u ("uBgIn");
            uFusionBgMid       = u ("uBgMid");
            uFusionBgOut       = u ("uBgOut");
            uFusionAspect      = u ("uAspect");

            // Per layer, packed: (blend, opacity, rot, scale) /
            // (offX, offY, amount, hasAlpha) / (bands, angle, edge, symmetry) /
            // (mirrorCount, mirrorAngle, spin, zoom).
            const char* an[3] = { "uL0a", "uL1a", "uL2a" };
            const char* bn[3] = { "uL0b", "uL1b", "uL2b" };
            const char* cn[3] = { "uL0c", "uL1c", "uL2c" };
            const char* dn[3] = { "uL0d", "uL1d", "uL2d" };
            for (int i = 0; i < 3; ++i)
            {
                uFusionLayA[i] = u (an[i]);
                uFusionLayB[i] = u (bn[i]);
                uFusionLayC[i] = u (cn[i]);
                uFusionLayD[i] = u (dn[i]);
            }

            uFusionWarp        = u ("uWarp");
            uFusionWarpAmt     = u ("uWarpAmt");
            uFusionWarpSwirl   = u ("uWarpSwirl");
            uFusionWarpSmooth  = u ("uWarpSmooth");
            uFusionWarpDenoise = u ("uWarpDenoise");
            uFusionWarpSrc     = u ("uWarpSrc");

            uFusionSymmetry    = u ("uSymmetry");
            uFusionMirror      = u ("uMirror");
            uFusionMirrorAngle = u ("uMirrorAngle");
            uFusionSpin        = u ("uSpin");
            uFusionZoom        = u ("uZoom");
            uFusionVortex      = u ("uVortex");
            uFusionDrive       = u ("uDrive");

            uFusionGlobLayers   = u ("uGlobLayers");
            uFusionLiquid       = u ("uLiquid");
            uFusionLiquidAmt    = u ("uLiquidAmt");
            uFusionLiquidSmooth = u ("uLiquidSmooth");
            uFusionLiquidDenoise = u ("uLiquidDenoise");
            uFusionTunnel       = u ("uTunnel");
            uFusionTime         = u ("uTime");
        }
    }

    void setFusionUniforms (const FusionShaderState& st, const FusionFrameData& fd,
                         int offX, int offY, int resW, int resH, int firstUnit)
    {
        auto set1 = [] (const std::unique_ptr<juce::OpenGLShaderProgram::Uniform>& u, float v)
        { if (u != nullptr) u->set (v); };

        if (uFusionRes    != nullptr) uFusionRes->set    ((float) resW, (float) resH);
        if (uFusionOffset != nullptr) uFusionOffset->set ((float) offX, (float) offY);
        if (uFusionL0     != nullptr) uFusionL0->set     (firstUnit);
        if (uFusionL1     != nullptr) uFusionL1->set     (firstUnit + 1);
        if (uFusionL2     != nullptr) uFusionL2->set     (firstUnit + 2);

        set1 (uFusionCount, (float) fd.layerCount);

        // Per layer, in the same compacted order as the texture units.
        for (int i = 0; i < IFusionShaderSource::kMaxLayers; ++i)
        {
            const auto& L = fd.layer[i];
            if (uFusionLayA[i] != nullptr) uFusionLayA[i]->set (L.blend, L.opacity, L.rot, L.scale);
            // hasAlpha and fromFbo travel packed in one component — see the note
            // beside srcHasAlpha in the shader. There is no fourth free slot here
            // and the shader reads them one line apart.
            if (uFusionLayB[i] != nullptr) uFusionLayB[i]->set (L.offX, L.offY, L.amount,
                                                                L.hasAlpha + 2.0f * L.fromFbo);
            if (uFusionLayC[i] != nullptr) uFusionLayC[i]->set (L.bands, L.angle, L.edge, L.symmetry);
            if (uFusionLayD[i] != nullptr) uFusionLayD[i]->set (L.mirror, L.mirrorAngle, L.spin, L.zoom);
        }

        if (uFusionLocal0  != nullptr) uFusionLocal0->set  (st.local0.x,  st.local0.y);
        if (uFusionLocalDU != nullptr) uFusionLocalDU->set (st.localDU.x, st.localDU.y);
        if (uFusionLocalDV != nullptr) uFusionLocalDV->set (st.localDV.x, st.localDV.y);

        // The module's own background: the same vertical gradient
        // AlterTheme::paintBackground draws, so Fusion sits in the HUD like every
        // other module and follows the theme.
        // Only meaningful offscreen: a fusion drawn straight into the HUD window has
        // nothing behind it to be transparent against.
        if (uFusionNoBg != nullptr) uFusionNoBg->set ((st.noBackground && st.offscreen) ? 1.0f : 0.0f);

        const juce::Colour mid = AlterTheme::bgDeep.interpolatedWith (AlterTheme::bgVoid, 0.5f);
        if (uFusionBgIn  != nullptr) uFusionBgIn->set  (AlterTheme::bgDeep.getFloatRed(),
                                                  AlterTheme::bgDeep.getFloatGreen(),
                                                  AlterTheme::bgDeep.getFloatBlue());
        if (uFusionBgMid != nullptr) uFusionBgMid->set (mid.getFloatRed(), mid.getFloatGreen(), mid.getFloatBlue());
        if (uFusionBgOut != nullptr) uFusionBgOut->set (AlterTheme::bgVoid.getFloatRed(),
                                                  AlterTheme::bgVoid.getFloatGreen(),
                                                  AlterTheme::bgVoid.getFloatBlue());

        // The MODULE's aspect, not the viewport's. On a panel rotated 90 or 270
        // degrees the viewport is the bounding box of the rotated rectangle, so its
        // width and height are swapped relative to the module's own local space —
        // and Mirror works in local space. Feeding it the viewport's aspect is what
        // would squash the wedges the moment the panel was turned.
        set1 (uFusionAspect, juce::jmax (0.01f, st.localAspect));

        set1 (uFusionWarp,       fd.warp);
        set1 (uFusionWarpAmt,    fd.warpAmt);
        set1 (uFusionWarpSwirl,  fd.warpSwirl);
        set1 (uFusionWarpSmooth, fd.warpSmooth);
        set1 (uFusionWarpDenoise, fd.warpDenoise);
        set1 (uFusionWarpSrc,    fd.warpSrc);

        set1 (uFusionSymmetry,   fd.symmetry);
        set1 (uFusionMirror,     fd.mirror);
        set1 (uFusionMirrorAngle,fd.mirrorAngle);
        set1 (uFusionSpin,       fd.spin);
        set1 (uFusionZoom,       fd.zoom);
        set1 (uFusionVortex,     fd.vortex);
        set1 (uFusionDrive,      fd.drive);

        if (uFusionGlobLayers != nullptr)
            uFusionGlobLayers->set (fd.globLayer[0], fd.globLayer[1], fd.globLayer[2]);
        set1 (uFusionLiquid,       fd.liquid);
        set1 (uFusionLiquidAmt,    fd.liquidAmt);
        set1 (uFusionLiquidSmooth, fd.liquidSmooth);
        set1 (uFusionLiquidDenoise, fd.liquidDenoise);
        set1 (uFusionTunnel,       fd.tunnel);
        set1 (uFusionTime,         fd.time);
    }

    void renderFusionOffscreen (IFusionShaderSource* s, const FusionShaderState& st)
    {
        using namespace juce::gl;
        const int w = st.w, h = st.h;

        // THROTTLED, for the same reason as the spectrogram's offscreen path: it
        // ends in a glReadPixels, which stalls the pipeline of the HUD's context
        // to serve one detached window.
        {
            const auto now = juce::Time::getMillisecondCounter();
            auto& last = fusionOffscreenLastMs[s];
            if (last != 0 && now - last < 28) return;
            last = now;
        }

        FusionFrameData fd;
        if (! s->fusionGlPrepare (context, 0, fd) || fd.layerCount <= 0)
            return;

        auto* fb = offscreenFor (s, w, h);
        if (fb == nullptr)
            return;

        fb->makeCurrentRenderingTarget();
        glViewport (0, 0, w, h);
        glDisable (GL_SCISSOR_TEST);
        clearOffscreen (st.noBackground);

        fusionShader->use();
        glDisable (GL_BLEND);
        setFusionUniforms (st, fd, 0, 0, w, h, 0);
        glBindVertexArray (quadVAO);
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray (0);

        for (int i = IFusionShaderSource::kMaxLayers - 1; i >= 0; --i)
        {
            glActiveTexture ((GLenum) (GL_TEXTURE0 + i));
            glBindTexture (GL_TEXTURE_2D, 0);
        }

        fb->releaseAsRenderingTarget();

        // Same readback as the spectrogram's: fromTopDown, because the component
        // that blits this image works top-down while GL hands pixels back bottom-up.
        std::vector<juce::PixelARGB> buf ((size_t) w * (size_t) h);
        fb->readPixels (buf.data(), juce::Rectangle<int> (0, 0, w, h),
                                juce::OpenGLFrameBuffer::RowOrder::fromTopDown);

        juce::Image img (juce::Image::ARGB, w, h, false, juce::SoftwareImageType());
        {
            juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < h; ++y)
                std::memcpy (bd.getLinePointer (y),
                             buf.data() + (size_t) y * (size_t) w,
                             sizeof (juce::PixelARGB) * (size_t) w);
        }
        s->deliverFusionOffscreenImage (img);
    }

    // ═══════════════════════ GEOMETRY (instanced + bloom) ════════════════════
    //
    // One module, start to finish:
    //
    //   geoSceneFB   <- the crisp strokes, one draw call
    //   geoDown0/1   <- exact 2x2 box averages down to 1/4
    //   geoVeilTmp   <- horizontal gaussian into 1/8
    //   geoVeil      <- vertical gaussian; this is the blurred veil
    //   target       <- composite: theme gradient, veil over it, crisp strokes over
    //                   both — the same order and the same maths the CPU path used
    //
    // Everything but the stroke pass is a full-tile quad at ever smaller sizes, so
    // a module costs six draw calls no matter how many shapes are alive or how
    // large it has been dragged.

    static bool ensureFB (juce::OpenGLFrameBuffer& fb, juce::OpenGLContext& ctx, int w, int h)
    {
        w = juce::jmax (1, w);
        h = juce::jmax (1, h);
        if (fb.getWidth() != w || fb.getHeight() != h)
        {
            fb.release();
            fb.initialise (ctx, w, h);
        }
        return fb.isValid();
    }

    // Bind an FBO's colour texture to a unit with the sampling state these passes
    // rely on. Set explicitly rather than trusting whatever the last binder left
    // behind: the blur and composite both sample BETWEEN texels, and a stray
    // GL_NEAREST or a REPEAT wrap would show up as blocky or wrapped haloes.
    static void bindFbTexture (juce::OpenGLFrameBuffer& fb, int unit)
    {
        using namespace juce::gl;
        glActiveTexture ((GLenum) (GL_TEXTURE0 + unit));
        glBindTexture (GL_TEXTURE_2D, fb.getTextureID());
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    void drawGeoModule (IGeoShaderSource* s, const GeoShaderState& st,
                        int vw, int vh, unsigned int targetFB, int tx, int ty)
    {
        using namespace juce::gl;

        if (geoShader == nullptr || geoCompShader == nullptr)
            return;

        // The source uploads its instance array and binds it to unit 0.
        glActiveTexture (GL_TEXTURE0);
        GeoFrameData fd;
        if (! s->geoGlPrepare (context, 0, fd))
            return;

        if (! ensureFB (geoSceneFB, context, vw, vh))
            return;

        // ── shapes -> scene FBO ──────────────────────────────────────────────
        geoSceneFB.makeCurrentRenderingTarget();
        glViewport (0, 0, vw, vh);
        glDisable (GL_SCISSOR_TEST);
        glClearColor (0.0f, 0.0f, 0.0f, 0.0f);      // transparent: the background
        glClear (GL_COLOR_BUFFER_BIT);              // is added by the composite

        const int segs  = juce::jlimit (12, 240, fd.segments);
        const int count = juce::jmax (0, fd.instanceCount);

        if (count > 0)
        {
            geoShader->use();
            if (uGeoInst       != nullptr) uGeoInst->set (fd.texUnit);
            if (uGeoInstTexW   != nullptr) uGeoInstTexW->set (kGeoInstTexW);
            if (uGeoModuleSize != nullptr) uGeoModuleSize->set ((float) juce::jmax (1, st.w),
                                                                (float) juce::jmax (1, st.h));
            if (uGeoSegs       != nullptr) uGeoSegs->set (segs);
            if (uGeoAaPad      != nullptr) uGeoAaPad->set (2.0f);

            // ONE pass: the crisp stroke. Premultiplied "over" — instances are
            // emitted back-to-front by the worker and a single glDrawArrays
            // preserves primitive order, so the painter's ordering survives without
            // one draw call per shape.
            //
            // There is deliberately NO per-shape glow pass. Widening the ring and
            // fading across it gives a halo with a DEFINED OUTER EDGE, and the eye
            // reads that edge as a second contour — the shape looked like it had
            // been drawn again a few pixels out, which is not what glow looks like.
            // All the light now comes from the blur pyramid below, whose falloff
            // never terminates anywhere.
            glEnable (GL_BLEND);
            glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glBindVertexArray (geoVAO);
            glDrawArrays (GL_TRIANGLES, 0, (GLsizei) count * (GLsizei) segs * 6);
            glBindVertexArray (0);
        }

        // ── the veil ─────────────────────────────────────────────────────────
        // A faithful port of what the CPU renderer did, because that is the look
        // this module is supposed to have:
        //
        //   CPU: downscale the scene to 1/6, box-blur it twice with radius
        //        2 + bloom*6, multiply EVERY byte (colour AND alpha) by
        //        1.8 + bloom*2.2, then draw it under the crisp strokes at
        //        opacity 0.60 + bloom*0.40.
        //
        // Two box passes of radius r have variance (2r^2 + 2r)/3, so the sigma
        // being reproduced is sqrt((2r^2+2r)/3) sixth-res pixels — 12 to 42 pixels
        // of the module at bloom 0 and 1. That figure is what the eye judges, so it
        // is matched exactly; the buffer is 1/8 rather than 1/6 only because powers
        // of two let each downsample be an EXACT 2x2 average (one bilinear tap
        // landing on a texel corner), which the single-step 6x minification would
        // not be — thin distant strokes would have sparkled in and out of the veil.
        const bool wantBloom = (st.bloom > 0.001f) && count > 0 && geoBlurShader != nullptr;
        bool haveBloom = false;

        if (wantBloom)
        {
            const int d0w = juce::jmax (1, vw / 2), d0h = juce::jmax (1, vh / 2);
            const int d1w = juce::jmax (1, vw / 4), d1h = juce::jmax (1, vh / 4);
            const int vlw = juce::jmax (1, vw / 8), vlh = juce::jmax (1, vh / 8);

            if (ensureFB (geoDown0,   context, d0w, d0h)
             && ensureFB (geoDown1,   context, d1w, d1h)
             && ensureFB (geoVeilTmp, context, vlw, vlh)
             && ensureFB (geoVeil,    context, vlw, vlh))
            {
                // The CPU's two box passes, expressed as the gaussian they add up to.
                // Its radius was 2 + round(bloom*6); the rounding is dropped here on
                // purpose, so the width grows continuously instead of stepping — the
                // curve passes through the CPU's value exactly at every integer
                // radius and merely interpolates between them.
                const float r         = 2.0f + st.bloom * 6.0f;
                const float sigmaLow  = std::sqrt ((2.0f * r * r + 2.0f * r) / 3.0f);
                const float sigmaTile = 6.0f * sigmaLow;    // module pixels

                // The 3x3 tent the composite reads the veil with is itself a blur:
                // variance 0.5 veil texels per axis, i.e. sigma 0.707 * 8 = 5.66
                // module pixels. Blurs add in QUADRATURE, so take it back out here
                // or the veil would come out wider than the CPU's.
                const float sigmaTent = 0.7071f * 8.0f;
                const float sigmaMain = std::sqrt (juce::jmax (1.0f,
                                            sigmaTile * sigmaTile - sigmaTent * sigmaTent));

                // The 5-bilinear-tap kernel in the blur shader has
                //   variance = 2*(0.31622*1.38462^2 + 0.07027*3.23077^2) = 2.6794
                // so its sigma is 1.6369 * uStep — NOT 2 * uStep, which is the value
                // the offsets superficially suggest.
                const float step      = sigmaMain / 1.6369f;

                geoBlurShader->use();
                glDisable (GL_BLEND);
                if (uGbTex    != nullptr) uGbTex->set (1);
                if (uGbOffset != nullptr) uGbOffset->set (0.0f, 0.0f);
                glBindVertexArray (quadVAO);

                // uStep = 0 collapses the kernel to a single tap (the weights sum to
                // 1), and one bilinear tap at a 2x downscale lands exactly on a texel
                // corner — so these two passes are exact 2x2 box averages.
                if (uGbStep != nullptr) uGbStep->set (0.0f, 0.0f);

                geoDown0.makeCurrentRenderingTarget();
                glViewport (0, 0, d0w, d0h);
                bindFbTexture (geoSceneFB, 1);
                if (uGbRes != nullptr) uGbRes->set ((float) d0w, (float) d0h);
                glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

                geoDown1.makeCurrentRenderingTarget();
                glViewport (0, 0, d1w, d1h);
                bindFbTexture (geoDown0, 1);
                if (uGbRes != nullptr) uGbRes->set ((float) d1w, (float) d1h);
                glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

                // Separable gaussian at 1/8. The offsets are expressed against the
                // MODULE's own size, so the veil is the same width in module pixels
                // whatever the display scale — the scale cancels.
                if (uGbRes != nullptr) uGbRes->set ((float) vlw, (float) vlh);

                geoVeilTmp.makeCurrentRenderingTarget();
                glViewport (0, 0, vlw, vlh);
                bindFbTexture (geoDown1, 1);
                if (uGbStep != nullptr) uGbStep->set (step / (float) juce::jmax (1, st.w), 0.0f);
                glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

                geoVeil.makeCurrentRenderingTarget();
                glViewport (0, 0, vlw, vlh);
                bindFbTexture (geoVeilTmp, 1);
                if (uGbStep != nullptr) uGbStep->set (0.0f, step / (float) juce::jmax (1, st.h));
                glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);

                glBindVertexArray (0);
                haveBloom = true;
            }
        }

        // ── composite -> the caller's framebuffer ────────────────────────────
        glBindFramebuffer (GL_FRAMEBUFFER, targetFB);
        glViewport (tx, ty, vw, vh);
        if (targetFB != 0)
        {
            glEnable (GL_SCISSOR_TEST);
            glScissor (tx, ty, vw, vh);
        }
        else
        {
            glDisable (GL_SCISSOR_TEST);
        }

        geoCompShader->use();
        glDisable (GL_BLEND);                       // the composite is opaque

        bindFbTexture (geoSceneFB, 1);
        bindFbTexture (haveBloom ? geoVeil : geoSceneFB, 2);

        if (uGcScene  != nullptr) uGcScene->set (1);
        if (uGcVeil   != nullptr) uGcVeil->set  (2);
        if (uGcRes    != nullptr) uGcRes->set    ((float) vw, (float) vh);
        if (uGcOffset != nullptr) uGcOffset->set ((float) tx, (float) ty);
        // ANDed with offscreen on purpose: a layer always renders through the FBO,
        // so this can never make the module transparent while it is still being
        // drawn inline in the HUD.
        if (uGcNoBg != nullptr)
            uGcNoBg->set ((st.noBackground && st.offscreen) ? 1.0f : 0.0f);

        if (uGcVeilTexel != nullptr)
            uGcVeilTexel->set (8.0f / (float) juce::jmax (1, vw),
                               8.0f / (float) juce::jmax (1, vh));

        // Blur AVERAGES, so an isolated stroke's halo dims as it spreads; the gain
        // puts that energy back, and where many strokes converge the boosted haloes
        // saturate into one fused core. Same formula the CPU path used.
        if (uGcBloomGain != nullptr)
            uGcBloomGain->set ((1.8f + st.bloom * 2.2f)
                                 * juce::jlimit (0.5f, 1.6f, 0.4f + 0.6f * st.brightness));

        // Veil opacity, also the CPU's — with ONE addition: a fade over the bottom
        // 6% of the knob. The CPU path simply skipped the whole veil below bloom
        // 0.02 and drew it at 60% opacity and 1.84 gain the instant it crossed, so
        // the light appeared at nine tenths of full strength in a single step. That
        // is the blink. Above 0.06 this is byte-identical to the old formula, so the
        // fade costs nothing anywhere the knob is actually used.
        if (uGcVeilOpacity != nullptr)
        {
            const float t    = juce::jlimit (0.0f, 1.0f, st.bloom / 0.06f);
            const float fade = t * t * (3.0f - 2.0f * t);
            uGcVeilOpacity->set (haveBloom ? (0.60f + 0.40f * st.bloom) * fade : 0.0f);
        }

        // The theme's vertical background gradient, reproduced exactly as
        // AlterTheme::paintBackground draws it (bgDeep -> mid at 0.55 -> bgVoid),
        // so the module sits seamlessly beside every 2D module.
        const juce::Colour mid = AlterTheme::bgDeep.interpolatedWith (AlterTheme::bgVoid, 0.5f);
        if (uGcBgIn  != nullptr) uGcBgIn->set  (AlterTheme::bgDeep.getFloatRed(),
                                                AlterTheme::bgDeep.getFloatGreen(),
                                                AlterTheme::bgDeep.getFloatBlue());
        if (uGcBgMid != nullptr) uGcBgMid->set (mid.getFloatRed(), mid.getFloatGreen(), mid.getFloatBlue());
        if (uGcBgOut != nullptr) uGcBgOut->set (AlterTheme::bgVoid.getFloatRed(),
                                                AlterTheme::bgVoid.getFloatGreen(),
                                                AlterTheme::bgVoid.getFloatBlue());

        glBindVertexArray (quadVAO);
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray (0);

        glDisable (GL_SCISSOR_TEST);
        glEnable (GL_BLEND);
    }

    // Render one geometry module into the offscreen FBO and hand the image back.
    void renderGeoOffscreen (IGeoShaderSource* s, const GeoShaderState& st)
    {
        using namespace juce::gl;
        const int w = st.w, h = st.h;

        // THROTTLED to ~35 Hz, for the same reason renderSgOffscreen is: this path
        // ends in a glReadPixels, which stalls the pipeline of the HUD's own
        // context. A detached module at 35 Hz is indistinguishable; the HUD
        // dropping below its refresh to serve one detached window is not.
        {
            const auto now = juce::Time::getMillisecondCounter();
            auto& last = geoOffscreenLastMs[s];
            if (last != 0 && now - last < 28) return;
            last = now;
        }

        auto* fb = offscreenFor (s, w, h);
        if (fb == nullptr)
            return;

        // Bracket the draw with JUCE's own target calls (rather than only binding
        // the id) so its notion of the current target stays in step — drawGeoModule
        // binds several intermediate FBOs of its own along the way.
        fb->makeCurrentRenderingTarget();
        drawGeoModule (s, st, w, h, fb->getFrameBufferID(), 0, 0);
        fb->releaseAsRenderingTarget();

        std::vector<juce::PixelARGB> buf ((size_t) w * (size_t) h);
        fb->readPixels (buf.data(), juce::Rectangle<int> (0, 0, w, h),
                                juce::OpenGLFrameBuffer::RowOrder::fromTopDown);

        juce::Image img (juce::Image::ARGB, w, h, false, juce::SoftwareImageType());
        {
            juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < h; ++y)
                std::memcpy (bd.getLinePointer (y),
                             buf.data() + (size_t) y * (size_t) w,
                             sizeof (juce::PixelARGB) * (size_t) w);
        }
        s->deliverGeoOffscreenImage (img);
    }

    // Render one fractal into the offscreen FBO and hand the image to the source.
    void renderOffscreen (ISynShaderSource* s, const SynShaderState& st)
    {
        using namespace juce::gl;
        const int w = st.w, h = st.h;

        auto* fb = offscreenFor (s, w, h);
        if (fb == nullptr)
            return;

        fb->makeCurrentRenderingTarget();
        glViewport (0, 0, w, h);
        glDisable (GL_SCISSOR_TEST);
        clearOffscreen (st.noBackground);

        shader->use();
        glDisable (GL_BLEND);
        setFractalUniforms (st, 0, 0, w, h);
        glBindVertexArray (quadVAO);
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray (0);

        fb->releaseAsRenderingTarget();

        // read back top-down so it can be blitted directly as a JUCE image
        std::vector<juce::PixelARGB> buf ((size_t) w * (size_t) h);
        fb->readPixels (buf.data(), juce::Rectangle<int> (0, 0, w, h),
                                juce::OpenGLFrameBuffer::RowOrder::fromTopDown);

        juce::Image img (juce::Image::ARGB, w, h, false, juce::SoftwareImageType());
        {
            juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < h; ++y)
                std::memcpy (bd.getLinePointer (y),
                             buf.data() + (size_t) y * (size_t) w,
                             sizeof (juce::PixelARGB) * (size_t) w);
        }
        s->deliverOffscreenImage (img);
    }

    void setFractalUniforms (const SynShaderState& st, int offX, int offY, int resW, int resH)
    {
        if (uRes    != nullptr) uRes->set    ((float) resW, (float) resH);
        if (uOffset != nullptr) uOffset->set ((float) offX, (float) offY);
        if (uTime   != nullptr) uTime->set   (st.time * 0.4f);
        if (uPitch  != nullptr) uPitch->set  (st.pitch);
        if (uOctave != nullptr) uOctave->set (st.octave);
        if (uRMS    != nullptr) uRMS->set    (st.rms);
        if (uZoom   != nullptr) uZoom->set    (st.zoom);
        if (uRot    != nullptr) uRot->set    (st.rotation);
        if (uSym    != nullptr) uSym->set    ((float) st.symmetry);
        if (uSat    != nullptr) uSat->set    (st.saturation);
        if (uBright != nullptr) uBright->set (st.brightness);
        if (uBloom  != nullptr) uBloom->set  (st.bloom);
        if (uShake  != nullptr) uShake->set  (st.shakeX, st.shakeY);
        if (uManual != nullptr) uManual->set (st.manual ? 1.0f : 0.0f);
        if (uBaseHue != nullptr) uBaseHue->set (st.baseHue);
        if (uBaseSat != nullptr) uBaseSat->set (st.baseSat);
        if (uBaseVal != nullptr) uBaseVal->set (st.baseVal);
        if (uVariation != nullptr) uVariation->set (st.variation);
        if (uTransmute != nullptr) uTransmute->set (st.transmute);
        if (uMirror != nullptr) uMirror->set (st.mirror ? 1.0f : 0.0f);
        if (uClear  != nullptr) uClear->set  (st.clear);
        if (uDenoise != nullptr) uDenoise->set (st.denoise);
        if (uTunnel  != nullptr) uTunnel->set  (st.tunnel);
        if (uVortex  != nullptr) uVortex->set  (st.vortex);
        if (uBeatPulse != nullptr) uBeatPulse->set (st.beatPulse);
        // ANDed with offscreen on purpose: a layer always renders through the FBO,
        // so this can never make the module transparent while it is still being
        // drawn inline in the HUD.
        if (uSynNoBg != nullptr) uSynNoBg->set ((st.noBackground && st.offscreen) ? 1.0f : 0.0f);
        if (uBgIn   != nullptr) uBgIn->set   (AlterTheme::bgDeep.getFloatRed(),
                                             AlterTheme::bgDeep.getFloatGreen(),
                                             AlterTheme::bgDeep.getFloatBlue());
        if (uBgOut  != nullptr) uBgOut->set  (AlterTheme::bgVoid.getFloatRed(),
                                             AlterTheme::bgVoid.getFloatGreen(),
                                             AlterTheme::bgVoid.getFloatBlue());
    }

    // ── GL resources ─────────────────────────────────────────────────────────
    void buildQuad()
    {
        using namespace juce::gl;
        static const float quadVerts[] = { -1.0f,-1.0f,  1.0f,-1.0f,  -1.0f,1.0f,  1.0f,1.0f };
        glGenVertexArrays (1, &quadVAO);
        glGenBuffers (1, &quadVBO);
        glBindVertexArray (quadVAO);
        glBindBuffer (GL_ARRAY_BUFFER, quadVBO);
        glBufferData (GL_ARRAY_BUFFER, sizeof (quadVerts), quadVerts, GL_STATIC_DRAW);
        glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray (0);
        glBindVertexArray (0);
    }

    // Attribute-less VAO for the geometry passes. GL 3.2 core refuses to draw with
    // no vertex array object bound, but the geometry shader reads NOTHING from
    // vertex attributes — it derives everything from gl_VertexID — so this one is
    // deliberately left empty. Reusing quadVAO instead would leave attribute 0
    // enabled and pointing at a four-vertex buffer while we draw hundreds of
    // thousands of vertices, which is an out-of-bounds fetch.
    void buildEmptyVao()
    {
        juce::gl::glGenVertexArrays (1, &geoVAO);
    }

    // ── Geometry shader: gl_VertexID -> stroked, glowing outline ─────────────
    void buildGeoShader()
    {
        const char* vertexShader =
            "uniform sampler2D uInst;\n"        // instance records, 3 texels each
            "uniform int   uInstTexW;\n"        // records per texture row
            "uniform vec2  uModuleSize;\n"      // module size, logical px
            "uniform int   uSegs;\n"            // ring segments (multiple of 12)
            "uniform float uAaPad;\n"           // extra band width for the AA ramp
            "varying float vAcross;\n"          // signed distance across the stroke, logical px
            "flat varying float vHalfW;\n"
            "flat varying vec4  vCol;\n"
            "void main()\n"
            "{\n"
            "    int vpi  = uSegs * 6;\n"
            "    int inst = gl_VertexID / vpi;\n"
            "    int lv   = gl_VertexID - inst * vpi;\n"
            "    int seg  = lv / 6;\n"
            "    int cor  = lv - seg * 6;\n"
            // Two triangles per ring segment, as (segment endpoint, side) pairs:
            //   0:(0,out) 1:(0,in) 2:(1,out) | 3:(1,out) 4:(0,in) 5:(1,in)
            "    float ei = (cor == 0 || cor == 1 || cor == 4) ? 0.0 : 1.0;\n"
            "    float sd = (cor == 0 || cor == 2 || cor == 3) ? 1.0 : -1.0;\n"
            "    int c = inst - (inst / uInstTexW) * uInstTexW;\n"
            "    int r = (inst / uInstTexW) * 3;\n"
            "    vec4 d0 = texelFetch(uInst, ivec2(c, r    ), 0);\n"   // cx, cy, radius, rot
            "    vec4 d1 = texelFetch(uInst, ivec2(c, r + 1), 0);\n"   // colour
            "    vec4 d2 = texelFetch(uInst, ivec2(c, r + 2), 0);\n"   // halfW, (unused), sides, (unused)
            "    float ang = (float(seg) + ei) * 6.283185307 / float(uSegs);\n"
            // Regular n-gon with a vertex at angle 0: rf is its radius at this angle
            // (1 at a corner, cos(pi/n) at an edge midpoint). The miter factor m
            // turns "move the EDGE out by w" into "grow the circumradius by w*m" —
            // exact, not an approximation, because the apothem is R*cos(pi/n).
            // Verified to 1e-14 px against the true point-to-outline distance.
            "    float rf = 1.0;\n"
            "    float m  = 1.0;\n"
            "    if (d2.z >= 2.5)\n"
            "    {\n"
            "        float hf = 3.141592654 / d2.z;\n"
            "        float a2 = mod(ang, 2.0 * hf) - hf;\n"
            "        rf = cos(hf) / cos(a2);\n"
            "        m  = 1.0 / cos(hf);\n"
            "    }\n"
            "    float band = d2.x + uAaPad;\n"
            // A wide glow band on a small shape would push the INNER ring through the
            // centre and turn the outline inside out. Clamping the radius at zero
            // makes it fill the disc instead — and because the perpendicular distance
            // is recovered from the CLAMPED radius, the falloff stays truthful there
            // rather than pretending the centre is further out than it is.
            "    float R = max(d0.z + sd * band * m, 0.0);\n"
            "    vec2  p = (R * rf) * vec2(cos(ang), sin(ang));\n"
            "    float cr = cos(d0.w), sr = sin(d0.w);\n"
            "    p = vec2(p.x * cr - p.y * sr, p.x * sr + p.y * cr) + d0.xy;\n"
            "    vAcross   = (R - d0.z) / m;\n"
            "    vHalfW    = d2.x;\n"
            "    vCol      = d1;\n"
            "    gl_Position = vec4(p.x * 2.0 / uModuleSize.x - 1.0,\n"
            "                       1.0 - p.y * 2.0 / uModuleSize.y, 0.0, 1.0);\n"
            "}\n";

        const char* fragmentShader =
            "varying float vAcross;\n"
            "flat varying float vHalfW;\n"
            "flat varying vec4  vCol;\n"
            "void main()\n"
            "{\n"
            // fwidth gives the stroke-space size of one fragment: the unit in which
            // "one pixel wide" has to be expressed for the ramp to hold at any zoom,
            // any display scale and any shape size. This is the anti-aliasing the
            // software rasteriser used to pay for in fill cost. Everything else about
            // the look (the halo, the fusion between neighbouring shapes) comes from
            // the blur pyramid, which is why this shader has nothing else in it.
            "    float aa = max(fwidth(vAcross), 0.0008);\n"

            // SUB-PIXEL STROKES. Once halfW drops below half a fragment — a small
            // shape, a thin stroke, or simply a lot of shapes at once — the ramp
            // straddles the centre line and the smoothstep evaluates to about 0.5
            // there no matter HOW thin the stroke actually is. Coverage stops
            // tracking width, so as shapes move or shrink each stroke flickers
            // between roughly-half-lit and gone: the crawling, pixelated look on a
            // field of small shapes. The fix is the standard one for distance-field
            // line rendering — never draw thinner than half a fragment, and take the
            // width that was given up out of the ALPHA instead. Total light emitted
            // stays proportional to the true width, so the shape keeps its
            // brightness and simply fades as it gets finer, which is what the eye
            // expects and what survives a downscale.
            "    float minH = 0.5 * aa;\n"
            "    float hw   = max(vHalfW, minH);\n"
            "    float fade = min(vHalfW / minH, 1.0);\n"

            // Exact-coverage ramp, ONE fragment wide and centred on the edge. The
            // old smoothstep ran from hw - aa to hw + aa, i.e. across TWO fragments,
            // so every stroke in the picture carried a pixel more softness than the
            // anti-aliasing needed — the whole module read slightly out of focus,
            // and the export made that obvious.
            "    float a = clamp((hw - abs(vAcross)) / aa + 0.5, 0.0, 1.0) * vCol.a * fade;\n"
            "    gl_FragColor = vec4(vCol.rgb * a, a);\n"   // premultiplied, over
            "}\n";

        auto sp = std::make_unique<juce::OpenGLShaderProgram> (context);
        const bool ok = sp->addVertexShader   (juce::OpenGLHelpers::translateVertexShaderToV3   (vertexShader))
                     && sp->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (fragmentShader));

        if (ok && sp->link())
        {
            geoShader = std::move (sp);
            uGeoInst      .reset (new juce::OpenGLShaderProgram::Uniform (*geoShader, "uInst"));
            uGeoInstTexW  .reset (new juce::OpenGLShaderProgram::Uniform (*geoShader, "uInstTexW"));
            uGeoModuleSize.reset (new juce::OpenGLShaderProgram::Uniform (*geoShader, "uModuleSize"));
            uGeoSegs      .reset (new juce::OpenGLShaderProgram::Uniform (*geoShader, "uSegs"));
            uGeoAaPad     .reset (new juce::OpenGLShaderProgram::Uniform (*geoShader, "uAaPad"));
            geoReady().store (true);
        }
        else
        {
            // Leave geoReady() false: every GeometryVisual then keeps rasterising on
            // its worker thread, exactly as it did before, instead of going black.
            jassertfalse;
        }
    }

    // ── Separable gaussian, used for both halves of the bloom blur ───────────
    void buildGeoBlurShader()
    {
        const char* vertexShader =
            "attribute vec2 position;\n"
            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";

        // Classic 9-tap gaussian collapsed to 5 bilinear fetches. uStep is one
        // gaussian unit expressed in SOURCE uv, so the horizontal pass can read the
        // full-resolution scene straight into the quarter-resolution target and do
        // the downsample and the blur in one go.
        const char* fragmentShader =
            "uniform sampler2D uTex;\n"
            "uniform vec2 iResolution;\n"
            "uniform vec2 iOffset;\n"
            "uniform vec2 uStep;\n"
            "void main()\n"
            "{\n"
            "    vec2 uv = (gl_FragCoord.xy - iOffset) / iResolution;\n"
            "    vec2 o1 = uStep * 1.3846153846;\n"
            "    vec2 o2 = uStep * 3.2307692308;\n"
            "    vec4 s = texture2D(uTex, uv) * 0.2270270270;\n"
            "    s += (texture2D(uTex, uv + o1) + texture2D(uTex, uv - o1)) * 0.3162162162;\n"
            "    s += (texture2D(uTex, uv + o2) + texture2D(uTex, uv - o2)) * 0.0702702703;\n"
            "    gl_FragColor = s;\n"
            "}\n";

        auto sp = std::make_unique<juce::OpenGLShaderProgram> (context);
        const bool ok = sp->addVertexShader   (juce::OpenGLHelpers::translateVertexShaderToV3   (vertexShader))
                     && sp->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (fragmentShader));
        if (ok)
            juce::gl::glBindAttribLocation (sp->getProgramID(), 0, "position");

        if (ok && sp->link())
        {
            geoBlurShader = std::move (sp);
            uGbTex   .reset (new juce::OpenGLShaderProgram::Uniform (*geoBlurShader, "uTex"));
            uGbRes   .reset (new juce::OpenGLShaderProgram::Uniform (*geoBlurShader, "iResolution"));
            uGbOffset.reset (new juce::OpenGLShaderProgram::Uniform (*geoBlurShader, "iOffset"));
            uGbStep  .reset (new juce::OpenGLShaderProgram::Uniform (*geoBlurShader, "uStep"));
        }
    }

    // ── Final composite: theme gradient + bloom veil + crisp scene ───────────
    void buildGeoCompositeShader()
    {
        const char* vertexShader =
            "attribute vec2 position;\n"
            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";

        const char* fragmentShader =
            "uniform sampler2D uScene;\n"
            "uniform sampler2D uVeil;\n"
            "uniform vec2  iResolution;\n"
            "uniform vec2  iOffset;\n"
            "uniform float uBloomGain;\n"
            "uniform float uVeilOpacity;\n"
            "uniform vec2  uVeilTexel;\n"      // one veil texel, in uv
            "uniform vec3  uBgIn;\n"
            "uniform vec3  uBgMid;\n"
            "uniform vec3  uBgOut;\n"
            "uniform float uNoBg;\n"           // 1 while lent to a Fusion as a layer
            // 3x3 tent reconstruction of the veil.
            //
            // The veil is an eighth of the tile, so reading it with a single
            // bilinear tap stretches it 8x — and bilinear magnification is only
            // C0-continuous, which on a smooth field shows up as the diamond-shaped
            // facets that make two overlapping haloes look like they are being
            // INTERPOLATED between rather than adding up. Nine taps (weights
            // 1-2-1 / 2-4-2 / 1-2-1) reconstruct it smoothly and the facets go.
            "vec4 veilTent(vec2 uv)\n"
            "{\n"
            "    vec2 t = uVeilTexel;\n"
            "    vec4 s = texture2D(uVeil, uv) * 4.0;\n"
            "    s += (texture2D(uVeil, uv + vec2(-t.x, 0.0)) + texture2D(uVeil, uv + vec2(t.x, 0.0))\n"
            "        + texture2D(uVeil, uv + vec2(0.0, -t.y)) + texture2D(uVeil, uv + vec2(0.0, t.y))) * 2.0;\n"
            "    s += texture2D(uVeil, uv + vec2(-t.x, -t.y)) + texture2D(uVeil, uv + vec2(t.x, -t.y))\n"
            "       + texture2D(uVeil, uv + vec2(-t.x,  t.y)) + texture2D(uVeil, uv + vec2(t.x,  t.y));\n"
            "    return s * 0.0625;\n"
            "}\n"
            "void main()\n"
            "{\n"
            "    vec2 uv = (gl_FragCoord.xy - iOffset) / iResolution;\n"
            // The theme's vertical gradient, exactly as AlterTheme::paintBackground
            // draws it: bgDeep at the top, the midpoint at 0.55, bgVoid at the
            // bottom. GL's y runs upward, hence 1 - uv.y.
            "    float bt = clamp(1.0 - uv.y, 0.0, 1.0);\n"
            "    vec3 bg = (bt < 0.55) ? mix(uBgIn, uBgMid, bt / 0.55)\n"
            "                          : mix(uBgMid, uBgOut, (bt - 0.55) / 0.45);\n"
            "    vec4 sc = texture2D(uScene, uv);\n"
            "    vec4 v  = veilTent(uv);\n"
            // GAIN — applied to the ALPHA as well as the colour. This is not a
            // detail: gainARGB in the CPU path multiplied every byte of a
            // premultiplied pixel, so where strokes converge the veil went not just
            // brighter but OPAQUE, and that is what fused them into one solid core.
            // Gaining only the colour leaves the veil translucent everywhere and
            // the glow reads thin and washed out.
            "    v *= uBloomGain;\n"
            // Roll off the top instead of clipping it, and roll off the CHANNELS
            // TOGETHER instead of one at a time.
            //
            // Clipping each channel at 1 does two visible things wrong. It shifts
            // the hue — a colour like (1.0, 0.35, 0.6) has its red pinned while
            // green and blue keep climbing, so as Bloom rises the glow slides
            // toward white unevenly and the colour looks like it is being pushed.
            // And it flattens: once a region clips, more light adds nothing, so two
            // overlapping haloes stop growing into each other and sit as one dead
            // plateau. Scaling all three by the same factor keeps the hue fixed,
            // and the exponential shoulder never quite reaches 1, so overlapping
            // haloes keep getting brighter however many of them pile up.
            "    float m = max(v.r, max(v.g, v.b));\n"
            "    if (m > 0.8)\n"
            "    {\n"
            "        float ms = 0.8 + 0.2 * (1.0 - exp(-(m - 0.8) / 0.2));\n"
            "        v.rgb *= ms / m;\n"
            // Scaling all three channels together holds the hue, but on its own it
            // also means the colour stops changing once it is over — so a pile of
            // haloes would settle on one flat saturated plateau instead of growing
            // into a core. Real light does not do that: past a point it reads white.
            // So beyond a good margin of overload, bleed toward white. Below 1.5x
            // this term is exactly zero, which is the whole range where a hue shift
            // would be noticed as the colour "moving".
            "        float wht = 1.0 - exp(-max(m - 1.5, 0.0) * 0.35);\n"
            "        v.rgb = mix(v.rgb, vec3(ms), wht);\n"
            "    }\n"
            "    v.a = min(v.a, 1.0);\n"
            // Then the veil's own opacity. On a premultiplied image this scales
            // colour and coverage together, exactly like juce::Graphics::setOpacity.
            "    v *= uVeilOpacity;\n"
            // Compositing order is the CPU path's: background, veil OVER it, crisp
            // strokes over both. The veil sits UNDER the cores so the halo radiates
            // around a hot line instead of milking it over.
            // Lent to a Fusion: the same compositing order, but onto NOTHING
            // instead of onto the background — the veil and the strokes carry their
            // own coverage, so unioning it as they stack gives real alpha.
            "    if (uNoBg > 0.5)\n"
            "    {\n"
            "        float va = clamp(v.a,  0.0, 1.0);\n"
            "        float sa = clamp(sc.a, 0.0, 1.0);\n"
            "        vec3  pc = v.rgb * (1.0 - sa) + sc.rgb;\n"
            "        float pa = va * (1.0 - sa) + sa;\n"
            "        gl_FragColor = vec4(clamp(pc, 0.0, 1.0), pa);\n"
            "        return;\n"
            "    }\n"
            "    vec3 col = bg;\n"
            "    col = col * (1.0 - clamp(v.a,  0.0, 1.0)) + v.rgb;\n"
            "    col = col * (1.0 - clamp(sc.a, 0.0, 1.0)) + sc.rgb;\n"
            "    gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);\n"
            "}\n";

        auto sp = std::make_unique<juce::OpenGLShaderProgram> (context);
        const bool ok = sp->addVertexShader   (juce::OpenGLHelpers::translateVertexShaderToV3   (vertexShader))
                     && sp->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (fragmentShader));
        if (ok)
            juce::gl::glBindAttribLocation (sp->getProgramID(), 0, "position");

        if (ok && sp->link())
        {
            geoCompShader = std::move (sp);
            uGcScene       .reset (new juce::OpenGLShaderProgram::Uniform (*geoCompShader, "uScene"));
            uGcVeil        .reset (new juce::OpenGLShaderProgram::Uniform (*geoCompShader, "uVeil"));
            uGcRes         .reset (new juce::OpenGLShaderProgram::Uniform (*geoCompShader, "iResolution"));
            uGcOffset      .reset (new juce::OpenGLShaderProgram::Uniform (*geoCompShader, "iOffset"));
            uGcBloomGain   .reset (new juce::OpenGLShaderProgram::Uniform (*geoCompShader, "uBloomGain"));
            uGcVeilOpacity .reset (new juce::OpenGLShaderProgram::Uniform (*geoCompShader, "uVeilOpacity"));
            uGcVeilTexel   .reset (new juce::OpenGLShaderProgram::Uniform (*geoCompShader, "uVeilTexel"));
            uGcNoBg        .reset (new juce::OpenGLShaderProgram::Uniform (*geoCompShader, "uNoBg"));
            uGcBgIn     .reset (new juce::OpenGLShaderProgram::Uniform (*geoCompShader, "uBgIn"));
            uGcBgMid    .reset (new juce::OpenGLShaderProgram::Uniform (*geoCompShader, "uBgMid"));
            uGcBgOut    .reset (new juce::OpenGLShaderProgram::Uniform (*geoCompShader, "uBgOut"));
        }
    }

    void buildShader()
    {
        const char* vertexShader =
            "attribute vec2 position;\n"
            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";

        const char* fragmentShader =
            "uniform vec2 iResolution;\n"
            "uniform vec2 iOffset;\n"
            "uniform float iTime;\n"
            "uniform float iPitch;\n"
            "uniform float iOctave;\n"
            "uniform float iRMS;\n"
            "uniform float iZoom;\n"
            "uniform float iRotation;\n"
            "uniform float iSymmetry;\n"
            "uniform float iSaturation;\n"
            "uniform float iBrightness;\n"   // 0..2, 1 = neutral overall light output
            "uniform float iBloom;\n"
            "uniform vec3 iBgInner;\n"
            "uniform vec3 iBgOuter;\n"
            "uniform float uNoBg;\n"       // 1 while lent to a Fusion as a layer
            "uniform vec2 iShake;\n"
            "uniform float iManual;\n"     // 1 = use iBaseHue, 0 = pitch-derived hue
            "uniform float iBaseHue;\n"    // manual base hue 0..1
            "uniform float iBaseSat;\n"    // manual base saturation 0..1 (1 in tone mode)
            "uniform float iBaseVal;\n"    // manual base brightness 0..1 (1 in tone mode)
            "uniform float iVariation;\n"  // 0..1 'Change': morphs the fractal smoothly
            "uniform float iTransmute;\n"  // 0..1 'Transmute': a different, SYMMETRIC morph
            "uniform float iMirror;\n"     // 1 = mirror fold (reflective symmetry)
            "uniform float iClear;\n"      // 0..1 'Clear': down-shifts the octave/layer complexity
            "uniform float iDenoise;\n"    // 0..1 'Denoise': joins dashed curves + removes high noise layers
            "uniform float iTunnel;\n"     // 0..1 'Tunnel': symmetric fly-through tunnel morph
            "uniform float iVortex;\n"     // 0..1 'Vortex': swirl/spiral twist of the tunnel walls
            "uniform float iBeatPulse;\n"  // 0..1 BPM mode: pulse of light on each beat
            // hue -> rgb. THE CHANNEL OFFSETS MUST BE (0, 2/3, 1/3), NOT (0, 1/3, 2/3).
            //
            // Written the wrong way round this function is still a perfectly smooth,
            // perfectly saturated colour wheel — it just runs BACKWARDS. Swapping the
            // green and blue offsets mirrors the wheel about red, i.e. h -> 1-h, so
            // ten of the twelve semitones came out as their opposite: E drew blue
            // instead of green, A# drew yellow instead of magenta, B drew orange
            // instead of rose. Only red (0.0) and cyan (0.5) — the two fixed points of
            // the mirror — happened to agree with Chladni and Geometry, which is why
            // the base hue "matching" was never enough to make the modules agree.
            //
            // This also governs the MANUAL colour: iBaseHue is the hue of the colour
            // the user picked, so picking green used to draw blue.
            //
            // Equivalent to the canonical form mod(c.x*6 + vec3(0,4,2), 6) - 3.
            "vec3 hsv2rgb(vec3 c)\n"
            "{\n"
            "    vec3 p = abs(fract(c.xxx + vec3(0.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0);\n"
            "    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);\n"
            "}\n"
            "void main()\n"
            "{\n"
            "    vec2 fc = gl_FragCoord.xy - iOffset;\n"            // viewport-local pixel
            "    vec2 uv = (fc * 2.0 - iResolution.xy) / iResolution.xy;\n"
            "    vec2 bgUv = uv;\n"
            "    uv += iShake;\n"                                   // React: audio-driven jitter

            // TWO COORDINATE PATHS:
            //   uv  = the FRACTAL path. The tunnel is its OUTERMOST warp, so the
            //         completely composed classic shader (rotation, zoom, symmetry,
            //         mirror, Fragment, Transmute) is evaluated THROUGH the tunnel
            //         map — i.e. the tunnel is applied at the very END of the chain:
            //         the finished classic image is what lines the tunnel walls.
            //   uvB = the SCREEN path (no tunnel), folded identically. uv0 comes
            //         from it, so every brightness term (radial exp falloff, rings,
            //         hue spread, beat wave) keeps the classic screen distribution
            //         and the light intensity follows RMS exactly as originally.
            "    vec2 uvB = uv;\n"
            "    float tunShade = 1.0;\n"
            "    float asp = iResolution.x / max(iResolution.y, 1.0);\n"           // pixel aspect → used to keep swirl/tunnel circular, not elliptical
            // VORTEX: a true swirl around the centre that works with OR without the
            // tunnel. We rotate every point by an angle that grows toward the centre
            // (∝ 1/radius), so the inner region winds faster than the rim — a spiral
            // pulling from the middle out to the edges. Done in ASPECT-CORRECTED space
            // (x scaled by aspect) so the whirl stays perfectly round when the module
            // is stretched wide, then mapped back.
            "    if (abs(iVortex) > 0.001)\n"
            "    {\n"
            "        vec2 q = vec2(uv.x * asp, uv.y);\n"
            "        float vr = length(q);\n"
            "        float va = atan(q.y, q.x) + iVortex * 7.8 * (1.0 / (vr + 0.22) - 1.15);\n"   // 3x stronger; the -1.15 flips the swirl sign past mid-radius, so the inner core and the outer rim wind in OPPOSITE directions (a two-way counter-rotating vortex)
            "        q = vec2(cos(va), sin(va)) * vr;\n"
            "        uv = vec2(q.x / asp, q.y);\n"
            "        vec2 qb = vec2(uvB.x * asp, uvB.y);\n"
            "        float vrb = length(qb);\n"
            "        float vab = atan(qb.y, qb.x) + iVortex * 7.8 * (1.0 / (vrb + 0.22) - 1.15);\n"
            "        qb = vec2(cos(vab), sin(vab)) * vrb;\n"
            "        uvB = vec2(qb.x / asp, qb.y);\n"
            "    }\n"
            "    float angle = iRotation * 3.14159 / 180.0;\n"
            "    float s = sin(angle); float c = cos(angle);\n"
            "    uv  = vec2(uv.x * c - uv.y * s,   uv.x * s + uv.y * c);\n"
            "    uvB = vec2(uvB.x * c - uvB.y * s, uvB.x * s + uvB.y * c);\n"
            "    uv  /= iZoom;\n"
            "    uvB /= iZoom;\n"
            "    if (iSymmetry > 1.0)\n"
            "    {\n"
            "        float seg = 6.28318 / iSymmetry;\n"
            "        float a = atan(uv.y, uv.x);\n"
            "        float r = length(uv);\n"
            "        a = mod(a, seg) - seg * 0.5;\n"
            "        uv = vec2(cos(a), sin(a)) * r;\n"
            "        float aB = atan(uvB.y, uvB.x);\n"
            "        float rB = length(uvB);\n"
            "        aB = mod(aB, seg) - seg * 0.5;\n"
            "        uvB = vec2(cos(aB), sin(aB)) * rB;\n"
            "    }\n"
            "    if (iMirror > 0.5)\n"                                    // Mirror fold: reflect into one quadrant
            "    {\n"
            "        uv  = abs(uv);\n"                                    // → clean left/right + top/bottom mirror symmetry
            "        uvB = abs(uvB);\n"
            "    }\n"
            // TUNNEL (applied LAST, after rotation/zoom/symmetry/mirror, so the fully
            // composed symmetric image is what lines the walls). This is a proper 3D
            // fly-through: each screen pixel is re-expressed as a point on a round tube —
            //   • around = angle about the tube axis (mirror-folded ⇒ no seam, symmetric)
            //   • depth  = 1/radius, so the CENTRE is infinitely far (the vanishing point)
            //     and the rim is the near wall; +iTime flies us forward down the tube.
            // Aspect-corrected first (x·asp) so the mouth of the tunnel is a true circle,
            // not an ellipse, no matter how the module is resized. tunShade darkens the
            // centre to black — the far end — completing the depth read.
            "    if (iTunnel > 0.001)\n"
            "    {\n"
            "        vec2 q = vec2(uv.x * asp, uv.y);\n"
            "        float r = max(length(q), 0.0015);\n"
            "        float a = atan(q.y, q.x);\n"
            "        float depth  = 0.33 / r + iTime * 0.5;\n"            // perspective depth: centre far, rim near, flying forward
            "        float around = abs(a) * 0.31831;\n"                  // |angle|/π ⇒ symmetric top↔bottom, seamless
            "        uv  = mix(uv, vec2(around, depth), iTunnel);\n"
            "        tunShade = mix(1.0, smoothstep(0.0, 0.62, r), iTunnel);\n"  // dark vanishing centre → bright near walls
            "    }\n"
            "    vec2 uv0 = uvB;\n"
            "    vec3 finalColor = vec3(0.0);\n"
            "    float baseHue = (iManual > 0.5) ? iBaseHue : (iPitch / 12.0);\n"
            // FRAGMENT ('Change'/iVariation) IS A GEOMETRY MORPH, NOT A PALETTE KNOB.
            // It used to do `baseHue = fract(baseHue + iVariation * 0.5)` — half the
            // colour wheel, i.e. SIX SEMITONES of hue. With Fragment up, a C read as
            // an F#, so the same note was a different colour here than in Chladni or
            // Geometry. Same for the radial `hspread = iVariation * 0.15`. Both hue
            // terms are gone; every structural term below (fold, wfreq, warp, vrot)
            // is untouched, so Fragment morphs exactly as before — just in one colour.
            "    float hsc = 1.0;\n"                                       // layer spread now expressed directly in wheel units (see kToneHueSpread)
            "    float fold = 1.2 + iVariation * 0.5;\n"                   // 'Change' morphs the structure
            "    float wfreq = (8.0 + iVariation * 6.0);\n"                // 'Change' ripple frequency (Denoise no longer alters it — geometry stays put)
            "    float hspread = 0.0;\n"                                   // no radial hue drift: Fragment must not move the note's colour
            "    float vrot = iVariation * 2.3;\n"                         // per-iteration twist → spirals
            "    float vca = cos(vrot); float vsa = sin(vrot);\n"
            "    float warp = iVariation * 0.55;\n"                        // domain warp → organic structure
            // BIPOLAR bloom: bbp = boost side (stronger than the classic max used to
            // be), bbn = dim side (thinner + darker than the neutral look).
            "    float bbp = clamp(iBloom, 0.0, 1.0);\n"
            "    float bbn = clamp(-iBloom, 0.0, 1.0);\n"
            "    float tfall = (1.0 - iTransmute * 0.55) * (1.0 - iTunnel * 0.85);\n"   // 'Transmute' radial falloff; drop the screen vignette in the tunnel so near walls stay bright
            "    float tpow  = 1.2 + iTransmute * 0.6;\n"                  // 'Transmute' glow (kept soft → bold lines, not fine noise)
            "    float trings = (2.5 + iTransmute * 6.0) * iZoom;\n"      // 'Transmute' rings (Denoise no longer alters it — geometry stays put)
            "    float tradial = iTransmute * 0.8;\n"                     // 'Transmute' radial lensing-wave amount
            "    float tfreq = (1.6 + iTransmute * 3.4) * iZoom;\n"      // 'Transmute' radial wave (Denoise no longer alters it)
            "    float tevo  = iTime * (1.0 + iTransmute * 0.5);\n"      // 'Transmute' evolution — only a small speed-up as it rises (kept gentle)
            "    int maxIter = int(clamp(ceil(iOctave - iClear * 5.0), 1.0, 7.0));\n"   // 'Clear' down-shifts the audio-reactive octave complexity → fewer layers (keeps reactivity)
            "    for (int i = 0; i < 7; i++)\n"
            "    {\n"
            "        if (i >= maxIter) break;\n"
            "        uv = vec2(uv.x * vca - uv.y * vsa, uv.x * vsa + uv.y * vca);\n"   // 'Change' twist
            "        uv += warp * sin(uv.yx * 3.0 + iTime * 0.4);\n"                    // 'Change' domain warp
            "        uv = fract(uv * fold) - 0.5;\n"
            "        float rl = length(uv);\n"
            "        uv *= 1.0 + tradial * sin(rl * tfreq - tevo * 1.6);\n"            // 'Transmute' radial lensing wave (symmetric, evolving)
            "        float d = length(uv) * exp(-length(uv0) * tfall);\n"
            // LAYER HUE SPREAD — capped at PitchUtils::kToneHueSpread (0.02 of the
            // wheel ≈ a quarter semitone). It was `float(i) * 0.05 * 0.5` = up to
            // 0.15, i.e. nearly two semitones of drift across the seven layers, which
            // is why the fractal's dominant colour never quite matched the plate or
            // the shapes. Normalising by 6.0 keeps the gradient shape identical while
            // pinning its total excursion to the shared budget: layer 0 is the exact
            // note colour and the deepest layer is a barely-perceptible step off it.
            "        float hue = fract(baseHue + (float(i) / 6.0) * 0.02 * hsc\n"
            "                                  + length(uv0) * hspread);\n"
            // Core saturation is the SHARED reference (PitchUtils::kToneSaturation = 1.0)
            // whenever the colour comes from the tone. In manual mode the picked
            // colour's own S and V ride along instead of being discarded — hue alone
            // cannot express white, black or any grey, which is why an achromatic pick
            // used to come out as full-saturation red.
            "        vec3 col = hsv2rgb(vec3(hue, iBaseSat, iBaseVal));\n"

            "        float gray = (col.r + col.g + col.b) / 3.0;\n"
            "        col = mix(vec3(gray), col, iSaturation);\n"
            "        float phase = d * wfreq + iTime;\n"
            "        float sv = abs(sin(phase));\n"
            "        float aaw = fwidth(phase);\n"                                   // stripe-cycles packed into ONE pixel — explodes in the far distance / tunnel centre where lines crowd together
            "        sv = mix(sv, 0.6366, clamp(aaw * 0.5, 0.0, 1.0) * iDenoise);\n" // DENOISE: where the stripes are finer than a pixel they can only ALIAS into broken dashes/speckle; dissolve them into their exact analytic mean (2/pi) so the thin far lines stay smooth & continuous instead of shattering
            "        d = sv / 8.0;\n"
            // BLOOM does the glow: widen the line core + soften the falloff → thick, bright,
            // glowing strokes. Positive side is BOOSTED beyond the old maximum; the
            // negative side sharpens + thins the lines below the neutral look.
            "        float lnum = mix(mix(0.01, 0.004, bbn), 0.06, bbp);\n"        // BLOOM widens the line (moderate) → glow spread
            "        float lpow = mix(mix(tpow, tpow * 1.4, bbn), tpow * 0.4, bbp);\n"
            "        lpow *= mix(1.0, 0.7, iDenoise);\n"                           // DENOISE softens the falloff → BRIDGES gaps so broken curves connect (not a glow)
            "        d = pow(lnum / max(d, 0.001), lpow) / float(i + 1);\n"
            "        d *= mix(1.0, 0.5, bbp);\n"                                   // BLOOM stays energy-neutral: dim the peak as the line widens → glow without getting brighter
            "        float ring = 0.55 + 0.45 * cos(length(uv0) * trings - tevo * 0.7);\n"   // 'Transmute' rings (symmetric, evolving)
            // DENOISE (does NOT widen / glow — that's Bloom):
            //   1) JOIN — fully removes the radial dashing on EVERY layer (incl. the
            //      primary i==0) so the broken curves read as continuous connected lines.
            //   2) CLEAN — at strong settings, progressively removes the highest thin
            //      (noise) layers entirely.
            "        ring = mix(ring, 1.0, iDenoise);\n"
            "        d *= ring;\n"
            "        float cull = 1.0 - clamp((iDenoise - 0.55) / 0.45, 0.0, 1.0)\n"
            "                         * clamp((float(i) - 2.0) / 3.0, 0.0, 1.0);\n"
            "        d *= cull;\n"
            "        finalColor += col * d;\n"
            "    }\n"
            "    float intensity = iRMS * iRMS * 2.0;\n"
            // BRIGHTNESS: overall light output (0 = black … 1 = neutral … 2 = doubled).
            // Squared response so the knob feels perceptually even.
            "    intensity *= iBrightness * iBrightness;\n"
            // BPM mode: on each beat a bright wave sweeps from the centre out across the
            // WHOLE shader. iBeatPulse runs 1 (beat) → 0 (next beat); the wavefront radius
            // grows with (1 - iBeatPulse), so the band travels edge-ward through everything.
            "    float wf = (1.0 - iBeatPulse) * 1.8;\n"
            "    float beatWave = exp(-pow((length(uv0) - wf) * 2.2, 2.0));\n"
            "    intensity *= 1.0 + beatWave * 1.2;\n"
            "    finalColor *= intensity;\n"
            "    finalColor *= tunShade;\n"                                        // TUNNEL depth: fade to black at the far vanishing point (centre)
            "    finalColor *= 1.0 - bbn * 0.45;\n"                                // negative bloom: dim below neutral
            // TRANSMUTE'S HUE SHIFT WAS AN OVERDRIVE ARTEFACT, NOT A COLOUR TERM.
            // Transmute never touched `hue` — it fattens and stacks the lines, which
            // pushes finalColor well past 1.0. The framebuffer then clipped each
            // channel INDEPENDENTLY, so on a note like (1.0, 0.35, 0.6) the red pinned
            // while green and blue kept climbing: the colour visibly crawled toward
            // white along a different path than in Chladni/Geometry. Same fix the geo
            // compositor already uses — roll the channels off TOGETHER (hue held
            // exactly), and only bleed to white far past the point where a hue shift
            // would read as the colour "moving".
            "    float pk = max(finalColor.r, max(finalColor.g, finalColor.b));\n"
            "    if (pk > 0.8)\n"
            "    {\n"
            "        float ps = 0.8 + 0.2 * (1.0 - exp(-(pk - 0.8) / 0.2));\n"
            "        finalColor *= ps / pk;\n"
            "        float wht = 1.0 - exp(-max(pk - 1.5, 0.0) * 0.35);\n"
            "        finalColor = mix(finalColor, vec3(ps), wht);\n"
            "    }\n"
            "    float bgT = clamp(length(bgUv) * 0.65 + 0.12 * sin(iTime * 0.6 + length(bgUv) * 3.0), 0.0, 1.0);\n"
            "    vec3 bg = mix(iBgInner, iBgOuter, bgT);\n"
            // Coverage now tracks the ROLLED-OFF peak, so a line that used to clip to
            // a flat opaque plateau keeps gaining coverage as it gets hotter.
            "    float cov = clamp(pk, 0.0, 1.0);\n"
            // Lent to a Fusion: hand out the fractal ALONE, with coverage in the
            // alpha channel, so the layers below it show through where it is not
            // drawing. mix(bg, finalColor, cov) is exactly finalColor*cov once the
            // background is taken away, and that is already premultiplied.
            "    if (uNoBg > 0.5)\n"
            "    {\n"
            "        gl_FragColor = vec4(finalColor * cov, cov);\n"
            "        return;\n"
            "    }\n"
            "    gl_FragColor = vec4(mix(bg, finalColor, cov), 1.0);\n"
            "}\n";

        auto sp = std::make_unique<juce::OpenGLShaderProgram> (context);
        const bool ok = sp->addVertexShader   (juce::OpenGLHelpers::translateVertexShaderToV3   (vertexShader))
                     && sp->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (fragmentShader));
        if (ok)
            juce::gl::glBindAttribLocation (sp->getProgramID(), 0, "position");

        if (ok && sp->link())
        {
            shader = std::move (sp);
            uRes   .reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iResolution"));
            uOffset.reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iOffset"));
            uTime  .reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iTime"));
            uPitch .reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iPitch"));
            uOctave.reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iOctave"));
            uRMS   .reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iRMS"));
            uZoom  .reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iZoom"));
            uRot   .reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iRotation"));
            uSym   .reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iSymmetry"));
            uSat   .reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iSaturation"));
            uBright.reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iBrightness"));
            uBloom .reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iBloom"));
            uBgIn  .reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iBgInner"));
            uBgOut .reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iBgOuter"));
            uShake .reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iShake"));
            uManual.reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iManual"));
            uBaseHue.reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iBaseHue"));
            uBaseSat.reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iBaseSat"));
            uBaseVal.reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iBaseVal"));
            uVariation.reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iVariation"));
            uTransmute.reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iTransmute"));
            uMirror.reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iMirror"));
            uClear .reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iClear"));
            uDenoise.reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iDenoise"));
            uTunnel.reset  (new juce::OpenGLShaderProgram::Uniform (*shader, "iTunnel"));
            uVortex.reset  (new juce::OpenGLShaderProgram::Uniform (*shader, "iVortex"));
            uBeatPulse.reset (new juce::OpenGLShaderProgram::Uniform (*shader, "iBeatPulse"));
            uSynNoBg  .reset (new juce::OpenGLShaderProgram::Uniform (*shader, "uNoBg"));
        }
    }

    // ── Spectrogram shader ───────────────────────────────────────────────────
    // Produces the visible picture directly from the history texture. Everything
    // it needs is a handful of scalars, so the per-frame CPU cost of a
    // spectrogram is now its ANALYSIS only — no rasterisation, no per-frame
    // image upload, and no render pixel budget (hence no upscale blur).
    void buildSgShader()
    {
        const char* vertexShader =
            "attribute vec2 position;\n"
            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";

        const char* fragmentShader =
            "uniform vec2 iResolution;\n"       // viewport size, framebuffer px
            "uniform vec2 iOffset;\n"           // viewport origin, framebuffer px
            "uniform sampler2D uHistory;\n"     // x = frequency row, y = time column
            "uniform sampler2D uLut;\n"         // 256x1 colour map
            "uniform vec2  uLocal0;\n"          // module-local uv at the rect's top-left
            "uniform vec2  uLocalDU;\n"         // d(local uv) across the rect
            "uniform vec2  uLocalDV;\n"         // d(local uv) down the rect
            "uniform float uHead;\n"            // newest column, fractional, wrapped
            "uniform float uVisCols;\n"         // columns across the visible width
            "uniform float uBufCols;\n"         // history capacity (texture height)
            "uniform float uFilled;\n"          // valid columns behind the head
            "uniform float uRowSpan;\n"         // one screen pixel in local-y units
            "uniform float uYTaps;\n"           // vertical box-filter taps, integral 1..8
            "uniform vec3  uBgIn;\n"
            "uniform vec3  uBgMid;\n"
            "uniform vec3  uBgOut;\n"
            "uniform float uNoBg;\n"            // 1 while lent to a Fusion as a layer
            "void main()\n"
            "{\n"
            "    vec2 fc = gl_FragCoord.xy - iOffset;\n"
            "    float u = fc.x / max(iResolution.x, 1.0);\n"
            "    float v = 1.0 - fc.y / max(iResolution.y, 1.0);\n"   // 0 = top
            // The theme's vertical background gradient, reproduced exactly as
            // AlterTheme::paintBackground draws it, so the part of the window that
            // has no history yet matches every other module.
            "    float bt = clamp(v, 0.0, 1.0);\n"
            "    vec3 bg = (bt < 0.55) ? mix(uBgIn, uBgMid, bt / 0.55)\n"
            "                          : mix(uBgMid, uBgOut, (bt - 0.55) / 0.45);\n"
            // Module-local coordinates. Under a rotated panel this basis is not
            // the identity, which is what keeps the picture aligned with the 2D
            // grid overlay the component paints on top.
            "    vec2 lc = uLocal0 + u * uLocalDU + v * uLocalDV;\n"
            "    float back = (1.0 - lc.x) * uVisCols;\n"
            // The oldest DRAWABLE column is one in from the oldest written one.
            // The time interpolation below reads c0 AND c0+1, so letting `back`
            // reach uFilled makes the left-most pixel blend the column BEFORE the
            // oldest — which, once the ring buffer is full, wraps around to the
            // NEWEST column. That is what frayed the left edge: it was not an
            // artefact but a genuine sliver of live audio from the other end of
            // the history, which is why it moved with the spectrum.
            "    if (lc.x < 0.0 || lc.x > 1.0 || lc.y < 0.0 || lc.y > 1.0 || back > uFilled - 1.0)\n"
            "    {\n"
            "        gl_FragColor = (uNoBg > 0.5) ? vec4(0.0) : vec4(bg, 1.0);\n"
            "        return;\n"
            "    }\n"
            // TIME: a fractional column index, so scrolling is sub-pixel by
            // construction. The two neighbouring columns are fetched explicitly
            // rather than relying on texture filtering, because the history is a
            // ring buffer and hardware filtering would smear across its seam.
            "    float cf  = uHead - back;\n"
            "    float c0  = floor(cf);\n"
            "    float fx  = cf - c0;\n"
            "    float ty0 = (mod(c0,       uBufCols) + 0.5) / uBufCols;\n"
            "    float ty1 = (mod(c0 + 1.0, uBufCols) + 0.5) / uBufCols;\n"
            // FREQUENCY: the history has far more rows than the module has pixels,
            // so average the rows that land inside one screen pixel. Without this a
            // thin partial flickers on and off as it crosses a pixel boundary — the
            // CPU path's bilinear downscale had exactly that failure mode.
            "    float taps = max(uYTaps, 1.0);\n"
            "    float acc = 0.0;\n"
            "    for (int i = 0; i < 8; i++)\n"
            "    {\n"
            "        if (float(i) >= taps) break;\n"
            "        float o  = ((float(i) + 0.5) / taps - 0.5) * uRowSpan;\n"
            "        float rx = clamp(lc.y + o, 0.0, 1.0);\n"
            "        float a  = texture2D(uHistory, vec2(rx, ty0)).r;\n"
            "        float b  = texture2D(uHistory, vec2(rx, ty1)).r;\n"
            "        acc += mix(a, b, fx);\n"
            "    }\n"
            "    float e = acc / taps;\n"
            // +0.5/256 lands on the texel centre, so the lookup reproduces the
            // CPU path's 256-entry table exactly (and interpolates between them).
            "    vec3 c = texture2D(uLut, vec2((e * 255.0 + 0.5) / 256.0, 0.5)).rgb;\n"
            // Lent to a Fusion: coverage is the ENERGY, which is the one thing a
            // spectrogram actually knows. Subtracting the background out of the LUT
            // colour would be guessing at it a second time, and the palette's dark
            // end is not the background anyway. The gate only removes true silence,
            // so quiet partials still reach the fusion.
            "    if (uNoBg > 0.5)\n"
            "    {\n"
            "        float a = smoothstep(0.0, 0.04, e);\n"
            "        gl_FragColor = vec4(c * a, a);\n"
            "        return;\n"
            "    }\n"
            "    gl_FragColor = vec4(c, 1.0);\n"
            "}\n";

        auto sp = std::make_unique<juce::OpenGLShaderProgram> (context);
        const bool ok = sp->addVertexShader   (juce::OpenGLHelpers::translateVertexShaderToV3   (vertexShader))
                     && sp->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (fragmentShader));
        if (ok)
            juce::gl::glBindAttribLocation (sp->getProgramID(), 0, "position");

        if (ok && sp->link())
        {
            sgShader = std::move (sp);
            uSgRes     .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "iResolution"));
            uSgOffset  .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "iOffset"));
            uSgHistory .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uHistory"));
            uSgLut     .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uLut"));
            uSgLocal0  .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uLocal0"));
            uSgLocalDU .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uLocalDU"));
            uSgLocalDV .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uLocalDV"));
            uSgHead    .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uHead"));
            uSgVisCols .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uVisCols"));
            uSgBufCols .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uBufCols"));
            uSgFilled  .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uFilled"));
            uSgRowSpan .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uRowSpan"));
            uSgYTaps   .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uYTaps"));
            uSgBgIn    .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uBgIn"));
            uSgBgMid   .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uBgMid"));
            uSgBgOut   .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uBgOut"));
            uSgNoBg    .reset (new juce::OpenGLShaderProgram::Uniform (*sgShader, "uNoBg"));
        }
    }

    // static registry: attached-component -> host
    static juce::CriticalSection& mapLock() { static juce::CriticalSection l; return l; }
    static std::map<juce::Component*, AlterGLHost*>& hostMap()
    {
        static std::map<juce::Component*, AlterGLHost*> m;
        return m;
    }
    static AlterGLHost*& primaryRef() { static AlterGLHost* p = nullptr; return p; }

    // Set once the instanced geometry shader links; cleared when the context goes
    // away. Read from the worker threads of every GeometryVisual, hence atomic.
    static std::atomic<bool>& geoReady() { static std::atomic<bool> b { false }; return b; }

    juce::OpenGLContext context;
    juce::Component* comp = nullptr;
    bool attached = false;

    std::unique_ptr<juce::OpenGLShaderProgram> shader;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform>
        uRes, uOffset, uTime, uPitch, uOctave, uRMS, uZoom, uRot, uSym, uSat, uBright, uBloom, uBgIn, uBgOut, uShake,
        uManual, uBaseHue, uBaseSat, uBaseVal, uVariation, uTransmute, uMirror, uClear, uDenoise, uTunnel, uVortex, uBeatPulse,
        uSynNoBg;
    std::unique_ptr<juce::OpenGLShaderProgram> sgShader;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform>
        uSgRes, uSgOffset, uSgHistory, uSgLut, uSgLocal0, uSgLocalDU, uSgLocalDV,
        uSgHead, uSgVisCols, uSgBufCols, uSgFilled, uSgRowSpan, uSgYTaps,
        uSgBgIn, uSgBgMid, uSgBgOut, uSgNoBg;

    std::unique_ptr<juce::OpenGLShaderProgram> fusionShader;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform>
        uFusionRes, uFusionOffset, uFusionL0, uFusionL1, uFusionL2, uFusionCount,
        uFusionLocal0, uFusionLocalDU, uFusionLocalDV,
        uFusionBgIn, uFusionBgMid, uFusionBgOut, uFusionAspect, uFusionNoBg,
        uFusionWarp, uFusionWarpAmt, uFusionWarpSwirl, uFusionWarpSmooth, uFusionWarpDenoise, uFusionWarpSrc,
        uFusionSymmetry, uFusionMirror, uFusionMirrorAngle, uFusionSpin, uFusionZoom,
        uFusionVortex,
        uFusionDrive,
        uFusionGlobLayers, uFusionLiquid, uFusionLiquidAmt, uFusionLiquidSmooth, uFusionLiquidDenoise,
        uFusionTunnel, uFusionTime;

    std::unique_ptr<juce::OpenGLShaderProgram::Uniform>
        uFusionLayA[IFusionShaderSource::kMaxLayers],
        uFusionLayB[IFusionShaderSource::kMaxLayers],
        uFusionLayC[IFusionShaderSource::kMaxLayers],
        uFusionLayD[IFusionShaderSource::kMaxLayers];

    std::unique_ptr<juce::OpenGLShaderProgram> geoShader;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform>
        uGeoInst, uGeoInstTexW, uGeoModuleSize, uGeoSegs, uGeoAaPad;
    std::unique_ptr<juce::OpenGLShaderProgram> geoBlurShader;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform>
        uGbTex, uGbRes, uGbOffset, uGbStep;
    std::unique_ptr<juce::OpenGLShaderProgram> geoCompShader;
    std::unique_ptr<juce::OpenGLShaderProgram::Uniform>
        uGcScene, uGcVeil, uGcRes, uGcOffset, uGcBloomGain, uGcVeilOpacity,
        uGcVeilTexel, uGcBgIn, uGcBgMid, uGcBgOut, uGcNoBg;

    unsigned int quadVAO = 0, quadVBO = 0;
    unsigned int geoVAO  = 0;              // attribute-less, for the gl_VertexID passes
    // ONE FRAMEBUFFER PER SOURCE, not one shared between them.
    //
    // A single shared FBO was fine while "offscreen" meant one detached window.
    // Transparent export broke that assumption: it puts EVERY GL module offscreen
    // at once, and in the HUD they all have different sizes — so each one found
    // the buffer at the wrong size, released it and allocated a new one, several
    // times per frame, on the GL thread, while srcLock was held for the whole
    // frame. Repeated FBO allocation is about the worst thing you can ask a
    // driver for; the message thread then blocked on srcLock behind it and the
    // app stopped responding, which is why Stop could not even be clicked.
    //
    // Keyed by source pointer. They live until the context goes away — a few MB
    // of GPU memory for a module that stopped being offscreen is a much better
    // trade than reallocating one every frame.
    std::map<const void*, std::unique_ptr<juce::OpenGLFrameBuffer>> offscreenFBs;

    /** GL THREAD. This source's own offscreen target, at the size it asked for. */
    juce::OpenGLFrameBuffer* offscreenFor (const void* key, int w, int h)
    {
        auto& slot = offscreenFBs[key];
        if (slot == nullptr) slot = std::make_unique<juce::OpenGLFrameBuffer>();
        return ensureFB (*slot, context, w, h) ? slot.get() : nullptr;
    }

    // Geometry render targets. Shared by every geometry module in the window: the
    // GL thread draws them one after another, so one set is enough however many
    // there are. Sized to whichever tile is currently being drawn.
    juce::OpenGLFrameBuffer geoSceneFB;    // native resolution, premultiplied ARGB
    juce::OpenGLFrameBuffer geoDown0;      // 1/2 — exact 2x2 box of the scene
    juce::OpenGLFrameBuffer geoDown1;      // 1/4 — exact 2x2 box of geoDown0
    juce::OpenGLFrameBuffer geoVeilTmp;    // 1/8 — horizontal gaussian
    juce::OpenGLFrameBuffer geoVeil;       // 1/8 — vertical gaussian; this is the veil

    juce::CriticalSection srcLock;
    std::vector<ISynShaderSource*> sources;
    std::vector<ISgShaderSource*>  sgSources;
    std::vector<IGeoShaderSource*> geoSources;
    std::vector<IFusionShaderSource*> fusionSources;
    std::map<ISgShaderSource*,  juce::uint32> sgOffscreenLastMs;   // GL thread only
    std::map<IGeoShaderSource*, juce::uint32> geoOffscreenLastMs;  // GL thread only
    std::map<IFusionShaderSource*, juce::uint32> fusionOffscreenLastMs;  // GL thread only

    juce::CriticalSection      deadTexLock;
    std::vector<unsigned int>  deadTextures;   // freed at the top of the next frame
    std::vector<const void*>   deadOffscreenKeys;   // ditto, for per-source framebuffers

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AlterGLHost)
};
