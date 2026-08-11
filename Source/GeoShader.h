/*
  ==============================================================================

    GeoShader.h
    Shared types so AlterGLHost (the per-window OpenGL host) and GeometryVisual
    can talk without an include cycle — the same arrangement SynShader.h provides
    for the fractal and SgShader.h for the spectrogram.

    WHY GEOMETRY MOVES TO THE GPU
    -----------------------------
    The CPU path stroked every shape with juce::Graphics into a software image,
    and — with Bloom on — did FOUR more full-resolution passes on top of it:

        alloc + clear sceneImg      (w*h*4 memset)
        stroke every shape          (path tessellation + scanline AA, per frame)
        downscale full-res -> w/6   (CPU bilinear resample of the whole frame)
        upscale w/6 -> full-res     (CPU bilinear resample of the whole frame)
        composite sceneImg on top   (full-res alpha blend)

    Every one of those scales with MODULE AREA, which is why enlarging the module
    was what hurt most, and the render pixel budget only bounded the damage by
    throwing resolution away. On top of that the budget was keyed off the Bloom
    value in three discrete steps, so AUTOMATING Bloom across a step boundary
    reallocated all three image buffers every frame and made the resolution visibly
    pop.

    Here the currency changes. The simulation still runs on the worker thread, but
    it now emits a flat array of GeoInstance records instead of rasterising. The
    array is uploaded as a small floating-point texture (a few hundred KB at most,
    independent of module size) and the GPU expands each record into a stroked
    outline in the vertex shader. Consequences:

      * per-frame CPU cost is the SIMULATION only — no rasterisation at all;
      * no render pixel budget, therefore no upscale, therefore no soft frame;
      * the glow is a multi-scale blur of the whole scene, so halos from
        neighbouring shapes SUM instead of alpha-compositing — the "melting"
        fusion the CPU path could never produce with per-path strokes;
      * cost is independent of how large the module is dragged.

    OUTLINE GEOMETRY (why there is no vertex buffer)
    ------------------------------------------------
    A stroked regular n-gon is generated entirely from gl_VertexID: no attribute
    buffers, no glVertexAttribDivisor (which is core only in GL 3.3 — the context
    here asks for 3.2), no per-frame vertex upload. Each instance covers
    `segments * 6` vertices; the shader derives the instance index, the segment and
    the quad corner by integer division, and reads that instance's record with
    texelFetch.

    The offset outline is exact rather than approximate. For a regular n-gon of
    circumradius R, growing R by w / cos(pi/n) moves every EDGE outward by exactly
    w (the apothem is R*cos(pi/n), so it grows by exactly w). So the inner and
    outer edges of the stroke are two concentric n-gons and the signed distance
    across the stroke is available in the fragment shader for free — which is what
    both the anti-aliasing and the glow falloff are built on. Verified to 1e-14 px.

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <vector>

/** Width of the instance texture, in records. Anything wider risks bumping into
    GL_MAX_TEXTURE_SIZE on a minimal GL 3.2 implementation, so instances wrap onto
    further rows instead: record `i` lives at column i % kGeoInstTexW, in the three
    texture rows starting at 3 * (i / kGeoInstTexW). */
static constexpr int kGeoInstTexW = 1024;

/** Hard ceiling on shapes per module per frame. 8192 instances is 96 KB of
    instance data — nothing — and at that point the shape count stopped being the
    thing that limits the picture. */
static constexpr int kGeoMaxInstances = 8192;

/** ONE stroked shape for ONE frame. Plain data, produced on the worker thread and
    uploaded verbatim to the GPU. Coordinates are in the module's own LOGICAL
    pixels (the w/h renderHeadless was called with); the host converts to clip
    space, so the display scale factor never enters this struct.

    The first twelve floats map onto the three RGBA32F texels the vertex shader
    reads, in this order. `sortKey` is deliberately NOT one of them: it exists only
    so the worker can order the array back-to-front, and is left behind on the CPU. */
struct GeoInstance
{
    // texel 0
    float cx = 0.0f, cy = 0.0f;   // centre, module-local logical px
    float radius = 0.0f;          // circumradius of the outline, logical px
    float rot = 0.0f;             // rotation about the centre, radians

    // texel 1
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;   // straight (NOT premultiplied) colour

    // texel 2
    float halfW    = 1.0f;        // half the core stroke width, logical px
    // glowW / glowGain are CPU-FALLBACK ONLY — the GPU shader ignores them.
    // A per-shape halo has a defined outer radius, and a falloff that stops at a
    // fixed distance from the outline reads as a SECOND CONTOUR rather than as
    // light: the shape looks like it was drawn again a few pixels out. On the GPU
    // every bit of glow comes from the blur pyramid instead, whose falloff never
    // terminates anywhere. The software rasteriser cannot afford a pyramid, so it
    // still strokes one wide faint outline and lives with the compromise.
    float glowW    = 0.0f;        // halo reach outside the core stroke, logical px
    float sides    = 0.0f;        // 3 = triangle, 4 = square, <3 = circle
    float glowGain = 0.0f;        // halo brightness (0 = none)

    // CPU-side only — never uploaded.
    float sortKey = 0.0f;         // depth progress 0..1; the array is sorted on this
                                  // so a single draw call keeps the painter's order
};

/** Where a geometry instance lives, and how it is to be lit. Written on the
    message thread, read on the GL thread; scalar races are benign (at worst one
    slightly-stale frame). The rectangle is in the HOST component's coordinates,
    because that is the space the host sets its viewport in. */
struct GeoShaderState
{
    bool active    = false;           // false -> host skips this instance
    bool offscreen = false;           // true  -> render to an FBO and hand back an image
                                      //          (a detached window has no GL context)
    int  x = 0, y = 0, w = 0, h = 0;  // rect in the HOST component's coordinates

    /** Wide-bloom amount 0..1 (the knob's positive side). 0 disables the whole
        blur pyramid — the scene is then composited straight over the background
        and the per-shape analytic glow is all the light there is. */
    float bloom = 0.0f;

    /** Overall light output 0..2, 1 = neutral. Scales the bloom veil's gain, the
        same way the CPU path scaled its blur gain. */
    float brightness = 1.0f;

    /** True while this module is lent to a Fusion as a layer: the veil and the
        strokes are composited onto NOTHING rather than onto the theme background,
        so their own coverage survives into the alpha channel. */
    bool noBackground = false;
};

/** What the host needs to know about the instance array for ONE frame. Filled in
    by the source inside geoGlPrepare, right after it has uploaded the array. */
struct GeoFrameData
{
    int instanceCount = 0;    // records in the instance texture, starting at 0
    int segments      = 48;   // ring segments per shape; MUST be a multiple of 12
                              // so that triangle and square corners are sampled exactly
    int texUnit       = 0;    // texture unit the instance texture is bound to
};

/** Implemented by GeometryVisual; consumed by AlterGLHost. */
struct IGeoShaderSource
{
    virtual ~IGeoShaderSource() = default;

    /** MESSAGE or GL thread. Cheap snapshot of where this instance lives. */
    virtual GeoShaderState getGeoState() const = 0;

    /** GL THREAD. Create/refresh the instance texture, upload whatever the worker
        produced since the last frame, bind it to `texUnitInstances`, and fill
        `out`. Return false to skip the draw (nothing to show / allocation failed).
        Returning true with instanceCount == 0 is legal and means "draw the
        background only" — which is how the module goes dark in silence. */
    virtual bool geoGlPrepare (juce::OpenGLContext& ctx,
                               int texUnitInstances,
                               GeoFrameData& out) = 0;

    /** GL THREAD. The context is going away — drop every GL object. */
    virtual void geoGlRelease() = 0;

    /** GL THREAD. Detached windows have no context of their own: the primary host
        renders them into an FBO and hands back the image (top-down). */
    virtual void deliverGeoOffscreenImage (const juce::Image&) {}
};
