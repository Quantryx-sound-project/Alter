/*
  ==============================================================================

    SgShader.h
    Shared types so AlterGLHost (the per-window OpenGL host) and SpectrogramMeter
    can talk without an include cycle — the same arrangement SynShader.h provides
    for the fractal.

    WHY THE SPECTROGRAM MOVES TO THE GPU
    ------------------------------------
    The CPU path rasterised the whole visible image every frame and handed the
    finished picture to the GL context, which uploaded it as a texture on every
    swap. That upload is proportional to the MODULE AREA, and it is the reason a
    narrow HUD was always fine while fullscreen never was:

        narrow  HUD  ~0.2 Mpx  ->  0.8 MB per frame  ->   48 MB/s at 60 Hz
        fullscreen   ~1.2 Mpx  ->  4.8 MB per frame  ->  288 MB/s at 60 Hz

    The render pixel budget existed only to bound that, which is exactly why
    short windows looked soft: the frame was rasterised at 1.2 Mpx and stretched.
    Sharpness and smoothness were traded against each other because both were
    paid for in the same currency.

    Here the currency changes. The history lives in a GPU texture and only the
    NEW columns are uploaded — about 4 columns x 2048 rows = 8 KB per frame,
    some 600x less, and independent of how large the module is. The visible image
    is then produced by a fragment shader at native resolution, so:

      * no render pixel budget, therefore no upscale, therefore no static blur;
      * scrolling is a float texture coordinate, so it is sub-pixel by nature
        rather than snapped to whole pixels;
      * per-frame CPU is the analysis only — which is what was starving Ableton.

    The DSP is untouched. FFT / Constant-Q / reassignment, the per-row EMA, the
    row-fill taper and the log-frequency row table all stay on the worker thread
    exactly as they were; this layer only receives the finished column of row
    magnitudes and is responsible for storing, scrolling and colouring it.

    TEXTURE LAYOUT (transposed on purpose)
    --------------------------------------
    The history texture is kRows wide (FREQUENCY on x) and bufferCols tall (TIME
    on y), i.e. one analysis column occupies one texture ROW. A column is then a
    single contiguous 2048-byte run in client memory, so uploading a burst of new
    columns is ONE glTexSubImage2D of a contiguous block instead of N strided
    1-pixel-wide uploads. The shader simply swaps the axes when sampling.

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

/** One frame's worth of placement for a spectrogram instance. Plain data,
    written on the message thread and read on the GL thread.

    The rectangle is expressed in the HOST component's coordinates (that is where
    the host sets its viewport). Because a panel can be ROTATED by 90/180/270
    degrees via an AffineTransform, the rect alone is not enough to know which way
    the module's own axes run inside it — so the mapping from the rect back into
    the module's local space is carried explicitly as an affine basis:

        localUV = local0 + u * localDU + v * localDV

    with u across the rect (0 = left, 1 = right) and v down it (0 = top, 1 =
    bottom), and localUV normalised to the module's own local bounds. For an
    unrotated panel this is simply local0 = (0,0), localDU = (1,0), localDV =
    (0,1). Deriving it from the live transform means the GPU picture and the 2D
    overlay the component paints on top always agree, whatever the rotation. */
struct SgShaderState
{
    bool active    = false;           // false -> host skips this instance
    bool offscreen = false;           // true  -> render to an FBO and hand back an image
                                      //          (detached window has no GL context)
    int  x = 0, y = 0, w = 0, h = 0;  // rect in the HOST component's coordinates

    juce::Point<float> local0  { 0.0f, 0.0f };
    juce::Point<float> localDU { 1.0f, 0.0f };
    juce::Point<float> localDV { 0.0f, 1.0f };

    /** True while this module is lent to a Fusion as a layer: coverage then goes
        into the alpha channel (from the ENERGY, which is what a spectrogram
        actually knows) instead of the palette being drawn over the background. */
    bool noBackground = false;
};

/** Everything the host's fragment shader needs about the source's history for
    ONE frame. Filled in by the source on the GL thread (in sgGlPrepare, right
    after it has uploaded whatever columns the worker produced); the host owns
    the program and pushes these into the uniforms. */
struct SgFrameData
{
    float headCol = 0.0f;    // fractional position of the newest column, wrapped to [0, bufCols)
    float visCols = 1.0f;    // columns spanning the visible width
    float bufCols = 1.0f;    // history texture height (columns of capacity)
    float filled  = 0.0f;    // valid columns behind the head (older area is background)
    float rowSpan = 1.0f;    // one screen pixel, in normalised local-y units
    float yTaps   = 1.0f;    // vertical box-filter taps (1..8)
};

/** Implemented by SpectrogramMeter; consumed by AlterGLHost. */
struct ISgShaderSource
{
    virtual ~ISgShaderSource() = default;

    /** MESSAGE or GL thread. Cheap snapshot of where this instance lives. */
    virtual SgShaderState getSgState() const = 0;

    /** GL THREAD. Create/refresh this source's textures, upload whatever columns
        the worker has produced since the last frame, bind them to the units the
        host has reserved, and fill `out`.
        Return false to skip the draw (nothing to show yet / allocation failed).
        `localYPixels` is how many framebuffer pixels the module's FULL local-y
        (frequency) extent covers on screen — the rect's height normally, its
        width under a 90/270-degree rotation. The source needs it to size the
        vertical box filter. */
    virtual bool sgGlPrepare (juce::OpenGLContext& ctx,
                              int texUnitHistory, int texUnitLut,
                              float localYPixels, SgFrameData& out) = 0;

    /** GL THREAD. The context is going away — drop every GL object. */
    virtual void sgGlRelease() = 0;

    /** GL THREAD. Detached windows have no context of their own: the primary
        host renders them into an FBO and hands back the image (top-down). */
    virtual void deliverSgOffscreenImage (const juce::Image&) {}
};
