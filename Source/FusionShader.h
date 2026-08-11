/*
  ==============================================================================

    FusionShader.h
    Shared types so AlterGLHost (the per-window OpenGL host) and FusionVisual can
    talk without an include cycle — the same arrangement SynShader.h, SgShader.h
    and GeoShader.h provide for the fractal, the spectrogram and the geometry.

    ── THE MODEL: A STACK, NOT A MENU ─────────────────────────────────────────
    Fusion is a COMPOSITOR. It draws nothing of its own; it owns a stack of
    layers and a chain of post effects, and that is the whole of it.

    The stack accumulates BOTTOM-UP, exactly as every compositor anyone has used
    already works:

        acc = layer A                       (the base)
        acc = blend(acc, layer B, mode B)
        acc = blend(acc, layer C, mode C)

    Each layer carries its OWN blend mode. That is the change that matters. The
    previous design split the layers into "layer 0" against "everything else
    screened together", which meant B and C could never combine with each other,
    the settings said things about "the two sides" that stopped being true the
    moment a third layer appeared, and slot A was silently privileged. None of
    that was explainable, because none of it was a model — it was one special
    case wearing the clothes of a general one.

    Merge and Weave were global switches in that design. They are BLEND MODES
    here, which is what they always were: both are answers to "how does this layer
    meet the one below it", and asking that question per layer is what makes three
    layers behave like three layers.

    WEAVE is the one that cannot be answered a pair at a time. Bands only read as
    a weave if every layer taking part knows how many others there are, so the
    weaving layers are handled as a GROUP: with two, the bands alternate exactly
    as they always did; with three, they cycle A, B, C. Done pairwise instead, the
    third layer got the leftovers of a decision the first two had already made,
    which is why it kept disappearing.

    ── POST: COORDINATES, NOT COLOURS ─────────────────────────────────────────
    Warp, Mirror, Symmetry, Spin and Zoom never combined anything; they decide
    WHERE a picture is read from. So they run before the blending, on the sampling
    coordinate.

    They are asked TWICE, and the difference is the whole point:

      * per LAYER, on that layer's own sampling coordinate, so two layers can be
        folded differently and still meet in one picture,
      * GLOBALLY, on the coordinate the finished stack is read at, which folds the
        RESULT.

    A fold every layer shares is a fold of the result, and there was previously no
    way to say anything else — every layer saw the same folded space, so a stack
    of three could only ever be three copies of one arrangement.

    MIRROR is a COUNT of reflection axes, evenly spaced about the centre, and it
    is a different stage from SYMMETRY: mirror reflects and leaves the radius
    alone, symmetry builds congruent wedges and folds the radius to fill them.
    Both exist because asking for two mirrors used to mean reaching for symmetry,
    which is a different picture rather than a stronger version of the same one.

    THE ORDER inside the chain is zoom, vortex, wedges, turn, mirror. Vortex is
    first because swirling an already-wedged picture would twist each wedge away
    from its neighbours and open every seam. Mirror is last because folding the
    finished rosette is the picture worth having — folding first only changes what
    the wedges are cut from, which with an even wedge count is barely a change at
    all, and it left the Axis angle being dragged around by Spin.

    ── COVERAGE: REAL ─────────────────────────────────────────────────────────
    A layer's alpha is its coverage, and it is now real for EVERY module. The 2D
    ones are told to skip their background (see ThemedBackground); the three that
    draw themselves on the GPU honour the same instruction through a uNoBg uniform
    in their own shaders, each answering in its own terms — Synesthesia by the
    fractal's brightness, Geometry by unioning the coverage its veil and strokes
    already carried, the Spectrogram by the energy.

    The hasAlpha flag survives as the honest fallback for anything that has not
    been taught the trick, and for Geometry's CPU path. It is no longer the normal
    case for anything.

    ── WHY THE GPU ────────────────────────────────────────────────────────────
    Every output pixel is a function of a few input pixels. On the CPU that cost
    is proportional to module AREA and is paid on a worker thread every frame; the
    render pixel budget existed only to bound it, which is exactly why the module
    used to look pixellated. Uploading finished frames as textures costs the same
    whatever size the module is drawn at, and the fragment shader then evaluates
    the whole chain per output pixel at NATIVE resolution.

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

/** One frame's worth of placement for a Fusion instance. Plain data, written on
    the message thread and read on the GL thread.

    The rectangle is in the HOST component's coordinates (that is where the host
    sets its viewport). A panel can be ROTATED by 90/180/270 degrees via an
    AffineTransform, so the mapping from the rect back into the module's own local
    space is carried explicitly as an affine basis:

        localUV = local0 + u * localDU + v * localDV

    with u across the rect (0 = left, 1 = right) and v down it (0 = top, 1 =
    bottom). For an unrotated panel this is local0 = (0,0), localDU = (1,0),
    localDV = (0,1). Identical in meaning to SgShaderState. */
struct FusionShaderState
{
    bool active    = false;           // false -> host skips this instance
    bool offscreen = false;           // true  -> render to an FBO and hand back an image

    /** Composite onto NOTHING instead of onto the theme gradient, and let the
        fusion's own coverage survive into the alpha channel.

        This is what an alpha export needs, and it is the same instruction the
        layer modules already receive through their own uNoBg uniforms — an
        Fusion simply had no way to be told it until transparent export existed,
        because a fusion drawn straight into the HUD window has nothing useful to
        be transparent against. */
    bool noBackground = false;

    int  x = 0, y = 0, w = 0, h = 0;  // rect in the HOST component's coordinates

    juce::Point<float> local0  { 0.0f, 0.0f };
    juce::Point<float> localDU { 1.0f, 0.0f };
    juce::Point<float> localDV { 0.0f, 1.0f };

    /** Aspect of the module's OWN local space, width / height.

        Not the viewport's aspect once the panel is rotated by 90 or 270 degrees:
        the viewport is then the bounding box of the rotated rectangle, so its
        width and height are swapped relative to the module's. Mirror works in
        local space and needs the local one, or the folds would be squashed the
        moment the panel was turned. */
    float localAspect = 1.0f;
};

/** How a layer meets the one below it. Kept as a float in the uniform block
    because that is what GLSL takes; the shader compares against midpoints. */
enum class FusionBlend
{
    screen = 0,   /**< Light adds up. Both layers stay visible, neither punches a
                       hole in the other. The honest "no effect" default. */
    merge  = 1,   /**< The two stop being two: colours pull toward each other in
                       proportion to local brightness, so the seam runs along the
                       actual shapes, and the overlap comes back as light. */
    weave  = 2    /**< Interleaved bands. The layers trade the foreground stripe by
                       stripe rather than one hiding the other. */
};

/** Everything one layer contributes. */
struct FusionLayerParams
{
    float blend   = 0.0f;   // FusionBlend as float
    float opacity = 1.0f;   // 0..1

    /** 0..3 quarter turns, THE MODULE'S OWN. Applied in the SAMPLING coordinate
        because a layer is never drawn in its own rectangle, so the panel's
        rotation transform never reaches it — this is the only place it can
        happen, and it is free.

        Where it comes from matters as much as where it is applied: the value is
        read off the layer module's own panel, set in the module's own editor. A
        fusion performs the turn; it does not decide it. */
    float rot     = 0.0f;

    float scale   = 1.0f;   // about the layer's centre
    float offX    = 0.0f;   // in layer widths
    float offY    = 0.0f;

    /** Blend-specific. Merge: bias, 0 = all the layer below, 1 = all this one.
        Weave: which side owns how much of a band. */
    float amount  = 0.5f;

    float bands   = 8.0f;   // weave only
    float angle   = 0.0f;   // weave only, radians
    float edge    = 0.6f;   // weave only: band edge hardness

    /** 1 when the module handed us real coverage in its alpha channel, 0 when the
        background still has to be subtracted out (the GL modules). */
    float hasAlpha = 0.0f;

    /** 1 when these pixels are the module's OWN framebuffer, sampled exactly
        where the module drew them, and 0 when they are a finished frame uploaded
        from an image.

        This is not a preference, it is a fact about the texture: GL stores a
        framebuffer bottom-up and an uploaded image top-down, so the sampling v is
        flipped for one and not for the other. Getting it wrong does not look
        subtly wrong, it stands the layer on its head.

        It exists because a layer that draws itself on the GPU no longer makes the
        round trip through main memory — see the note in AlterGLHost about the
        layer targets. */
    float fromFbo = 0.0f;

    // ── this layer's OWN post chain ──────────────────────────────────────────
    //
    // The same four stages the whole stack gets globally, asked once per layer
    // and applied to THIS layer's sampling coordinate before anything else
    // touches it. Two layers can therefore be folded differently and still meet
    // in one picture, which is the only reason to have a stack at all — a fold
    // that every layer shares is a fold of the result, not of the layers.
    //
    // Order inside a layer is the same as the global order, so "mirror then
    // wedges then turn" means one thing in this module and not two.

    /** How many reflection axes, evenly spaced about the layer's centre, with
        the first one at `mirrorAngle`. 0 is no fold. 1 is a single axis, which
        folds one half onto the other; 2 adds a second axis at right angles to
        it; N gives axes every 180/N degrees.

        NOT the same stage as symmetry: mirror reflects and leaves the radius
        alone, symmetry builds congruent wedges and folds the radius to fill
        them. Reaching for one when you wanted the other is the mistake this
        separation exists to prevent. */
    float mirror      = 0.0f;   // 0..8
    float mirrorAngle = 0.0f;   // radians

    float symmetry = 1.0f;      // 1..11 wedges, 1 = no fold
    float spin     = 0.0f;      // radians, already includes this layer's drift
    float zoom     = 1.0f;      // 0.25..4, about the layer's centre
};


/** Everything the host's fragment shader needs for ONE frame. Filled in by the
    source on the GL thread (in fusionGlPrepare, right after it has uploaded the
    layer frames); the host pushes these into the uniforms. */
struct FusionFrameData
{
    int layerCount = 0;               // 0..kMaxLayers; textures are bound to
                                      // consecutive units starting at the one the
                                      // host reserved

    FusionLayerParams layer[3];          // in the SAME COMPACTED ORDER as the textures

    // ── post: coordinate stages ──────────────────────────────────────────────
    float warp      = 0.0f;
    float warpAmt   = 0.5f;           // displacement distance
    float warpSwirl = 0.6f;           // 0 = push along the gradient, 1 = around it

    /** How much of the source's DETAIL the displacement is allowed to follow.

        0 is the raw per-pixel gradient and is exactly what Warp always was. Turned
        up, the field is measured across a wider radius and its peaks are put
        through a soft knee, so it follows the shapes rather than the speckle —
        which is the difference between a warp that flows and one that tears. */
    float warpSmooth = 0.0f;          // 0..1

    /** DENOISE: a small blur on the warped layer reads. Averages away the pixel-to-
        pixel speckle a strong warp tears in, and merges nearby colours and curves
        into simpler shapes. 0 = crisp, exactly as before. */
    float warpDenoise = 0.0f;         // 0..1

    float warpSrc   = 0.0f;           // which layer's gradient drives it

    /** Mirror symmetry: how many congruent wedges the picture is folded into.
        1 means no fold at all, which is why it is the default — the module should
        do nothing it was not asked to do. */
    float symmetry = 1.0f;            // 1..11

    /** Reflection axes, evenly spaced about the centre, the first at
        `mirrorAngle`. 0 is no fold. Independent of the wedge count: symmetry
        makes the picture radial, this just folds halves onto each other, which
        is the thing you want when the layers already have a left and a right.

        It is a COUNT rather than a switch because one axis is only ever the
        first answer — asking for a second one used to mean reaching for
        Symmetry, which folds the radius as well and is therefore a different
        picture, not a stronger version of the same one. */
    float mirror      = 0.0f;         // 0..8
    float mirrorAngle = 0.0f;         // radians

    float spin = 0.0f;                // radians, already includes drift
    float zoom = 1.0f;

    /** Swirl: a turn that GROWS with the radius, so the centre stands still and
        the edge is dragged round. Bipolar, 0 = off.

        Runs before both folds, and it has to: swirling an already-wedged picture
        would twist each wedge away from its neighbours and open every seam.
        Swirl the source, then fold it, and the copies still meet exactly. */
    float vortex = 0.0f;              // -1..1

    /** WHICH layers the whole global chain (fold, tunnel, liquid, warp) touches,
        one per compacted slot. 1 = folded with the result, 0 = read straight. */
    float globLayer[3] = { 1.0f, 1.0f, 1.0f };

    /** LIQUID: a shared flowing displacement that melts the targeted layers into
        one another. amt = travel, smooth = feature scale. */
    float liquid       = 0.0f;
    float liquidAmt    = 0.5f;
    float liquidSmooth = 0.5f;

    /** DENOISE: same small blur as Warp's, on the layers Liquid melts together — so
        the flow reads as one soft body of colour rather than a noisy smear. 0 =
        crisp, exactly as before. */
    float liquidDenoise = 0.0f;       // 0..1

    /** TUNNEL: remap the whole picture into a radially symmetric receding tunnel. */
    float tunnel       = 0.0f;

    /** Wall-clock seconds, so Liquid flows and the Tunnel drifts inward on their
        own. Wrapped to keep float precision. */
    float time         = 0.0f;

    float drive     = 0.0f;           // audio, already scaled by React
};

/** Implemented by FusionVisual; consumed by AlterGLHost. */
struct IFusionShaderSource
{
    static constexpr int kMaxLayers = 3;

    virtual ~IFusionShaderSource() = default;

    /** MESSAGE or GL thread. Cheap snapshot of where this instance lives. */
    virtual FusionShaderState getFusionState() const = 0;

    /** MESSAGE or GL thread. The components this fusion is currently using as
        layers, in slot order, empty slots left null.

        The host needs this BEFORE the fusion is drawn. A layer that renders
        itself on the GPU is given its own framebuffer and drawn into it once, at
        the top of the frame, and the fusion then samples that texture — so the
        host has to know which modules those are while there is still time to
        render them.

        Handing out raw pointers is safe for exactly as long as the call: the GL
        thread holds the source lock for its whole frame, and a module leaving a
        fusion goes through the message thread. */
    virtual void getFusionLayerComponents (juce::Component** out, int maxCount) const = 0;

    /** GL THREAD. Create/refresh this source's layer textures, upload whatever
        frames the capture has produced since the last call, bind them to
        consecutive units starting at `firstTexUnit`, and fill `out`.
        Return false to skip the draw (no layers yet / allocation failed). */
    virtual bool fusionGlPrepare (juce::OpenGLContext& ctx, int firstTexUnit,
                               FusionFrameData& out) = 0;

    /** GL THREAD. The context is going away — drop every GL object. */
    virtual void fusionGlRelease() = 0;

    /** GL THREAD. Detached windows have no context of their own: the primary host
        renders them into an FBO and hands back the image (top-down). */
    virtual void deliverFusionOffscreenImage (const juce::Image&) {}
};

/** The fragment shader source, kept next to the interface it serves.

    The background gradient MUST match AlterTheme::paintBackground and
    AlterTheme::backgroundAt. If one changes, change all three. */
namespace FusionShaderSource
{
    inline const char* vertex()
    {
        return
            "attribute vec2 position;\n"
            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";
    }

    inline const char* fragment()
    {
        return
            "uniform vec2 iResolution;\n"      // viewport size, framebuffer px
            "uniform vec2 iOffset;\n"          // viewport origin, framebuffer px
            "uniform sampler2D uLayer0;\n"
            "uniform sampler2D uLayer1;\n"
            "uniform sampler2D uLayer2;\n"
            "uniform float uLayerCount;\n"
            "uniform vec2  uLocal0;\n"
            "uniform vec2  uLocalDU;\n"
            "uniform vec2  uLocalDV;\n"
            "uniform vec3  uBgIn;\n"
            "uniform vec3  uBgMid;\n"
            "uniform vec3  uBgOut;\n"
            "uniform float uAspect;\n"
            "uniform float uNoBg;\n"          // 1 while exporting with an alpha channel
            // per layer, packed so the whole stack is twelve vectors rather than
            // fifty scalars: the host writes them once and the shader indexes by
            // the layer's compacted position.
            "uniform vec4 uL0a;\n"             // blend, opacity, rot, scale
            "uniform vec4 uL0b;\n"             // offX, offY, amount, source (see srcHasAlpha)
            "uniform vec4 uL0c;\n"             // bands, angle, edge, symmetry
            "uniform vec4 uL0d;\n"             // mirrorCount, mirrorAngle, spin, zoom
            "uniform vec4 uL1a;\n"
            "uniform vec4 uL1b;\n"
            "uniform vec4 uL1c;\n"
            "uniform vec4 uL1d;\n"
            "uniform vec4 uL2a;\n"
            "uniform vec4 uL2b;\n"
            "uniform vec4 uL2c;\n"
            "uniform vec4 uL2d;\n"
            "uniform float uWarp;\n"
            "uniform float uWarpAmt;\n"
            "uniform float uWarpSwirl;\n"
            "uniform float uWarpSmooth;\n"    // 0 = the raw field, 1 = only the flow
            "uniform float uWarpDenoise;\n"   // 0 = crisp, 1 = strongly blurred / merged
            "uniform float uWarpSrc;\n"
            "uniform float uSymmetry;\n"       // 1 = no fold, 2..11 = wedges
            "uniform float uMirror;\n"         // how many reflection axes, 0 = none
            "uniform float uMirrorAngle;\n"    // radians, where the first axis lies
            "uniform float uSpin;\n"
            "uniform float uZoom;\n"
            "uniform float uVortex;\n"        // bipolar swirl, grows with radius
            "uniform float uDrive;\n"
            "uniform vec3  uGlobLayers;\n"    // per-layer mask: 1 = global chain applies, 0 = read straight
            "uniform float uLiquid;\n"        // 1 = melt the targeted layers together
            "uniform float uLiquidAmt;\n"     // flow travel
            "uniform float uLiquidSmooth;\n"  // feature scale: fine ripples .. broad currents
            "uniform float uLiquidDenoise;\n" // 0 = crisp, 1 = strongly blurred / merged
            "uniform float uTunnel;\n"        // 1 = radially symmetric receding tunnel
            "uniform float uTime;\n"          // seconds, drives the liquid flow + tunnel drift
            "\n"
            "vec3 bgAt(float t)\n"
            "{\n"
            "    float b = clamp(t, 0.0, 1.0);\n"
            "    return (b < 0.55) ? mix(uBgIn, uBgMid, b / 0.55)\n"
            "                      : mix(uBgMid, uBgOut, (b - 0.55) / 0.45);\n"
            "}\n"
            "\n"
            "float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n"
            "\n"
            // ── the folds ────────────────────────────────────────────────────
            //
            // Both work on an aspect-corrected coordinate centred on the origin,
            // and both are used TWICE: once on each layer's own sampling
            // coordinate and once on the coordinate the finished stack is read
            // at. One implementation, so a per-layer fold and a global fold can
            // never mean two different things.
            //
            // Folds a coordinate back into 0..1 by reflecting at each edge, so the
            // radial fold fills the module instead of running off it.
            "float foldRadius(float t)\n"
            "{\n"
            "    return abs(fract(t * 0.5) * 2.0 - 1.0);\n"
            "}\n"
            "\n"
            // MIRROR: `n` reflection axes through the centre, evenly spaced, the
            // first one at `a0`.
            //
            // Reflecting in n evenly spaced lines is the same thing as folding the
            // ANGLE into a wedge of pi/n and reflecting at its edge — that is what
            // repeated reflection converges to, so it is written directly instead
            // of looped. n = 1 gives back exactly the single reflection this stage
            // used to be: one half folded onto the other.
            //
            // The RADIUS is deliberately untouched. That is the whole difference
            // between this and Symmetry, and it is what keeps a mirrored layer
            // life-size rather than pulled into a rosette.
            "vec2 foldMirror(vec2 p, float n, float a0)\n"
            "{\n"
            "    if (n < 0.5) return p;\n"
            "    float seg = 3.14159265359 / n;\n"
            "    float two = seg * 2.0;\n"
            "    float r = length(p);\n"
            // atan(0, 0) is undefined and some drivers hand back a NaN, which would
            // put one dead pixel in the exact centre of every folded layer.
            "    if (r < 0.000001) return p;\n"
            "    float a = atan(p.y, p.x) - a0;\n"
            "    a -= two * floor(a / two);\n"
            "    if (a > seg) a = two - a;\n"
            "    a += a0;\n"
            "    return vec2(r * cos(a), r * sin(a));\n"
            "}\n"
            "\n"
            // SYMMETRY: wedge the ANGLE, fold the RADIUS to fill.
            //
            // The radius is the only thing that can be folded without breaking the
            // symmetry, because it is the one quantity a kaleidoscope does not care
            // about the direction of — every wedge stays congruent and the source
            // is read 1:1 with no magnification. Reflecting at each fold is what
            // makes neighbouring copies meet as mirror images, so there is never a
            // visible join however far it is turned.
            //
            // 1 means no fold at all, and it is the default.
            "vec2 foldSym(vec2 p, float sym, float spin)\n"
            "{\n"
            "    if (sym < 1.5) return p;\n"
            "    float r = length(p);\n"
            "    if (r < 0.000001) return p;\n"
            "    float a = atan(p.y, p.x);\n"
            "    float seg = 6.28318530718 / sym;\n"
            "    float rel = a - spin;\n"
            "    rel -= seg * floor(rel / seg);\n"
            "    if (rel > seg * 0.5) rel = seg - rel;\n"
            "    a = spin + rel;\n"
            "    float rr = foldRadius(r * 2.0) * 0.5;\n"
            "    return vec2(rr * cos(a), rr * sin(a));\n"
            "}\n"
            "\n"
            // VORTEX: turn by an angle that GROWS with the radius, so the centre
            // stands still and the edge is dragged round. Straight lines become
            // spirals and a kaleidoscope stops being a fixed rosette.
            //
            // It runs FIRST, before either fold, and that is the whole reason it
            // works: a swirl applied to an already-wedged picture would twist each
            // wedge away from its neighbours and every seam would open. Swirl the
            // source, then fold it, and the copies still meet exactly.
            //
            // Bipolar, 0 = off.
            "vec2 vortex(vec2 p, float amt)\n"
            "{\n"
            "    if (amt == 0.0) return p;\n"
            "    float a = amt * length(p) * 6.0;\n"
            "    float cs = cos(a), sn = sin(a);\n"
            "    return vec2(p.x * cs - p.y * sn, p.x * sn + p.y * cs);\n"
            "}\n"
            "\n"
            // The whole coordinate chain — zoom, vortex, wedges, turn, mirror — in
            // the one order, applied to whatever coordinate is handed in.
            //
            // MIRROR IS LAST, after the wedges rather than before them. It folds
            // the finished rosette, which is a picture you cannot get the other way
            // round: fold first and the wedges are simply built out of a mirrored
            // source, and with an even wedge count that is barely a change at all.
            // It also means the Axis angle is a real angle in the module, fixed
            // where you put it, instead of being carried round by Spin.
            "vec2 postChain(vec2 uv, float zoom, float mirrorN, float mirrorA,\n"
            "               float sym, float spin, float vtx)\n"
            "{\n"
            "    vec2 p = vec2((uv.x - 0.5) * uAspect, uv.y - 0.5) / max(0.05, zoom);\n"
            "    p = vortex(p, vtx);\n"
            "    if (sym > 1.5)\n"
            "        p = foldSym(p, sym, spin);\n"
            "    else if (spin != 0.0)\n"
            "    {\n"
            // With no wedges, Spin is a plain rotation of the whole picture.
            "        float cs = cos(spin), sn = sin(spin);\n"
            "        p = vec2(p.x * cs - p.y * sn, p.x * sn + p.y * cs);\n"
            "    }\n"
            "    p = foldMirror(p, mirrorN, mirrorA);\n"
            "    return vec2(p.x / uAspect + 0.5, p.y + 0.5);\n"
            "}\n"
            "\n"
            // ── per-layer sampling ───────────────────────────────────────────
            //
            // Quarter turns are derived from — and must stay identical to — the
            // AffineTransform PanelHost::resized applies to an ordinary panel, so
            // 90 degrees means the same thing whether a module sits in the HUD or
            // inside a Fusion. JUCE rotates by
            //     x' = x cos - y sin,  y' = x sin + y cos
            // which with y pointing down is clockwise on screen; inverting that
            // for a quarter turn gives (u,v) -> (v, 1-u).
            "vec2 rotUV(vec2 uv, float q)\n"
            "{\n"
            "    if (q > 2.5) return vec2(1.0 - uv.y, uv.x);\n"
            "    if (q > 1.5) return vec2(1.0 - uv.x, 1.0 - uv.y);\n"
            "    if (q > 0.5) return vec2(uv.y, 1.0 - uv.x);\n"
            "    return uv;\n"
            "}\n"
            "\n"
            // A layer's CONTENT, premultiplied.
            //
            // hasAlpha = 1: the module skipped its background, so the alpha channel
            // is real coverage and nothing has to be guessed.
            //
            // hasAlpha = 0: the module painted the theme gradient under itself (the
            // GL modules still do), so it is subtracted back out and the remainder's
            // brightness becomes the coverage. The gate is deliberately gentle —
            // cutting high looked clean on a spectrum's solid bars and erased every
            // faint tail, glow and particle, which is most of what makes a fusion
            // worth looking at.
            // NOTE the texel is ALREADY premultiplied and must not be multiplied by
            // its own alpha a second time. juce::Image::ARGB stores premultiplied
            // pixels, and the GL modules write premultiplied colour too, so doing it
            // again here squared the coverage and quietly darkened every layer.
            // WHERE a layer's pixels came from, packed into one float because there
            // was no fourth free component in uL{i}b and the two answers are always
            // read one line apart:
            //
            //   low bit  — 1: the alpha channel is real coverage
            //   next bit — 1: the pixels are the module's own framebuffer, which GL
            //                 stores bottom-up, so the sampling v is NOT flipped
            //
            // So 0 and 1 are an uploaded frame, 2 and 3 are a live framebuffer.
            "float srcHasAlpha(float s) { return mod(floor(s + 0.5), 2.0); }\n"
            "bool  srcFromFbo (float s) { return s > 1.5; }\n"
            "\n"
            "vec4 decode(vec4 texel, float v, float hasAlpha)\n"
            "{\n"
            "    if (hasAlpha > 0.5)\n"
            "        return texel;\n"
            "\n"
            "    vec3 k = max(texel.rgb - bgAt(v), vec3(0.0));\n"
            "    float a = smoothstep(0.004, 0.035, luma(k));\n"
            "    return vec4(k * a, a);\n"
            "}\n"
            "\n"
            // Layer transform: this layer's OWN post chain first, then scale and
            // offset about the centre, then the quarter turn. Anything outside the
            // layer is empty, never clamped, so a scaled-down layer sits in the
            // composite instead of smearing its edge pixels across the rest of it.
            //
            // The layer's chain runs BEFORE its placement on purpose: mirror and
            // wedges are about the layer's own picture, so they have to see it
            // where it was drawn, not where it was afterwards moved to. Folding
            // after the offset would have made Offset and Mirror Angle fight over
            // the same centre.
            "vec4 fetch(sampler2D tex, vec2 uv, vec4 a, vec4 b, vec4 c, vec4 d)\n"
            "{\n"
            "    vec2 q = uv;\n"
            // Exact identity when this layer asked for none of it: a fold that does
            // nothing must cost nothing AND must not resample.
            "    if (d.x > 0.5 || c.w > 1.5 || d.z != 0.0 || abs(d.w - 1.0) > 0.0005)\n"
            "        q = postChain(uv, d.w, d.x, d.y, c.w, d.z, 0.0);\n"
            "\n"
            "    vec2 p = (q - 0.5) / max(0.05, a.w) + 0.5 - vec2(b.x, b.y);\n"
            "    p = rotUV(p, a.z);\n"
            "    if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return vec4(0.0);\n"
            // p.y runs DOWN the layer. An uploaded frame has its first row at v = 0
            // and a framebuffer has its first row at the bottom, so exactly one of
            // the two is read the other way up.
            "    float sy = srcFromFbo(b.w) ? p.y : 1.0 - p.y;\n"
            "    vec4 t = texture2D(tex, vec2(p.x, sy));\n"
            "    return decode(t, p.y, srcHasAlpha(b.w)) * a.y;\n"
            "}\n"
            "\n"
            "vec4 readLayer(int i, vec2 uv)\n"
            "{\n"
            "    if (i == 0) return fetch(uLayer0, uv, uL0a, uL0b, uL0c, uL0d);\n"
            "    if (i == 1) return fetch(uLayer1, uv, uL1a, uL1b, uL1c, uL1d);\n"
            "    return fetch(uLayer2, uv, uL2a, uL2b, uL2c, uL2d);\n"
            "}\n"
            "\n"
            // DENOISE. What Warp and Liquid tear into the picture is high-frequency:
            // the displacement field flips from pixel to pixel, so neighbouring output
            // pixels are read from very different places and the result speckles and
            // streaks — the noise in the reference frame. A small blur on the READ is
            // the direct cure: it averages that speckle away and, in doing so, lets
            // nearby colours and curves run into one another, which is exactly the
            // "melt it simpler" the effect wants. Centre is double-weighted so shapes
            // keep their place; the eight-tap ring does the merging. `rad` is in uv
            // units and scales with the Denoise knob. At 0 not one extra sample is
            // taken and the read is bit-for-bit what it was before this existed.
            "vec4 readLayerSoft(int i, vec2 uv, float rad)\n"
            "{\n"
            "    if (rad < 0.0005) return readLayer(i, uv);\n"
            "    vec4 s = readLayer(i, uv) * 2.0;\n"
            "    s += readLayer(i, uv + vec2( rad, 0.0));\n"
            "    s += readLayer(i, uv + vec2(-rad, 0.0));\n"
            "    s += readLayer(i, uv + vec2(0.0,  rad));\n"
            "    s += readLayer(i, uv + vec2(0.0, -rad));\n"
            "    float rd = rad * 0.70710678;\n"
            "    s += readLayer(i, uv + vec2( rd,  rd));\n"
            "    s += readLayer(i, uv + vec2( rd, -rd));\n"
            "    s += readLayer(i, uv + vec2(-rd,  rd));\n"
            "    s += readLayer(i, uv + vec2(-rd, -rd));\n"
            "    return s * 0.1;\n"                              // 2 + 8 taps
            "}\n"
            "\n"
            // ── blend modes ──────────────────────────────────────────────────
            "vec4 bScreen(vec4 a, vec4 b)\n"
            "{\n"
            "    return vec4(a.rgb + b.rgb - a.rgb * b.rgb,\n"
            "                a.a   + b.a   - a.a   * b.a);\n"
            "}\n"
            "\n"
            // MERGE. Colours are pulled toward each other in proportion to how
            // bright each is locally, so the brighter side leads and the seam runs
            // along the actual shapes rather than across a straight line. The
            // overlap is added back as light, which is what reads as one glowing
            // substance instead of two pictures averaged.
            //
            // Coverage is UNIONED, never traded: wherever only one side has
            // anything, that side is the answer whole. BIAS gets the full -1..+1 of
            // travel so its ends actually mean "all of the one below" and "all of
            // this one".
            "vec4 bMerge(vec4 a, vec4 b, float bias)\n"
            "{\n"
            "    float wa = a.a, wb = b.a;\n"
            "    float uA = wa + wb - wa * wb;\n"
            "    if (uA <= 0.0001) return vec4(0.0);\n"
            "\n"
            "    vec3 sa = a.rgb / max(0.0001, wa);\n"
            "    vec3 sb = b.rgb / max(0.0001, wb);\n"
            "\n"
            "    float la = luma(sa) * wa;\n"
            "    float lb = luma(sb) * wb;\n"
            "    float t  = (lb + 0.0005) / (la + lb + 0.001);\n"
            "    t = clamp(t + (bias - 0.5) * 2.0, 0.0, 1.0);\n"
            "\n"
            "    vec3 s = mix(sa, sb, t) + min(sa, sb) * (0.6 * min(wa, wb));\n"
            "    return vec4(min(s, vec3(1.0)) * uA, uA);\n"
            "}\n"
            "\n"
            // WEAVE, a GROUP effect over every layer set to it plus the base they
            // sit on. Each participant LEADS in its own bands and stays faintly
            // present in everybody else's, so nobody is hidden outright — they
            // trade the foreground. With two participants the bands alternate,
            // with three they cycle, and with W they repeat every W bands.
            //
            // This is the one blend that could not be answered a pair at a time.
            // Done pairwise, layer C met the finished A+B rather than meeting A and
            // B as peers, so its bands landed on top of a decision that had already
            // been made and it read as a faint wash instead of a third strand.
            //
            // Where a point falls along the bands, in band units.
            "float weaveT(vec2 uv, vec4 c)\n"
            "{\n"
            "    float axis = (uv.x - 0.5) * cos(c.y) + (uv.y - 0.5) * sin(c.y);\n"
            "    return axis * c.x + uSpin * 0.15;\n"
            "}\n"
            "\n"
            // What this participant is worth here: 1 in the middle of a band it
            // owns, 0.12 in everybody else's. AMOUNT widens or narrows the band a
            // participant claims, EDGE sets how hard the crossing is.
            "float weaveGain(float t, float slot, float w, vec4 c, float amount)\n"
            "{\n"
            "    float u = t - slot;\n"
            "    u -= w * floor(u / w);\n"                    // 0..w; this slot owns [0,1)
            "    float dc = abs(u - 0.5);\n"
            "    dc = min(dc, w - dc);\n"                     // wrap: bands repeat
            "    float half_ = 0.5 * clamp(amount * 2.0, 0.1, 1.9);\n"
            "    float soft  = 0.03 + (1.0 - c.z) * 0.5;\n"
            "    return mix(0.12, 1.0,\n"
            "               1.0 - smoothstep(half_ - soft, half_ + soft, dc));\n"
            "}\n"
            "\n"
            // `acc` is everything below, `lay` is this layer. Weave never arrives
            // here — it is not a question about a pair.
            "vec4 blendWith(vec4 acc, vec4 lay, vec4 a, vec4 b)\n"
            "{\n"
            "    if (a.x > 0.5) return bMerge(acc, lay, b.z);\n"
            "    return bScreen(acc, lay);\n"
            "}\n"
            "\n"
            // The brightness gradient Warp steers by, measured across a radius of
            // `e`. Pulled out as a function because SMOOTH needs it twice, and the
            // two calls have to measure the same thing at two scales.
            "vec2 warpGrad(int L, vec2 uv, float e)\n"
            "{\n"
            "    float gx = luma(readLayer(L, uv + vec2(e, 0.0)).rgb)\n"
            "             - luma(readLayer(L, uv - vec2(e, 0.0)).rgb);\n"
            "    float gy = luma(readLayer(L, uv + vec2(0.0, e)).rgb)\n"
            "             - luma(readLayer(L, uv - vec2(0.0, e)).rgb);\n"
            "    return vec2(gx, gy);\n"
            "}\n"
            "\n"
            // TUNNEL. Remap the plane into a receding tunnel: the angle wraps around
            // the walls and 1/radius becomes DEPTH, so the centre is infinitely far
            // and the picture recedes to a vanishing point. Radially symmetric by
            // construction — it is depth, not a lopsided space warp — and it drifts
            // inward on its own so the walls appear to fly past.
            //
            // The layer content tiles down the tunnel; aspect-correcting the radius
            // keeps the mouth circular rather than an ellipse on a wide panel.
            //
            // MIRROR-FOLD, not fract. fract wraps each coordinate hard from 1 back to
            // 0, and because a layer's picture is not periodic its right edge does not
            // match its left — so every wrap drew a visible SEAM and the tunnel looked
            // sliced into rings and wedges. Folding instead (triangle wave) makes
            // neighbouring tiles meet as MIRROR IMAGES of one another, which is
            // continuous across the join: the slices disappear and the walls read as
            // one unbroken receding surface. This is the same reflect-to-fill trick
            // foldRadius already uses for the kaleidoscope, applied here to depth and
            // to the angular wrap.
            "vec2 tunnelMap(vec2 uv)\n"
            "{\n"
            "    vec2 p = vec2((uv.x - 0.5) * uAspect, uv.y - 0.5);\n"
            "    float r = length(p);\n"
            "    float a = atan(p.y, p.x);\n"
            "    float depth = 0.16 / max(r, 0.0025) + uTime * 0.15;\n"
            "    float ang   = a * 0.15915494 + 0.5;\n"          // a / 2pi + 0.5, in 0..1
            // angle: fold the single 1->0 wrap so both edges (straight left) share the
            // same content; depth: triangle-wave the receding tiles so each ring meets
            // its neighbour mirrored instead of cut.
            "    float av = 1.0 - abs(2.0 * fract(ang) - 1.0);\n"
            "    float dv = foldRadius(depth);\n"
            "    return vec2(av, dv);\n"
            "}\n"
            "\n"
            // LIQUID. A shared flowing displacement, the SAME for every targeted
            // layer, so instead of one layer being pushed off another (that is
            // Warp) the whole stack drifts together and the shapes run into one
            // another like a single fluid.
            //
            // Two parts: a slow sinusoidal domain warp that animates with uTime for
            // the flow, and a swirl AROUND the base layer's bright structures so the
            // motion follows the picture rather than sliding across it — which is
            // what reads as melting rather than wobbling. SMOOTH sets the scale: low
            // is fine ripples, high is broad lazy currents.
            "vec2 liquidField(vec2 uv)\n"
            "{\n"
            "    float sc = mix(10.0, 2.5, clamp(uLiquidSmooth, 0.0, 1.0));\n"
            "    float t  = uTime * 0.6;\n"
            "    vec2  q  = uv * sc;\n"
            "    vec2 flow = vec2(sin(q.y + t)       + 0.5 * sin(q.y * 2.0 - t * 1.3),\n"
            "                     cos(q.x - t * 0.9) + 0.5 * cos(q.x * 2.0 + t * 1.1));\n"
            "    vec2 g = warpGrad(0, uv, 0.03 + uLiquidSmooth * 0.10);\n"
            "    flow += vec2(-g.y, g.x) * 5.0;\n"               // swirl around bright shapes
            "    return flow * (uLiquidAmt * 0.05);\n"
            "}\n"
            "\n"
            "void main()\n"
            "{\n"
            "    vec2 fc = gl_FragCoord.xy - iOffset;\n"
            "    float u = fc.x / max(iResolution.x, 1.0);\n"
            "    float v = 1.0 - fc.y / max(iResolution.y, 1.0);\n"
            "    vec2 uv = uLocal0 + u * uLocalDU + v * uLocalDV;\n"
            // The module's own vertical position, kept BEFORE anything folds uv:
            // the background belongs to the module, not to the fold.
            "    float bgV = uv.y;\n"
            "\n"
            // The GLOBAL post chain, on the coordinate the finished stack is read
            // at: zoom, then mirror axes, then wedges, then the turn. Exactly the
            // stages each layer already had to itself, asked once more of the
            // result — folding the result and folding the layers are two different
            // pictures, and the module can now say either.
            //
            // Mirror runs LAST, after the wedges: it folds the finished rosette,
            // which is a picture you cannot get the other way round. Done first,
            // the wedges are merely built out of a mirrored source, and with an
            // even wedge count that is barely a change at all.
            // Kept on a SEPARATE coordinate (uvG) instead of folding uv in place, so
            // the global chain can be applied per layer: a targeted layer is read at
            // uvG, an untargeted one stays on the raw uv. TUNNEL rides on the end of
            // the same coordinate — it is a whole-picture remap, so it belongs with
            // the global fold rather than inside any one layer.
            "    vec2 uvG = postChain(uv, uZoom, uMirror, uMirrorAngle, uSymmetry, uSpin, uVortex);\n"
            "    if (uTunnel > 0.5) uvG = tunnelMap(uvG);\n"
            "\n"
            "    int n = int(uLayerCount + 0.5);\n"
            "\n"
            // WARP, the last coordinate stage. The displacement comes from one
            // chosen layer's brightness gradient, taken over a WIDE radius — a
            // one-pixel derivative only wobbles edges, a broad one reads as flow.
            // SWIRL sets how much of it pushes around bright structures rather than
            // away from them.
            //
            // The source layer moves the OPPOSITE way to everything else. Pushing
            // them together would only translate the picture; pushing them apart is
            // what makes them carve into each other.
            "    vec2 d = vec2(0.0);\n"
            "    if (uWarp > 0.5 && n > 0)\n"
            "    {\n"
            "        int  L = int(min(uWarpSrc, float(n - 1)) + 0.5);\n"
            "        vec2 g = warpGrad(L, uvG, 0.035);\n"
            "\n"
            // SMOOTH. What made a strong warp look like a mess was the gradient
            // following every speck of detail in the source: a spectrum's bars or a
            // particle field flip its sign from pixel to pixel, neighbouring output
            // pixels are then read from wildly different places, and the picture
            // tears along the noise instead of flowing with the shapes.
            //
            // Two things fix it and the one number dials both:
            //
            //   a WIDER estimate, mixed in — a broad derivative cannot see the
            //   speckle and answers with the shape instead;
            //
            //   a soft knee on the magnitude — it is the sudden long jumps that
            //   read as torn, and compressing the big ones while leaving the small
            //   ones alone takes the tearing out without flattening the motion.
            //
            // At 0 neither happens and not one extra sample is taken, so the effect
            // is exactly what it was before this existed.
            "        if (uWarpSmooth > 0.002)\n"
            "        {\n"
            "            g = mix(g, warpGrad(L, uvG, 0.035 + uWarpSmooth * 0.13),\n"
            "                    uWarpSmooth);\n"
            "            g /= (1.0 + uWarpSmooth * 9.0 * length(g));\n"
            "        }\n"
            "\n"
            "        d = (g * (1.0 - uWarpSwirl) + vec2(-g.y, g.x) * uWarpSwirl)\n"
            "            * (uWarpAmt * 1.1 * (1.0 + uDrive));\n"
            "    }\n"
            "\n"
            // LIQUID: the shared flow, computed once on the folded coordinate.
            "    vec2 lq = (uLiquid > 0.5 && n > 0) ? liquidField(uvG) : vec2(0.0);\n"
            "\n"
            // Per-layer BASE coordinate. A layer the global chain targets
            // (uGlobLayers) is read at the folded coordinate plus the shared
            // displacements; one it does not target is read straight off the raw
            // module coordinate, untouched by fold, tunnel, liquid or warp. Warp's
            // source layer still travels the opposite way to the rest.
            "    vec2 gW0 = (uWarpSrc < 0.5) ? -d * 0.6 : d;\n"
            "    vec2 gW1 = (uWarpSrc > 0.5 && uWarpSrc < 1.5) ? -d * 0.6 : d;\n"
            "    vec2 gW2 = (uWarpSrc > 1.5) ? -d * 0.6 : d;\n"
            "    vec2 b0 = (uGlobLayers.x > 0.5) ? (uvG + lq + gW0) : uv;\n"
            "    vec2 b1 = (uGlobLayers.y > 0.5) ? (uvG + lq + gW1) : uv;\n"
            "    vec2 b2 = (uGlobLayers.z > 0.5) ? (uvG + lq + gW2) : uv;\n"
            "\n"
            // DENOISE radius. Warp and Liquid each carry their own knob; whichever is
            // ON and asks for more wins (max, not sum — two blurs stacked would just
            // wash the picture out). It applies ONLY to the layers the global chain
            // targets, because those are the ones being displaced and therefore torn;
            // a layer read straight off the raw coordinate is already crisp and is
            // left alone.
            "    float dn = 0.0;\n"
            "    if (uWarp   > 0.5) dn = max(dn, uWarpDenoise);\n"
            "    if (uLiquid > 0.5) dn = max(dn, uLiquidDenoise);\n"
            "    float rad = dn * 0.012;\n"
            "    float rad0 = (uGlobLayers.x > 0.5) ? rad : 0.0;\n"
            "    float rad1 = (uGlobLayers.y > 0.5) ? rad : 0.0;\n"
            "    float rad2 = (uGlobLayers.z > 0.5) ? rad : 0.0;\n"
            "\n"
            // ── the stack, bottom-up ─────────────────────────────────────────
            // Layer 0 is the base: it is what everything else is composited ONTO,
            // so its own blend mode is not consulted. Every layer above brings its
            // own, and the accumulator is the only thing they meet.
            //
            // WEAVE is the exception, because it is not a question about a pair.
            // Every layer set to Weave joins a group with the base, the group is
            // counted FIRST, and each member is then dimmed or led according to
            // which band it is standing in. The band settings come from the lowest
            // weaving layer: one weave has one set of bands, or the strands would
            // be cut to different widths and stop interlocking.
            "    float wB = (n > 1 && uL1a.x > 1.5) ? 1.0 : 0.0;\n"
            "    float wC = (n > 2 && uL2a.x > 1.5) ? 1.0 : 0.0;\n"
            "    float wN = 1.0 + wB + wC;\n"                       // participants, base included
            "    bool  weaving = (wB + wC) > 0.5;\n"
            "    vec4  wc   = (wB > 0.5) ? uL1c : uL2c;\n"
            "    float wamt = (wB > 0.5) ? uL1b.z : uL2b.z;\n"
            "    float wt   = weaving ? weaveT(uvG, wc) : 0.0;\n"
            "\n"
            "    vec4 acc = readLayerSoft(0, b0, rad0);\n"
            // Premultiplied, so scaling the whole vector dims colour and coverage
            // together — which is what "stay faintly present" has to mean.
            "    if (weaving) acc *= weaveGain(wt, 0.0, wN, wc, wamt);\n"
            "\n"
            "    if (n > 1)\n"
            "    {\n"
            "        vec4 lay = readLayerSoft(1, b1, rad1);\n"
            "        if (wB > 0.5) acc = bScreen(acc, lay * weaveGain(wt, 1.0, wN, wc, wamt));\n"
            "        else          acc = blendWith(acc, lay, uL1a, uL1b);\n"
            "    }\n"
            "    if (n > 2)\n"
            "    {\n"
            "        vec4 lay = readLayerSoft(2, b2, rad2);\n"
            // Slot 2 only when B is weaving too; otherwise C is the second strand.
            "        if (wC > 0.5) acc = bScreen(acc, lay * weaveGain(wt, 1.0 + wB, wN, wc, wamt));\n"
            "        else          acc = blendWith(acc, lay, uL2a, uL2b);\n"
            "    }\n"
            "\n"
            // Over the module's own background: the same gradient every other module
            // paints, so Fusion sits in the HUD like the rest and follows the theme.
            //
            // Unless the fusion is being EXPORTED WITH ALPHA, in which case there is
            // no background to sit on and `acc` is already exactly what we want: it
            // is premultiplied, so its rgb needs no further scaling and its alpha is
            // the fusion's real coverage. Compositing the gradient in here would be
            // the one irreversible step — once the background is mixed into a
            // semi-transparent pixel, nothing downstream can take it back out.
            "    float cov = clamp(acc.a, 0.0, 1.0);\n"
            "    gl_FragColor = (uNoBg > 0.5) ? vec4(acc.rgb, cov)\n"
            "                                 : vec4(acc.rgb + bgAt(bgV) * (1.0 - cov), 1.0);\n"
            "}\n";
    }
}
