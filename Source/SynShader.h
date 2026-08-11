/*
  ==============================================================================

    SynShader.h
    Shared types so AlterGLHost (the per-window OpenGL host) and VisualSynesthesia
    (the fractal shader module) can talk without an include cycle.

    The HUD runs on ONE OpenGL context per window. That context owns a single
    fractal shader; every Synesthesia instance under the window is just a "shader
    source" that hands the host its current uniform values and its rectangle.

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>

// One frame's worth of state for a single fractal instance. Plain data, copied
// on the message thread and read on the GL thread (scalar races are benign).
struct SynShaderState
{
    bool  active = false;                 // false -> host skips this instance
    bool  offscreen = false;              // true -> render into an FBO and hand back an image
                                          //         (detached window has no GL context of its own)
    int   x = 0, y = 0, w = 0, h = 0;     // inline: rect in the HOST component's coords;
                                          // offscreen: 0,0 + the instance's own pixel size

    float pitch      = 0.0f;
    float octave     = 2.0f;
    float rms        = 0.0f;
    float time       = 0.0f;
    float zoom       = 1.0f;
    float rotation   = 0.0f;
    float saturation = 1.0f;
    float brightness = 1.0f;   // 0..2, 1 = neutral (overall light output)
    float bloom      = 0.0f;
    int   symmetry   = 1;
    float shakeX     = 0.0f;   // audio-driven jitter (uv units) from React
    float shakeY     = 0.0f;
    bool  manual     = false;  // true -> use baseHue, false -> pitch-derived hue
    float baseHue    = 0.6f;   // manual colour hue 0..1
    // The rest of the picked colour. Manual mode used to send the HUE ALONE and let
    // the shader rebuild the core at (1, 1) — but hue is undefined on an achromatic
    // colour (JUCE reports 0 = red for white, black and every grey), so picking white
    // drew red and picking a deep navy drew a bright primary blue. These two carry
    // the axes the user actually moved in the picker. Both are 1 in tone mode, where
    // the note supplies the whole colour, and 1 for a fully-saturated pick — so the
    // Saturation and Brightness sliders behave exactly as they always have.
    float baseSat    = 1.0f;   // manual colour saturation 0..1
    float baseVal    = 1.0f;   // manual colour brightness 0..1
    float variation  = 0.0f;   // 'Change' 0..1: morphs the fractal
    float transmute  = 0.0f;   // 'Transmute' 0..1: a different, symmetric morph
    bool  mirror     = false;  // Mirror fold: reflective symmetry (like Symmetry, but mirrored)
    float tunnel     = 0.0f;   // 'Tunnel' 0..1: symmetric fly-through tunnel morph (0 = flat)
    float vortex     = 0.0f;   // 'Vortex' 0..1: swirl/spiral twist of the tunnel walls
    float clear      = 0.0f;   // 'Clear' 0..1: down-shifts the octave/layer complexity
    float denoise    = 0.0f;   // 'Denoise' 0..1: joins dashed curves + removes the high noise layers
    float beatPulse  = 0.0f;   // BPM mode: 0..1 pulse of light on each beat (0 when not synced)

    /** True while this module is lent to a Fusion as a layer: the shader then
        hands out the fractal ALONE, with coverage in the alpha channel, instead of
        compositing it over the theme background. Only ever set together with
        `offscreen`, because a layer always renders through the FBO. */
    bool noBackground = false;
};

// Implemented by VisualSynesthesia; consumed by AlterGLHost.
struct ISynShaderSource
{
    virtual ~ISynShaderSource() = default;
    virtual SynShaderState getSynState() const = 0;

    // Called on the GL thread for offscreen sources: the host hands back the
    // rendered fractal as a ready-to-blit image (top-down).
    virtual void deliverOffscreenImage (const juce::Image&) {}
};
