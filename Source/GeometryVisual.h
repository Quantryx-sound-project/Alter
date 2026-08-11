/*
  ==============================================================================

    GeometryVisual.h
    Generative geometric visual for ALTER.

    Basic shapes (triangle / square / circle) rush toward the viewer through a
    perspective tunnel - it feels like falling forward. Each shape is born tiny
    in the distance and grows with 1/z perspective until it passes the camera and
    is recycled (as a freshly chosen shape), so the on-screen count stays fixed
    (= COMPLEXITY). With no audio the whole thing fades out.

    Shapes are emitted back-to-front (painter's algorithm) so a freshly born shape
    always appears BEHIND the nearer ones and never flickers in front of them.

    WHERE THE WORK HAPPENS
    ──────────────────────
    The SIMULATION runs on a worker thread (AsyncVisualBase) and produces a flat
    array of GeoInstance records — centre, radius, rotation, colour, stroke width,
    glow reach. It never rasterises.

    The PIXELS come from the GPU. AlterGLHost uploads that array as a small
    floating-point texture and expands each record into a stroked outline in the
    vertex shader, then runs a glow pass, a blur pyramid and one composite (see
    GeoShader.h). The module's per-frame CPU cost is therefore the simulation
    alone, and is independent of how large the module is on screen — which is what
    used to make the old path stutter when the HUD was enlarged or when Bloom was
    automated from the DAW.

    The old CPU renderer is still here and still correct. It runs whenever the GPU
    path is unavailable — no GL context in this window, or a driver that could not
    build the shader — so the module degrades to its previous behaviour rather than
    to a black rectangle. Both paths consume the SAME instance array, so there is
    only one simulation to reason about.

    SPEED MODES
    ───────────
      • FREE   : a continuous ring of COMPLEXITY shapes falls forward at SPEED
                 (negative SPEED = reverse / receding).
      • BPM    : new shapes are SPAWNED on a beat division. COMPLEXITY = how many
                 shapes per spawn; the BEAT DIVISION knob = when they spawn. BPM
                 defaults to 100 and is overwritten by the host tempo the plugin
                 forwards into AlterState.

    Controls:
      COMPLEXITY : free = shapes alive at once; bpm = shapes per spawn burst.
      SPEED      : how fast shapes fall toward the viewer (negative = reverse).
      ROTATION   : spin strength - 0 = no spin, max = fast self-spin.
      RANDOM     : spread of each new shape's birth angle (0 = aligned → spiral).
      REACT      : how strongly the visual reacts to the audio (pulse + shake).
      TRIANGLE / SQUARE / CIRCLE : probability weights for the shape mix.
      SYMMETRY   : number of rotated copies (kaleidoscope).
      ZOOM / SATURATION / BLOOM / SMOOTH : as in Synesthesia.
      COLOUR     : base colour for the analogous palette, or TONE-dependent.

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <functional>
#include <vector>
#include <algorithm>
#include <cmath>
#include <memory>
#include <atomic>
#include "AudioSourceInterface.h"
#include "AlterTheme.h"
#include "AsyncVisualBase.h"
#include "GeoShader.h"
#include "AlterGLHost.h"
#include "PitchUtils.h"

class GeometryVisual : public AsyncVisualBase,
                       public IGeoShaderSource,
                       private juce::Timer
{
public:
    explicit GeometryVisual (IAudioSource& src)
        : AsyncVisualBase ("AlterGeometry", 60), audioSource (src)
    {
        // Transparent: on the GPU path the host draws our pixels straight into
        // this rectangle. On the CPU fallback our own paint() blits the worker's
        // frame, which is opaque and covers the whole area anyway.
        setOpaque (false);

        startTimerHz (60);    // placement + light snapshot for the GL host
        startAsyncRender();
    }

    // Stop the worker BEFORE our members are destroyed (it calls buildFrame()),
    // then leave the host's registry — removeSource blocks until the GL thread
    // has finished any frame that could still be touching us — and only then hand
    // the instance texture over for deletion.
    ~GeometryVisual() override
    {
        stopAsyncRender();
        stopTimer();

        if (currentHost != nullptr)
        {
            currentHost->removeSource (static_cast<IGeoShaderSource*> (this));
            currentHost->scheduleTextureDelete (instTex);
            instTex = 0;
            currentHost = nullptr;
        }
    }

    // ── Note divisions for BPM spawn mode (index → beats per spawn) ────────────
    // Full range + dotted (.) + triplet (T). 1 beat = quarter note (4/4).
    static int   numBeatDivisions() noexcept { return 12; }
    static float beatsForDivision (int idx) noexcept
    {
        static const float beats[12] =
        {
            16.0f,       // 0  4/1   four bars (slow)
            8.0f,        // 1  2/1   two bars
            4.0f,        // 2  1/1   whole
            2.0f,        // 3  1/2   half
            4.0f / 3.0f, // 4  1/2T  half triplet
            1.0f,        // 5  1/4   quarter   (default)
            2.0f / 3.0f, // 6  1/4T  quarter triplet
            0.5f,        // 7  1/8   eighth
            1.0f / 3.0f, // 8  1/8T  eighth triplet
            0.25f,       // 9  1/16  sixteenth
            1.0f / 6.0f, // 10 1/16T sixteenth triplet
            0.125f       // 11 1/32  thirty-second
        };
        return beats[juce::jlimit (0, 11, idx)];
    }
    static const char* labelForDivision (int idx) noexcept
    {
        static const char* names[12] =
        { "4/1", "2/1", "1/1", "1/2", "1/2T", "1/4", "1/4T",
          "1/8", "1/8T", "1/16", "1/16T", "1/32" };
        return names[juce::jlimit (0, 11, idx)];
    }

    // ── Synesthesia-style control API ─────────────────────────────────────────
    // "Tone smooth": HIGH = heavier smoothing (slow reaction), LOW = fast — the EMA
    // coefficient is the opposite, so invert (matches Synesthesia).
    void setSmoothAmount (float s01) noexcept
    {
        smoothAmount = juce::jlimit (0.01f, 0.99f, 1.0f - s01);
        // The RAW slider is kept as well, because the hue and the RMS envelope want
        // different things from it. smoothAmount above still drives smoothRMS, whose
        // full 0.01..0.99 travel was always live. The HUE went through a second,
        // narrower clamp — jlimit(0.02, 0.5, smoothAmount) — applied AFTER the
        // inversion, so every slider position from 0 to 0.5 pinned to the same
        // ceiling and half the travel did nothing to the colour. toneSmoothToRate
        // maps onto those endpoints instead of clipping against them.
        toneSmooth01 = juce::jlimit (0.0f, 1.0f, s01);
    }
    void setZoom (float z)        noexcept { zoom = juce::jlimit (0.5f, 2.0f, z); }
    /** Per-OBJECT spin (-360..0..+360): 0 = still, +/- = spin each way (bipolar). */
    void setRotation (float deg)  noexcept { spinStrength = juce::jlimit (-1.0f, 1.0f, deg / 360.0f); }
    /** Whole-MODULE rotation (-360..0..+360): 0 = still, +/- spins the arrangement each way. */
    void setGlobalRotation (float deg) noexcept { globalSpinStrength = juce::jlimit (-1.0f, 1.0f, deg / 360.0f); }
    /** SYMMETRY 1..8: how evenly the shapes are arranged around the centre. 1 = scattered
        (random angles, as before); 8 = mathematically even circle (with a low complexity
        like 6 → a flower-of-life arrangement). Works alongside Mirror. */
    void setSymmetry (int s)      noexcept { symmetry = juce::jlimit (1, 8, s); }
    void setSaturation (float s)  noexcept { saturation = juce::jlimit (0.0f, 2.0f, s); }
    /** Overall light output 0..2, 1 = neutral (scales shape brightness AND bloom gain). */
    void setBrightness (float b)  noexcept { brightness = juce::jlimit (0.0f, 2.0f, b); }
    void setBloom (float b)       noexcept { bloom = juce::jlimit (-1.0f, 1.0f, b); }   // bipolar: − dims, + boosts
    /** Speed = falling motion speed. Positive = shapes rush toward the viewer,
        0 = frozen depth, negative = reverse (shapes recede away from the camera). */
    void setShaderSpeed (float s) noexcept { forwardSpeed = juce::jlimit (-4.0f, 4.0f, s); }

    // ── Geometry-specific ──────────────────────────────────────────────────────
    void setBaseColour (juce::Colour c) noexcept { baseColour = c; }
    void setToneDependent (bool b)      noexcept { toneDependent = b; }

    /** 'Mirror tone color': reverses the tone→hue wheel direction (see PitchUtils).
        Only has an effect in tone-colour mode; the manual base colour is untouched. */
    void setToneTwist (bool b)          noexcept { toneTwist = b; }
    void setSubBinInterp  (bool b)      noexcept { subBinInterp = b; }
    void setRandomization (float r01)   noexcept { randomization = juce::jlimit (0.0f, 1.0f, r01); }
    /** 0 = ignores the audio (steady), 1 = strong pulse + shake. */
    void setReactivity (float r01)      noexcept { reactivity = juce::jlimit (0.0f, 1.0f, r01); }
    void setShapeWeights (float tri, float sq, float circ) noexcept
    {
        wTri    = juce::jmax (0.0f, tri);
        wSquare = juce::jmax (0.0f, sq);
        wCircle = juce::jmax (0.0f, circ);
    }
    /** FREE mode: shapes alive at once. BPM mode: shapes per spawn burst.
        The ceiling is 256 rather than 48 — on the GPU a shape is a few hundred
        vertices, so the old limit was a CPU-rasteriser limit that no longer
        applies. (The worker still clamps FREE mode down when it is falling back to
        the CPU renderer, where 256 stroked paths per frame really would hurt.) */
    void setComplexity (int layers) noexcept { complexity = juce::jlimit (1, kMaxComplexity, layers); }

    static constexpr int kMaxComplexity = 256;

    // ── BPM spawn mode ─────────────────────────────────────────────────────────
    /** true = spawn shapes on the beat (BPM mode); false = continuous FREE ring. */
    void setBpmSync (bool on)        noexcept { bpmSync = on; }
    /** Tempo used by BPM mode. Defaults to 100; the plugin overwrites this with the
        host's project tempo through AlterState (just like any automatable param). */
    void setBpm (float beatsPerMin)  noexcept { bpm = juce::jlimit (20.0f, 400.0f, beatsPerMin); }
    /** Beat-division index (see beatsForDivision): when a new spawn burst happens. */
    void setBeatDivision (int idx)   noexcept { beatDivIdx = juce::jlimit (0, numBeatDivisions() - 1, idx); }

    /** DEPTH — master control of the whole depth feel. Strongly sets the tunnel
        DEPTH (perspective spawn distance), AND the length over which shapes fade in/
        out, AND the line thickness-vs-proximity (near = thick, far = thin/small; the
        very-near weakening is unchanged, just mapped here). 0 = shallow, 1 = very deep. */
    void setDepth (float d01) noexcept
    {
        depth01 = juce::jlimit (0.0f, 1.0f, d01);
        depth   = 0.25f * std::pow (0.0015f / 0.25f, depth01);   // 0.25 (shallow) .. 0.0015 (very deep) — strong
    }

    /** TUNNEL: 0 = shapes spread AROUND the centre point (a field/ring around centre),
        1 = shapes all converge to a single centre point that, with perspective, opens
        into a tube you fly THROUGH. Blends the placement radius from constant (spread)
        to perspective-scaled (born at centre, sweeping out as they near). */
    void setTunnel (float t01) noexcept { tunnel = juce::jlimit (0.0f, 1.0f, t01); }

    /** APERTURE (photography iris): 1 = shape edges always cross the centre so it's
        never empty (iris closed), 0 = shapes shrink to a thin ring with an open, empty
        centre (iris wide open). Scales each shape's size relative to its ring offset. */
    void setAperture (float a01) noexcept { aperture = juce::jlimit (0.0f, 1.0f, a01); }

    /** MIRROR: reflect every shape into all four quadrants for a clean left/right +
        top/bottom symmetric tunnel. Works alongside Symmetry. */
    void setMirror (bool b) noexcept { mirror = b; }

    // ═══════════════════════════ IGeoShaderSource ════════════════════════════
    GeoShaderState getGeoState() const override
    {
        const juce::ScopedLock sl (stateLock);
        return snapshot;
    }

    /** GL THREAD. Publish whatever the worker produced since the last frame into
        the instance texture and bind it. */
    bool geoGlPrepare (juce::OpenGLContext&, int texUnitInstances, GeoFrameData& out) override
    {
        using namespace juce::gl;

        // Take the worker's latest array. Swapping (rather than copying) means the
        // worker's buffer becomes ours and its next frame reuses the capacity we
        // hand back, so neither side allocates in the steady state.
        {
            const juce::ScopedLock sl (instLock);
            if (instDirty)
            {
                glInstances.swap (pendingInstances);
                glSegments = pendingSegments;
                instDirty  = false;
            }
        }

        const int count = juce::jmin ((int) glInstances.size(), kGeoMaxInstances);
        const int rows  = juce::jmax (1, (count + kGeoInstTexW - 1) / kGeoInstTexW) * 3;

        if (instTex == 0)
        {
            glGenTextures (1, &instTex);
            if (instTex == 0) return false;
            texRows = -1;
        }

        glActiveTexture ((GLenum) (GL_TEXTURE0 + texUnitInstances));
        glBindTexture (GL_TEXTURE_2D, instTex);

        if (texRows != rows)
        {
            // NEAREST + CLAMP is not a filtering choice — the shader uses texelFetch,
            // which ignores filtering entirely. It is here because a texture with the
            // default mipmap MIN_FILTER and no mipmaps is INCOMPLETE, and an
            // incomplete texture reads as black on some drivers.
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA32F, kGeoInstTexW, rows, 0,
                          GL_RGBA, GL_FLOAT, nullptr);
            texRows = rows;
        }

        if (count > 0)
        {
            // One upload per frame. The array is padded out to full rows so the
            // whole block is contiguous — 3 texels per record, kGeoInstTexW records
            // per row-triple. At the 8192-instance ceiling that is 96 KB, which is
            // three orders of magnitude less than the frame the CPU path used to
            // push, and it does not grow when the module is enlarged.
            const int fullRows = rows / 3;
            uploadScratch.assign ((size_t) fullRows * (size_t) kGeoInstTexW * 12, 0.0f);

            for (int i = 0; i < count; ++i)
            {
                const int  col = i % kGeoInstTexW;
                const int  row = i / kGeoInstTexW;
                const auto& s  = glInstances[(size_t) i];
                float* d0 = uploadScratch.data() + ((size_t) (row * 3    ) * kGeoInstTexW + (size_t) col) * 4;
                float* d1 = uploadScratch.data() + ((size_t) (row * 3 + 1) * kGeoInstTexW + (size_t) col) * 4;
                float* d2 = uploadScratch.data() + ((size_t) (row * 3 + 2) * kGeoInstTexW + (size_t) col) * 4;
                d0[0] = s.cx;    d0[1] = s.cy;    d0[2] = s.radius; d0[3] = s.rot;
                d1[0] = s.r;     d1[1] = s.g;     d1[2] = s.b;      d1[3] = s.a;
                d2[0] = s.halfW; d2[1] = s.glowW; d2[2] = s.sides;  d2[3] = s.glowGain;
            }

            glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, kGeoInstTexW, rows,
                             GL_RGBA, GL_FLOAT, uploadScratch.data());
        }

        out.instanceCount = count;
        out.segments      = glSegments;
        out.texUnit       = texUnitInstances;
        return true;
    }

    void geoGlRelease() override
    {
        // The context is going away and takes its objects with it; just forget the
        // name so the next context allocates a fresh one.
        instTex = 0;
        texRows = -1;
    }

    void deliverGeoOffscreenImage (const juce::Image& img) override
    {
        {
            const juce::ScopedLock sl (imgLock);
            offscreenImg = img;
            ++offscreenGen;
        }
        juce::Component::SafePointer<GeometryVisual> sp (this);
        juce::MessageManager::callAsync ([sp]() mutable { if (sp != nullptr) sp->repaint(); });
    }

    /** ANY THREAD. Runs `fn` on the last frame the GL host rendered for us, while
        holding the image lock so it cannot be replaced mid-read.

        For Fusion, which uploads a layer's frame into a texture from the GL
        thread. `generation` counts delivered frames, so a caller that caches the
        result gets false when nothing new has arrived. Keep `fn` short. */
    bool readOffscreenFrame (juce::uint32& generation,
                             const std::function<void (const juce::Image&)>& fn)
    {
        const juce::ScopedLock sl (imgLock);

        if (! offscreenImg.isValid() || offscreenGen == generation)
            return false;

        generation = offscreenGen;
        fn (offscreenImg);
        return true;
    }

    /** Render through the FBO even when this window HAS a GL context. Used by
        Fusion, which needs the frame as an image to composite. */
    void setForceOffscreen (bool shouldForce)
    {
        if (forceOffscreen == shouldForce) return;
        forceOffscreen = shouldForce;
        updateHostRegistration();
    }

private:
    // ═══════════════════════ MESSAGE THREAD (placement) ══════════════════════

    /** On the GPU path the host owns our pixels, so the worker skips its back
        buffer entirely and calls renderHeadless instead. */
    bool producesFrameImage() const override { return ! gpuActive.load(); }

    /** True on BOTH paths now. On the CPU fallback our own renderImage skips the
        gradient; on the GPU path the compositor in AlterGLHost honours uNoBg and
        composites the veil and the strokes onto nothing, so their own coverage
        survives into the alpha channel. */
    bool transparencyReachesPixels() const override { return true; }

    void paint (juce::Graphics& g) override
    {
        if (! gpuActive.load())
        {
            // CPU fallback: present the frame the worker rasterised.
            paintLastRenderedFrame (g);
            return;
        }

        if (isOffscreen)
        {
            // Detached window: no context of its own, so the primary host renders
            // us into an FBO and we blit the result.
            const juce::ScopedLock sl (imgLock);
            if (offscreenImg.isValid())
                g.drawImage (offscreenImg, getLocalBounds().toFloat());
            else if (! isTransparentBackground())
            // On transparency this fallback would stamp an opaque gradient into a
            // fusion or an alpha export — the very thing being exported AROUND.
                AlterTheme::paintBackground (g, getLocalBounds().toFloat());
        }
        // Inline with a host: nothing — the shader has already drawn our rectangle.
    }

    void parentHierarchyChanged() override { updateHostRegistration(); updateGeoState(); }
    void resized() override                { updateGeoState(); }
    void moved() override                  { updateGeoState(); }
    void visibilityChanged() override      { updateGeoState(); }

    void timerCallback() override
    {
        // The context can be created after we are, in which case
        // parentHierarchyChanged found no host to join. Keep looking.
        if (currentHost == nullptr)
            updateHostRegistration();

        updateGeoState();
    }

    void updateHostRegistration()
    {
        // Host of THIS window: found for the HUD, null for a detached window.
        auto* localHost = AlterGLHost::forComponent (this);
        // Detached -> render offscreen via the primary (HUD) host instead.
        AlterGLHost* target = (localHost != nullptr) ? localHost : AlterGLHost::getPrimary();

        // forceOffscreen: a Fusion layer needs PIXELS it can composite, not
        // geometry painted straight into the window behind everything. It uses the
        // host it already has, just through the FBO path.
        isOffscreen = forceOffscreen ? (target != nullptr)
                                     : ((localHost == nullptr) && (target != nullptr));

        if (target != currentHost)
        {
            if (currentHost != nullptr)
            {
                currentHost->removeSource (static_cast<IGeoShaderSource*> (this));
                // The texture belongs to the OLD context; hand it back to it.
                currentHost->scheduleTextureDelete (instTex);
                instTex = 0;
                texRows = -1;
            }
            currentHost = target;
            if (currentHost != nullptr)
                currentHost->addSource (static_cast<IGeoShaderSource*> (this));
        }

        // GPU only when there is a host to draw us AND its shader actually built.
        // The worker reads this to choose its path, so publish it last.
        const bool gpu = (currentHost != nullptr) && AlterGLHost::geoGpuAvailable();
        if (gpu != gpuActive.load())
        {
            gpuActive.store (gpu);
            repaint();          // the two paths present differently
        }
    }

    void updateGeoState()
    {
        // SMOOTHED, not sampled. Automation arrives as a staircase — a host emits
        // one value per block and the knob itself jumps — and light responds to it
        // instantly, so every step in the lane shows up as a visible flicker in the
        // glow. A one-pole lag of a few frames is far below the threshold where the
        // control feels sluggish and completely removes the stepping.
        dispBloom      += (juce::jmax (0.0f, bloom) - dispBloom)      * 0.30f;
        dispBrightness += (brightness               - dispBrightness) * 0.30f;

        GeoShaderState st;
        st.bloom      = dispBloom;
        st.brightness = dispBrightness;
        st.offscreen  = isOffscreen;
        st.noBackground = isTransparentBackground();

        if (! gpuActive.load())
        {
            // Falling back to the worker's rasteriser: tell the host to skip us
            // entirely rather than have it composite a tile our own opaque paint()
            // is about to overwrite.
            const juce::ScopedLock sl (stateLock);
            snapshot = st;                       // st.active is still false
            return;
        }

        if (isOffscreen)
        {
            // FBO size = our own pixel size; the detached window blits the result.
            st.x = 0; st.y = 0;
            st.w = getWidth(); st.h = getHeight();
            st.active = isShowing() && getWidth() > 1 && getHeight() > 1;
        }
        else if (currentHost != nullptr)
        {
            if (auto* hc = currentHost->getAttachedComponent())
            {
                const auto r = hc->getLocalArea (this, getLocalBounds());
                st.x = r.getX();     st.y = r.getY();
                st.w = r.getWidth(); st.h = r.getHeight();
                st.active = isShowing() && r.getWidth() > 1 && r.getHeight() > 1;
            }
        }

        const juce::ScopedLock sl (stateLock);
        snapshot = st;
    }

    // ═══════════════════ WORKER THREAD (simulation, GPU path) ════════════════

    void renderHeadless (int w, int h) override
    {
        buildFrame (tickDelta(), w, h);

        const juce::ScopedLock sl (instLock);
        pendingInstances.swap (instances);   // hand the array over; take back the
        pendingSegments = segmentsForFrame;  // buffer the GL thread finished with
        instDirty = true;
    }

    // ═══════════════ WORKER THREAD (rasterisation, CPU fallback) ═════════════

    void renderImage (juce::Graphics& g, int w, int h) override
    {
        // A single fixed budget. The old code keyed this off the Bloom value in
        // three discrete steps, so AUTOMATING Bloom across a step boundary
        // reallocated every image buffer each frame and made the resolution
        // visibly pop. This path is now the fallback, so it takes the steady
        // middle setting and never changes it.
        setRenderPixelBudget (900000);

        buildFrame (tickDelta(), w, h);

        auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h);
        paintModuleBackground (g, bounds);

        if (instances.empty())
            return;

        // ── IMAGE-SPACE BLOOM (the "melting" look) ────────────────────────────
        // Stroke-glow alone can never make two shapes flow INTO each other — each
        // halo is per-path. For real bloom the whole scene is rendered into an
        // offscreen layer, a downscaled copy is blurred, and the blur is composited
        // back over the crisp strokes: where shapes are close their glows sum
        // inside the blur and they visually melt together.
        const bool imgBloom = bloom > 0.02f;
        std::unique_ptr<juce::Graphics> sg;
        juce::Graphics* tg = &g;
        if (imgBloom)
        {
            if (! sceneImg.isValid() || sceneImg.getWidth() != w || sceneImg.getHeight() != h)
                sceneImg = juce::Image (juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
            else
                sceneImg.clear (sceneImg.getBounds());
            sg = std::make_unique<juce::Graphics> (sceneImg);
            tg = sg.get();
        }

        for (const auto& s : instances)
            drawInstance (*tg, s);

        if (imgBloom)
        {
            sg.reset();   // close the Graphics before reading sceneImg

            const float bp = juce::jmax (0.0f, bloom);
            // 1/6 res: each blur pixel spans ~6 screen px → a much wider, softer
            // spread for the same blur cost.
            const int   bw = juce::jmax (8, w / 6), bh = juce::jmax (8, h / 6);
            if (! bloomImg.isValid() || bloomImg.getWidth() != bw || bloomImg.getHeight() != bh)
                bloomImg = juce::Image (juce::Image::ARGB, bw, bh, true, juce::SoftwareImageType());
            else
                bloomImg.clear (bloomImg.getBounds());
            {
                juce::Graphics bg (bloomImg);
                bg.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
                bg.drawImage (sceneImg, 0, 0, bw, bh, 0, 0, w, h);
            }
            // REAL bloom = blur + GAIN. A box blur *averages* (an isolated line's
            // halo dims with the spread); boosting the blurred result back up is
            // what makes it read as radiance.
            boxBlurARGB (bloomImg, 2 + (int) std::round (bp * 6.0f), 2);
            gainARGB    (bloomImg, (1.8f + bp * 2.2f)
                                   * juce::jlimit (0.5f, 1.6f, 0.4f + 0.6f * brightness));

            // glow UNDER the cores: the halo radiates around the crisp lines
            g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
            g.setOpacity (juce::jlimit (0.0f, 1.0f, 0.60f + 0.40f * bp));
            g.drawImage (bloomImg, bounds);
            g.setOpacity (1.0f);
            g.drawImageAt (sceneImg, 0, 0);           // crisp bright cores on top
        }
    }

    /** Frame-rate-independent (linear) time step from the worker's own clock. */
    float tickDelta()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const float  dt  = (lastTickMs > 0.0)
                             ? (float) juce::jlimit (0.0, 0.10, (now - lastTickMs) / 1000.0)
                             : 1.0f / 60.0f;
        lastTickMs = now;
        return dt;
    }

    // ═══════════════════ WORKER THREAD (the one simulation) ══════════════════

    /** Advance the simulation and emit this frame's shapes into `instances`,
        sorted back-to-front. Coordinates are module-local logical pixels. */
    void buildFrame (float dt, int w, int h)
    {
        advance (dt);

        instances.clear();
        segmentsForFrame = 48;

        auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h);

        // fade the whole scene with the signal: silent → dark / off
        const float activity = juce::jlimit (0.0f, 1.0f, smoothRMS * 5.0f);
        if (activity < 0.01f) return;

        const float minDim = juce::jmin (bounds.getWidth(), bounds.getHeight());

        // audio-driven camera shake (trembling), scaled by REACT
        const float shakeAmp = reactivity * juce::jlimit (0.0f, 1.0f, smoothRMS) * minDim * 0.035f;
        const float cx = bounds.getCentreX() + (rng.nextFloat() - 0.5f) * 2.0f * shakeAmp;
        const float cy = bounds.getCentreY() + (rng.nextFloat() - 0.5f) * 2.0f * shakeAmp;

        const float wSum = juce::jmax (0.0001f, wTri + wSquare + wCircle);
        const float triCum = wTri / wSum;
        const float sqCum  = (wTri + wSquare) / wSum;

        const float baseHue = toneDependent ? smoothHue : baseColour.getHue();

        // THE PICKED COLOUR IS A COLOUR, NOT JUST A HUE.
        //
        // Manual mode used to keep baseColour.getHue() and nothing else, then rebuild
        // the stroke at the Saturation slider's value and full depth brightness. Hue
        // is meaningless on an achromatic colour — JUCE reports 0 (red) for white,
        // black and every grey — so picking white with Saturation at 1 drew pure RED,
        // and picking a deep navy drew a bright primary blue. The two axes the user
        // actually moved in the picker were being thrown away.
        //
        // These factors put them back as MULTIPLIERS, so the Saturation and Brightness
        // sliders still work exactly as before on a fully-saturated pick (both = 1)
        // and now scale a muted or dark pick instead of overriding it.
        // Tone mode is unaffected: the note supplies the whole colour there.
        const float baseSatMul = toneDependent ? 1.0f : baseColour.getSaturation();
        const float baseBriMul = toneDependent ? 1.0f : baseColour.getBrightness();

        std::vector<float> fft;
        const int bins = audioSource.getLastFft (fft);

        // BIPOLAR bloom: positive = boost (a wider, brighter stroke and a real
        // halo), negative = dim below the neutral look down to a faint hairline.
        const float bp = juce::jmax (0.0f,  bloom);
        const float bn = juce::jmax (0.0f, -bloom);

        auto smoothstep = [] (float a, float b, float x)
        {
            const float t = juce::jlimit (0.0f, 1.0f, (x - a) / (b - a));
            return t * t * (3.0f - 2.0f * t);
        };

        float maxRadius = 1.0f;

        // Emit one shape instance for the given depth-progress (phase 0..1) and the
        // two independent hashes that fix its shape type and its birth angle.
        auto emitShape = [&] (float phase, juce::uint32 seedShape, juce::uint32 seedAngle, int shapeIdx)
        {
            // DEPTH = shape lifetime. depth01=1 → shapes persist right up to the
            // camera (barely fade in the distance); depth01=0 → they fade out early.
            const float fadeIn  = 0.04f;
            const float fadeOut = juce::jmap (depth01, 0.0f, 1.0f, 0.70f, 0.995f);
            const float fade = smoothstep (0.0f, fadeIn, phase) * (1.0f - smoothstep (fadeOut, 1.0f, phase));
            if (fade <= 0.01f) return;

            // 1/z perspective: far tiny, near rushes past.
            const float zRem   = (1.0f - phase) + depth;
            const float persp  = depth / zRem;                                 // 0 (far) .. ~1 (near)
            // ZOOM = uniform scale; APERTURE = size relative to the ring offset so the
            // shape edges either fill the centre (1) or leave an open ring (0).
            const float radius = minDim * zoom * persp * (0.2f + aperture * 1.6f);   // far → small, near → large

            const float hShape = hash01 (seedShape);
            const int   sides  = (hShape < triCum) ? 3 : (hShape < sqCum) ? 4 : 0;   // 0 = circle
            const float rndAngle = hash01 (seedAngle) * juce::MathConstants<float>::twoPi;
            // RANDOM only controls the object's own rotation. TUNNEL=1 aligns them all.
            const float baseAngle = randomization * rndAngle * (1.0f - tunnel);

            // SYMMETRY = number of evenly-spaced POINTS around the centre; COMPLEXITY
            // fills shapes onto those points in sequence (shapeIdx % symmetry).
            const int   sym        = juce::jmax (1, symmetry);
            const float pointAngle = (float) (shapeIdx % sym) / (float) sym
                                     * juce::MathConstants<float>::twoPi + globalSpinAngle;

            // APPROACH: a shape emanates from the centre and moves OUTWARD as it nears
            // (radius grows with perspective), so it reads as coming toward you — not
            // just scaling in place. TUNNEL=1 collapses every point onto the centre.
            const float baseR = minDim * 0.42f * zoom;   // spread scales with Zoom → uniform zoom
            const float ringR = baseR * persp * (1.0f - tunnel);
            const float ox = ringR * std::cos (pointAngle);
            const float oy = ringR * std::sin (pointAngle);

            // audio-reactive pulse, strength = REACT
            float band = smoothRMS;
            if (bins > 0)
            {
                const int b = juce::jlimit (0, bins - 1, (int) (phase * phase * (float) bins * 0.5f));
                band = juce::jmax (smoothRMS * 0.5f, fft[(size_t) b]);
            }
            const float pulse = juce::jlimit (0.0f, 1.5f, (smoothRMS * 0.5f + band) * reactivity);

            // Brightness is STATIC (depth shading only) – it does not pulse with the
            // audio. The module still reacts to sound through the size pulse + shake.
            const float depthBright = 0.35f + 0.65f * phase;
            // DEPTH HUE GRADIENT — capped at PitchUtils::kToneHueSpread so the shapes,
            // the plate and the fractal all settle on the SAME dominant note colour.
            // It was ±0.05 of the wheel (0.6 of a semitone) across the depth range, so
            // the near and far shapes sat on visibly different notes and the average
            // read as a different colour than Chladni's flat hue. ±0.01 keeps the
            // near/far separation legible without moving the note.
            float hue = baseHue + (phase - 0.5f) * PitchUtils::kToneHueSpread;
            hue -= std::floor (hue);
            const float sat = juce::jlimit (0.0f, 1.0f, saturation * baseSatMul);
            const float bri = juce::jlimit (0.0f, 1.0f, depthBright * brightness * baseBriMul);
            // Positive bloom brightens the core, negative bloom fades it.
            //
            // brighter() is DELIBERATE here, and deliberately not a value lift.
            // It walks each RGB channel toward white independently, so the core
            // loses saturation as Bloom rises — pure red goes to 0.74 saturation at
            // the top. That desaturation is wanted: it is what makes a hot line
            // read as hot rather than merely bright. Do not "fix" it to preserve
            // the hue; the hue-preserving version was tried and looks flatter.
            //
            // (The VEIL is a different matter — there the channels are scaled
            // together, because an uneven clip in the blurred layer shifts the hue
            // of the whole glow rather than just whitening the line.)
            const juce::Colour col = juce::Colour::fromHSV (hue, sat, bri, 1.0f)
                                       .brighter (bp * 0.35f);
            const float alpha = juce::jlimit (0.0f, 1.0f, fade * activity * (1.0f - bn * 0.55f));

            // self-spin over time + spiral twist with depth
            const float rot = baseAngle + spinAngle + phase * spinStrength * 9.0f;
            const float pr  = radius * (1.0f + 0.30f * pulse);

            // DEPTH-driven line thickness: far → thin, near → thick.
            const float lineW = juce::jmax (0.4f, (1.0f + depth01 * 3.0f) * persp);
            const float coreW = juce::jmax (0.4f, 1.6f * lineW * (1.0f + bp * 1.2f - bn * 0.45f));

            GeoInstance gi;
            gi.cx = cx + ox;  gi.cy = cy + oy;
            gi.radius = pr;
            gi.rot = rot;
            gi.r = col.getFloatRed(); gi.g = col.getFloatGreen(); gi.b = col.getFloatBlue();
            gi.a = alpha;
            gi.halfW = coreW * 0.5f;
            // Glow REACH beyond the core, in pixels. Grows with the knob and with the
            // stroke, capped so a huge near shape cannot turn into a screen-filling
            // wash. This is the halo that hugs the whole perimeter; the blur pyramid
            // in the host adds the wider bleed on top of it.
            gi.glowW    = juce::jmin (lineW * (1.2f + bp * 9.0f), 48.0f);
            gi.glowGain = bp * 0.55f;
            gi.sides    = (float) sides;
            gi.sortKey  = phase;

            maxRadius = juce::jmax (maxRadius, pr + gi.glowW);

            instances.push_back (gi);
            if (mirror)
            {
                const float pi = juce::MathConstants<float>::pi;
                GeoInstance m1 = gi; m1.cx = cx - ox; m1.cy = cy + oy; m1.rot = pi - rot;
                GeoInstance m2 = gi; m2.cx = cx + ox; m2.cy = cy - oy; m2.rot =    - rot;
                GeoInstance m3 = gi; m3.cx = cx - ox; m3.cy = cy - oy; m3.rot = pi + rot;
                instances.push_back (m1);
                instances.push_back (m2);
                instances.push_back (m3);
            }
        };

        if (! bpmSync)
        {
            // FREE: a ring of COMPLEXITY shapes evenly spaced in depth. The CPU
            // fallback strokes every one of them as a path, so it keeps the old
            // ceiling; the GPU path has no reason to.
            const int L = gpuActive.load() ? complexity : juce::jmin (complexity, 48);
            for (int j = 0; j < L; ++j)
            {
                const double raw    = globalPhase + (double) j / (double) L;
                const double phaseD = raw - std::floor (raw);             // 0..1 depth progress
                const int    cycle  = (int) std::floor (raw);
                const juce::uint32 seedShape = (juce::uint32) (j * 73856093)   ^ (juce::uint32) (cycle * 19349663);
                const juce::uint32 seedAngle = (juce::uint32) (j * 2654435761u) ^ (juce::uint32) (cycle * 40503u);
                emitShape ((float) phaseD, seedShape, seedAngle, j);
            }
        }
        else
        {
            // BPM: each particle was spawned on a beat and travels forward.
            for (const auto& p : particles)
            {
                const float phase = (float) (travelBpm - p.birthTravel);
                if (phase <= 0.0f || phase >= 1.0f) continue;
                const juce::uint32 seedAngle = p.seed * 2654435761u ^ 0x9E3779B9u;
                emitShape (phase, p.seed, seedAngle, p.idx);
            }
        }

        // Back-to-front: far (phase 0) first, nearest last. This is what makes a
        // freshly born shape appear BEHIND the nearer ones instead of flickering in
        // front of them — and sorting it here is what lets the whole array go out as
        // ONE draw call, because a single glDrawArrays rasterises its primitives in
        // order, so the painter's ordering survives without a draw call per shape.
        //
        // The key is the phase, not the radius: the audio pulse scales each shape
        // independently, so a loud far shape can be momentarily larger than a quiet
        // near one and radius is not quite monotonic in depth.
        if (instances.size() > (size_t) kGeoMaxInstances)
            instances.resize ((size_t) kGeoMaxInstances);

        std::sort (instances.begin(), instances.end(),
                   [] (const GeoInstance& a, const GeoInstance& b)
                   { return a.sortKey < b.sortKey; });

        // ── Ring resolution ──────────────────────────────────────────────────
        // Enough segments that the largest circle's faceting stays under ~0.3 px:
        // s ~= pi * sqrt(R / 2e). Kept a multiple of 12 so that triangle and square
        // corners always land exactly on a segment boundary and their edges stay
        // dead straight.
        const int need  = (int) std::ceil (4.055f * std::sqrt (juce::jmax (1.0f, maxRadius)) / 12.0f);
        int       segs  = 12 * juce::jlimit (2, 13, need);

        // ...but bounded by a per-frame VERTEX budget. Resolution and shape count
        // both cost vertices, and at the extreme (thousands of shapes in a fast BPM
        // division) the naive product would be millions of them twice over — for
        // shapes that are individually tiny and cannot show faceting anyway. Trading
        // segments away as the count climbs keeps the cost flat where it matters.
        if (! instances.empty())
        {
            const int budget = 1500000 / (6 * (int) instances.size());
            segs = juce::jmin (segs, 12 * juce::jlimit (2, 13, budget / 12));
        }
        segmentsForFrame = segs;
    }

    // ── Simulation step (worker thread) ───────────────────────────────────────
    void advance (float dt)
    {
        const float rms = getRmsLevel();
        smoothRMS += (rms - smoothRMS) * smoothAmount;

        const bool audible = smoothRMS > 0.015f;

        if (audible)
        {
            spinAngle += dt * spinStrength * 3.5f;             // per-object self-spin
            if (spinAngle > 1.0e6f) spinAngle = std::fmod (spinAngle, juce::MathConstants<float>::twoPi);

            globalSpinAngle += dt * globalSpinStrength * 2.0f; // whole-module rotation
            if (globalSpinAngle > 1.0e6f) globalSpinAngle = std::fmod (globalSpinAngle, juce::MathConstants<float>::twoPi);

            if (toneDependent)
            {
                const float target = detectPitchHue();
                if (target >= 0.0f)
                {
                    float d = target - smoothHue;
                    d -= std::round (d);
                    smoothHue += d * PitchUtils::toneSmoothToRate (toneSmooth01);
                    smoothHue -= std::floor (smoothHue);
                }
            }
        }

        if (! bpmSync)
        {
            if (audible)
                globalPhase += (double) (dt * forwardSpeed * 0.30f);
            return;
        }

        // BPM mode: advance the travel accumulator and spawn bursts on the beat.
        if (audible)
        {
            // Travel follows Speed, INCLUDING negative (reverse). Bursts spawn at the near
            // edge when reversing (see spawnBurst) so shapes recede instead of rushing in.
            const float travelSpeed = (std::abs (forwardSpeed) < 0.05f) ? 0.10f : forwardSpeed;
            travelBpm += (double) (dt * travelSpeed * 0.30f);

            // IMPULSE-SYNCED grid: the Creator plugin sends a one-shot sync impulse
            // when the DAW transport (re)starts. On each new impulse we re-anchor the
            // beat phase to that instant (beat 0 = the downbeat we started from); the
            // RATE and DIVISION stay manual (the BPM slider). Between impulses — and
            // when no plugin is connected — the clock free-runs at the manual BPM.
            IAudioSource::HostSyncInfo hs;
            const bool haveHost = audioSource.getHostSync (hs);

            const double bps  = (double) bpm / 60.0;     // manual RATE always
            const double divB = (double) beatsForDivision (beatDivIdx);
            const double prev = beatClock;

            if (haveHost && hs.pulseId != lastSyncPulseId)   // DAW downbeat/start → re-anchor phase
            {
                lastSyncPulseId = hs.pulseId;
                beatClock = (hs.pulseAgeMs == 0xFFFFFFFFu ? 0.0
                                                          : (double) hs.pulseAgeMs * 0.001 * bps);
            }

            const bool run = haveHost ? hs.playing : true;   // host: follow transport; standalone: run on sound
            if (run)
                beatClock += (double) dt * bps;

            if (beatClock >= prev)   // loop / relocate jumps back → resync silently
            {
                long long a = (long long) std::floor (prev      / divB);
                long long b = (long long) std::floor (beatClock / divB);
                long long bursts = b - a;
                if (bursts > 4) bursts = 4;                     // guard against huge dt / tiny division
                for (long long i = 0; i < bursts; ++i)
                    spawnBurst();
            }
        }

        // age out finished particles (passed the camera OR receded past the far point)
        particles.erase (std::remove_if (particles.begin(), particles.end(),
                          [this] (const Particle& p)
                          { const double ph = travelBpm - p.birthTravel; return ph <= 0.0 || ph >= 1.0; }),
                          particles.end());
    }

    void spawnBurst()
    {
        // Reverse (negative Speed): spawn at the NEAR edge (phase ~1) so shapes recede;
        // forward: spawn at the FAR edge (phase ~0) so they rush in.
        // NOTE: a tiny epsilon keeps the fresh particle strictly INSIDE (0,1) — spawning
        // exactly at 0/1 made the age-out filter (ph <= 0 || ph >= 1) delete every
        // particle in the same frame it was born, so BPM mode never showed anything.
        const double entryPhase = (forwardSpeed >= 0.0f) ? 1.0e-4 : 1.0 - 1.0e-4;
        const int n = complexity;
        for (int i = 0; i < n; ++i)
            particles.push_back ({ travelBpm - entryPhase, (juce::uint32) rng.nextInt(), i });

        // Hard cap so a fast division x big complexity cannot flood the pool. Drop the
        // NEWEST (far, tiny, least-visible) — never the oldest, which are the near/big
        // foreground shapes.
        //
        // On the GPU the cap is one number, not two: every shape costs the same handful
        // of vertices whether or not Mirror is on, so there is no longer any reason to
        // punish Mirror with a lower ceiling — which is what used to make turning
        // COMPLEXITY up show FEWER shapes. The CPU fallback keeps the old, much lower
        // limits, because there every shape is a stroked path and Mirror really is 4x
        // the work.
        const size_t cap = gpuActive.load() ? (size_t) (kGeoMaxInstances / (mirror ? 4 : 1))
                                            : (size_t) (mirror ? 110 : 260);
        if (particles.size() > cap)
            particles.resize (cap);   // keeps the first `cap` (oldest = nearest = visible)
    }

    float getRmsLevel()
    {
        const float raw = audioSource.getLastRms();
        if (raw < 0.001f) return 0.0f;
        const float dB = 20.0f * std::log10 (raw);
        return juce::jlimit (0.0f, 1.0f, (dB + 36.0f) / 36.0f);
    }

    // MIDI override: exact loudest held note → Hz. >0 = use it; 0 = no MIDI on the
    // track (fall back to FFT); <0 = MIDI present but nothing held (no hue update).
    float midiDominantHz()
    {
        if (! audioSource.getMidiNotes (midiScratch)) return 0.0f;
        if (midiScratch.empty())                      return -1.0f;
        const IAudioSource::MidiNote* top = &midiScratch.front();
        for (const auto& n : midiScratch)
            if (n.velocity > top->velocity) top = &n;
        return 440.0f * std::pow (2.0f, ((float) top->note - 69.0f) / 12.0f);
    }

    float detectPitchHue()
    {
        const float mh = midiDominantHz();
        if (mh > 0.0f)
        {
            return PitchUtils::hueFromHz (mh, toneTwist);   // shared wheel
        }
        if (mh < 0.0f) return -1.0f;   // MIDI on, nothing held → no hue update

        std::vector<float> spectrum;
        const int bins = audioSource.getLastFft (spectrum);
        if (bins == 0) return -1.0f;

        // Match Synesthesia EXACTLY so the same tone yields the same hue:
        // search 65 Hz..2 kHz, same bin width, same dominant-frequency picker.
        const double sr = audioSource.getSampleRate();   // real device rate → correct pitch
        const float bw = PitchUtils::binWidthHz (bins, sr);
        const int lo = juce::jmax (1, (int) std::round (65.0f   / bw));
        const int hi =               (int) std::round (2000.0f / bw);
        const float freq = PitchUtils::dominantFrequency (spectrum, lo, hi, 0.01f,
                                                          subBinInterp, sr);
        if (freq < 20.0f) return -1.0f;
        return PitchUtils::hueFromHz (freq, toneTwist);  // shared wheel
    }

    static float hash01 (juce::uint32 x) noexcept
    {
        x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
        return (float) (x & 0xFFFFFF) / (float) 0x1000000;
    }

    // ═══════════════ CPU FALLBACK rasterisation of one instance ══════════════

    void drawInstance (juce::Graphics& g, const GeoInstance& s)
    {
        juce::Path p;
        const int sides = (int) s.sides;
        if (sides < 3)
        {
            p.addEllipse (s.cx - s.radius, s.cy - s.radius, s.radius * 2.0f, s.radius * 2.0f);
        }
        else
        {
            for (int v = 0; v <= sides; ++v)
            {
                const float a = s.rot + (float) v * juce::MathConstants<float>::twoPi / (float) sides;
                const float x = s.cx + std::cos (a) * s.radius;
                const float y = s.cy + std::sin (a) * s.radius;
                if (v == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            p.closeSubPath();
        }

        const juce::Colour col = juce::Colour::fromFloatRGBA (s.r, s.g, s.b, s.a);

        // NO per-shape halo pass — deliberately, and this matches what the original
        // CPU renderer did (it kept the code but pinned glowPassesEff to 0). A halo
        // stroked around each path cannot fuse with its neighbours, so it reads as a
        // second outline rather than as light. All the glow comes from the
        // image-space blur in renderImage, exactly as it did before.
        g.setColour (col);
        g.strokePath (p, juce::PathStrokeType (juce::jmax (0.4f, s.halfW * 2.0f)));
    }

    // Brightness gain with saturation on a (small, premultiplied-ARGB) image —
    // the "bloom" half of blur+gain. Runs on the 1/6-res layer → negligible cost.
    static void gainARGB (juce::Image& img, float gain)
    {
        const int gi = (int) std::round (juce::jmax (1.0f, gain) * 256.0f);
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::readWrite);
        for (int y = 0; y < bd.height; ++y)
        {
            juce::uint8* p = bd.getLinePointer (y);
            const int nBytes = bd.width * bd.pixelStride;
            for (int i = 0; i < nBytes; ++i)
            {
                const int v = (p[i] * gi) >> 8;
                p[i] = (juce::uint8) (v > 255 ? 255 : v);
            }
        }
    }

    // Separable sliding-window box blur on a (small, premultiplied-ARGB) image.
    // Two passes ≈ a gaussian; runs on the 1/6-res bloom layer, so the cost is a
    // tiny fraction of the frame.
    static void boxBlurARGB (juce::Image& img, int radius, int passes)
    {
        if (radius < 1) return;
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::readWrite);
        const int W = bd.width, H = bd.height, ps = bd.pixelStride;
        const int div = radius * 2 + 1;
        std::vector<juce::uint8> tmp ((size_t) juce::jmax (W, H) * 4);

        auto blurLine = [&] (juce::uint8* base, int n, int stride)
        {
            int sum[4] = { 0, 0, 0, 0 };
            for (int i = -radius; i <= radius; ++i)
            {
                const juce::uint8* p = base + juce::jlimit (0, n - 1, i) * stride;
                for (int c = 0; c < 4; ++c) sum[c] += p[c];
            }
            for (int i = 0; i < n; ++i)
            {
                juce::uint8* o = tmp.data() + (size_t) i * 4;
                for (int c = 0; c < 4; ++c) o[c] = (juce::uint8) (sum[c] / div);
                const juce::uint8* add = base + juce::jmin (n - 1, i + radius + 1) * stride;
                const juce::uint8* rem = base + juce::jmax (0,     i - radius)     * stride;
                for (int c = 0; c < 4; ++c) sum[c] += add[c] - rem[c];
            }
            for (int i = 0; i < n; ++i)
            {
                juce::uint8* p = base + i * stride;
                const juce::uint8* o = tmp.data() + (size_t) i * 4;
                for (int c = 0; c < 4; ++c) p[c] = o[c];
            }
        };

        for (int pass = 0; pass < passes; ++pass)
        {
            for (int y = 0; y < H; ++y) blurLine (bd.getLinePointer (y), W, ps);
            for (int x = 0; x < W; ++x) blurLine (bd.getLinePointer (0) + x * ps, H, bd.lineStride);
        }
    }

    // ── Members ───────────────────────────────────────────────────────────────
    IAudioSource& audioSource;
    std::vector<IAudioSource::MidiNote> midiScratch;   // reused per frame (MIDI override)

    // GL host wiring (message thread, except gpuActive which the worker reads)
    AlterGLHost*      currentHost = nullptr;
    bool              isOffscreen = false;   // detached window → rendered via FBO image
    bool              forceOffscreen = false; // hosted inside Fusion → always via FBO image
    std::atomic<bool> gpuActive { false };   // false → renderImage on the worker

    mutable juce::CriticalSection stateLock;
    GeoShaderState snapshot;

    mutable juce::CriticalSection imgLock;
    juce::Image offscreenImg;                // last frame rendered for a detached window
    juce::uint32 offscreenGen = 0;   // ditto; see readOffscreenFrame

    // The instance array crosses worker -> GL thread by swapping buffers, so
    // neither side allocates once the counts have settled.
    juce::CriticalSection    instLock;
    std::vector<GeoInstance> instances;         // worker thread only
    std::vector<GeoInstance> pendingInstances;  // handoff, under instLock
    std::vector<GeoInstance> glInstances;       // GL thread only
    std::vector<float>       uploadScratch;     // GL thread only
    int  segmentsForFrame = 48;                 // worker
    int  pendingSegments  = 48;                 // handoff
    int  glSegments       = 48;                 // GL
    bool instDirty        = false;              // under instLock
    unsigned int instTex  = 0;                  // GL thread only
    int  texRows          = -1;                 // GL thread only

    // synesthesia-style params
    float smoothAmount { 0.15f };
    float toneSmooth01 { 0.15f };   // raw 'Tone smooth' slider; hue rate only
    float zoom         { 1.0f };
    float depth        { 0.0125f };  // perspective spawn distance (set by setDepth)
    float depth01      { 0.7f };     // raw Depth slider 0..1 (drives fade length + line thickness)
    float tunnel       { 0.0f };     // 0 = spread around centre, 1 = converge to centre → tube
    float aperture     { 0.8f };     // iris: 1 = edges fill centre, 0 = thin ring / open centre
    bool  mirror       { false };    // 4-fold reflective symmetry
    float spinStrength { 0.0f };     // per-object spin
    float globalSpinStrength { 0.0f };  // whole-module rotation speed
    float globalSpinAngle    { 0.0f };  // accumulated module rotation
    int   symmetry     { 6 };        // 1..8 number of symmetric points around the centre
    float saturation   { 1.0f };
    float brightness   { 1.0f };     // overall light output (0..2, 1 = neutral)
    float bloom        { 0.0f };
    double lastTickMs { 0.0 };       // for frame-rate-independent (linear) speed
    float forwardSpeed { 1.0f };

    // geometry-specific
    juce::Colour baseColour { AlterTheme::caribbeanGreen };
    bool  toneDependent { false };
    bool  toneTwist     { false };   // 'Mirror tone color': reverse the hue wheel direction
    bool  subBinInterp  { true };   // always on
    float randomization { 1.0f };
    float reactivity    { 0.5f };
    float wTri    { 1.0f };
    float wSquare { 1.0f };
    float wCircle { 1.0f };
    int   complexity { 12 };

    // BPM spawn mode
    bool   bpmSync    { false };
    float  bpm        { 100.0f };
    int    beatDivIdx { 5 };          // default 1/4 (one beat)
    double travelBpm  { 0.0 };        // forward-travel accumulator for spawned particles
    double beatClock  { 0.0 };        // running beat position (beats)
    juce::uint32 lastSyncPulseId { 0 };   // last DAW sync impulse we re-anchored to
    struct Particle { double birthTravel; juce::uint32 seed; int idx; };
    std::vector<Particle> particles;

    // Displayed (lagged) light values — see updateGeoState.
    float  dispBloom      { 0.0f };
    float  dispBrightness { 1.0f };

    float  smoothRMS  { 0.0f };
    float  smoothHue  { 0.33f };
    double globalPhase { 0.0 };
    float  spinAngle  { 0.0f };

    juce::Random rng;

    // image-space bloom layers (CPU fallback only, worker thread)
    juce::Image sceneImg;   // full-res offscreen scene (crisp strokes)
    juce::Image bloomImg;   // 1/6-res blurred copy (the melt/bleed veil)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GeometryVisual)
};
