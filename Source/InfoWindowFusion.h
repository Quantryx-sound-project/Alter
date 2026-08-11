/*
  ==============================================================================

    InfoWindowFusion.h
    Educational guide for the Fusion module.

  ==============================================================================
*/

#pragma once
#include "InfoWindowAudioMeters.h"   // ModuleInfoWindow + TextInfoWindow

class FusionInfoWindow : public TextInfoWindow
{
public:
    FusionInfoWindow() : TextInfoWindow ("Fusion - Educational Guide",
    {
        { "WHAT IS FUSION?",
          "A COMPOSITOR. It draws nothing of its own — it owns a stack of layers "
          "and a chain of post effects, and that is the whole of it. You pick up to "
          "three of the modules you already have, and Fusion combines their frames "
          "into one picture.\n\n"
          "A layer is not a copy. It IS the module you picked: it keeps its own "
          "settings, it keeps rendering with its own engine, it simply stops taking "
          "space in the HUD and lends its frames here instead. A module costs "
          "exactly the same whether it sits in the HUD or inside a Fusion." },

        { "THE STACK - bottom-up, like every compositor",
          "Layers accumulate from the bottom. A is the BASE — it is what everything "
          "else is composited onto, which is why it has no blend mode of its own: "
          "there is nothing underneath it. B then meets A, and C meets the result.\n\n"
          "Every layer above the base carries its OWN blend mode. That is what makes "
          "three layers behave like three layers: B and C combine with each other "
          "just as readily as either combines with A, and no slot is secretly more "
          "important than the others.\n\n"
          "Order matters. Moving a module from slot B to slot C changes what it "
          "meets, and therefore changes the picture." },

        { "BLEND - how a layer meets the one below",
          "SCREEN is the default and means 'just stack them': light adds up, both "
          "layers stay visible, neither punches a hole in the other. Nothing is "
          "claimed about how they relate until you say so.\n\n"
          "MERGE is the two stopping being two. Colours are pulled toward each other "
          "in proportion to how bright each is locally, so the brighter side leads "
          "and the seam runs along the actual shapes rather than across a straight "
          "line. Where both are bright at once the overlap comes back as light — "
          "that glow is what reads as one substance rather than two pictures "
          "averaged. BIAS decides which side wins: 0 is all of the layer below, 1 is "
          "all of this one, and 0.5 lets brightness decide pixel by pixel.\n\n"
          "WEAVE cuts the picture into bands and rotates which layer LEADS. Every "
          "layer set to Weave joins one group together with the base, so two "
          "weaving layers alternate and three cycle A, B, C, A, B, C. Nobody is "
          "hidden — they trade the foreground band by band. BANDS sets the count, "
          "ANGLE the direction, SHARE how wide a band each one claims.\n\n"
          "Weave is the one blend that is not a question about a pair, which is why "
          "it counts the whole group before deciding anything. Answered a pair at a "
          "time, a third layer met a decision the first two had already made and "
          "read as a faint wash instead of a third strand." },

        { "PER-LAYER SETTINGS",
          "OPACITY is how much of the layer arrives at all.\n\n"
          "ROTATE is NOT here. A layer's quarter turn is the module's own setting, "
          "in the module's own editor, exactly where it is for a module sitting in "
          "the HUD. Fusion reads it and its shader performs the turn — a layer is "
          "never drawn in a rectangle of its own, so the transform that turns an "
          "ordinary panel never reaches it — but Fusion holds no second opinion "
          "about which way up a module is. Two knobs that could disagree would "
          "have been one too many.\n\n"
          "SCALE pushes the layer in or out of the frame. Anything outside it is "
          "empty rather than smeared, so a shrunk layer sits inside the composite "
          "instead of stretching its edge pixels across it.\n\n"
          "Each layer then has its OWN Mirror, Symmetry, Spin, Speed and Zoom — the "
          "same five stages the Global block below applies to the finished picture. "
          "That is the difference worth knowing: folding a LAYER and folding the "
          "RESULT are two different pictures. Three layers each folded differently "
          "and then met is something the module simply could not make when every "
          "layer had to share one fold." },

        { "THE BLOCKS - folding the editor away",
          "Each layer is a block you can fold shut, and so is the Global chain. "
          "A folded block leaves one row naming what is in the slot, which is "
          "exactly as much of it as you need while you work on another one.\n\n"
          "Folding is remembered: it survives closing the controller and it is saved "
          "with the preset. A stack of three is unreadable if every slot springs "
          "open again the moment it loads." },

        { "WARP - the layers bend each other",
          "A coordinate stage: it decides WHERE the stack is read from, not how it "
          "combines. One chosen layer's brightness gradient displaces everything "
          "else, and is itself pushed the opposite way — pushing them together would "
          "only translate the picture, pushing them apart is what makes them carve "
          "into each other.\n\n"
          "SOURCE picks which layer drives it. AMOUNT is how far pixels travel. "
          "SWIRL is the character: at 0 the picture is pushed away from bright "
          "structures, at 1 it flows around them.\n\n"
          "SMOOTH is how much of the source's DETAIL the displacement follows. A "
          "strong warp used to look like a mess because the gradient followed every "
          "speck: a spectrum's bars or a particle field flip it from pixel to "
          "pixel, so neighbouring output pixels get read from wildly different "
          "places and the picture tears along the noise instead of flowing with the "
          "shapes. Turned up, the field is measured across a wider radius and its "
          "peaks go through a soft knee — it follows the shapes, and the long "
          "sudden jumps that read as torn are gone. At 0 nothing of the sort "
          "happens and not one extra sample is taken, so it is exactly the warp it "
          "always was.\n\n"
          "DENOISE works on the RESULT rather than the field: a small blur on the "
          "displaced reads. Where Smooth stops the warp tearing at its source, "
          "Denoise averages away whatever speckle is left and lets nearby colours "
          "and curves run into one another — the picture goes simpler and softer, "
          "the noise goes. Liquid has its own Denoise for the same reason, so the "
          "melted flow reads as one soft body of colour. At 0 the reads are crisp "
          "and untouched, exactly as before.\n\n"
          "With a single layer the gradient is that layer's own, so it warps itself "
          "— a slower, liquid version of the same effect." },

        { "SYMMETRY and MIRROR - two folds, not one",
          "SYMMETRY is how many congruent WEDGES the picture is folded into. 1 is no "
          "fold and is the default; 6 to 8 is the classic kaleidoscope, and 11 is a "
          "dense mandala. It makes the picture radial: the angle is wedged and the "
          "radius is folded to fill, which is what pulls the source into a rosette.\n\n"
          "MIRROR is how many reflection AXES run through the centre, evenly spaced, "
          "the first one at ANGLE. 0 is no fold. 1 folds one half onto the other. 2 "
          "adds a second axis at right angles to it, and N puts an axis every 180/N "
          "degrees.\n\n"
          "The difference that matters: mirror leaves the RADIUS alone. A mirrored "
          "layer stays life-size and keeps its left and its right; a wedged one is "
          "pulled into a rosette. They compose, and a mirrored picture cut into "
          "wedges is not the same thing as either on its own.\n\n"
          "Both fold the SAMPLING rather than a finished image, so neighbouring "
          "copies meet as mirror images and there is never a visible seam, however "
          "far it is turned. And both are offered twice: once per layer and once "
          "globally." },

        { "SPIN, ZOOM, SPEED, VORTEX",
          "SPIN is where the fold is turned to, and at SPEED 0 it stays exactly "
          "there — nothing rotates on its own unless you ask it to. SPEED 1 is a "
          "fast turn, and everything between is a drift.\n\n"
          "ZOOM pushes the whole picture in or out of the fold.\n\n"
          "VORTEX turns by an angle that GROWS with the radius: the centre stands "
          "still and the edge is dragged round, so straight lines become spirals "
          "and a kaleidoscope stops being a fixed rosette. Bipolar — swirling the "
          "other way is a different picture, not a smaller one.\n\n"
          "It runs FIRST in the chain, before either fold, and that is what makes "
          "it usable: swirl an already-wedged picture and each wedge twists away "
          "from its neighbours until every seam opens. Swirl the source, then fold "
          "it, and the copies still meet exactly." },

        { "THE ORDER OF THE CHAIN",
          "Zoom, then Vortex, then Symmetry, then Spin, then Mirror — and Warp "
          "last of all, on the sampling the stack is finally read at.\n\n"
          "Mirror sits after the wedges on purpose. Folding the FINISHED rosette is "
          "a picture you cannot get the other way round; folding first only changes "
          "what the wedges are cut from, which with an even wedge count is barely a "
          "change at all. It also means the Axis angle is a real angle in the "
          "module, fixed where you put it, instead of being carried round by "
          "Spin.\n\n"
          "The same chain, in the same order, is what each layer applies to itself. "
          "One implementation, so a per-layer fold and a global fold can never quietly "
          "come to mean two different things." },

        { "REACT - what the audio does",
          "Audio only ever MODULATES the settings you dialled in, it never replaces "
          "them: at silence the module looks exactly like your settings, and level "
          "pushes the fold angle and the warp distance further. REACT at 0 makes "
          "Fusion fully static and deterministic — useful when the layers are "
          "already audio-reactive and you only want the fusion to sit still." },

        { "COVERAGE - why layers do not hide each other",
          "A module normally fills its whole rectangle: the theme gradient first, "
          "the drawing on top. Stacked as-is, the top layer would simply hide "
          "everything below it.\n\n"
          "So a module lent to a Fusion is told to SKIP its background, and what "
          "arrives here in the alpha channel is real coverage that the module itself "
          "decided. Nothing has to be guessed — and that holds for every module, "
          "including the three that draw themselves on the GPU.\n\n"
          "Each of them answers the question in its own terms, because each knows "
          "something different about its own picture. Synesthesia hands over the "
          "fractal with its brightness as coverage. Geometry composites its glow "
          "veil and its strokes onto nothing, so the coverage they already carried "
          "survives. The Spectrogram uses the ENERGY — the one thing a spectrogram "
          "actually knows — so silence is transparent and a partial is as solid as "
          "it is loud.\n\n"
          "The older approach subtracted the known background back out of a finished "
          "frame and treated the remainder as coverage. That is an inverse operation "
          "on 8-bit data: it left a grey veil over dark areas and ate faint tails and "
          "glows, and the threshold that traded one against the other was guesswork. "
          "None of that is here any more." },

        { "DETAIL - what a layer is allowed to cost",
          "A layer is not the picture. It is a texture the fusion then folds, "
          "scales, warps and composites, and it is sampled smoothly into the "
          "fusion's rectangle — so past a point its resolution buys almost nothing "
          "while costing everything, because it costs AREA.\n\n"
          "Uncapped, three layers in a fullscreen HUD meant three full-screen "
          "module renders — Geometry's entire bloom pyramid among them — sixty "
          "times a second, on top of the fusion's own full-resolution pass. That is "
          "why enlarging the window was where a fusion started to stutter.\n\n"
          "DETAIL is a CAP and not a fraction, which is the part worth knowing: "
          "below it nothing changes at all, and above it the layers simply stop "
          "growing. Dragging the window bigger past the cap therefore costs "
          "nothing — the layers keep the size they had, and the framebuffer "
          "reallocations and static-cache rebuilds that every size change triggers "
          "stop with them.\n\n"
          "Normal is about a megapixel per layer, the same budget the 2D modules "
          "have always given themselves. Turn it up if you are running one layer at "
          "high symmetry, where the source is read close to 1:1 and softness shows; "
          "turn it down if you are running three." },

        { "PERFORMANCE",
          "The fusion is a fragment shader on the GPU, evaluated per output pixel at "
          "the module's real size. There is no render pixel budget and no upscale, "
          "so sharpness and smoothness are no longer paid for out of the same "
          "currency.\n\n"
          "A layer that draws itself with a shader — Synesthesia, Geometry, the "
          "Spectrogram — never leaves the GPU at all. It is drawn once into a "
          "framebuffer of its own and the fusion samples it where it already is.\n\n"
          "That used to be a full round trip through main memory every frame: "
          "rendered, pulled back, wrapped in an image, and uploaded again. The "
          "pull-back stalls the pipeline until the GPU has caught up, and it "
          "happened in the HUD's own context — so a third layer did not merely cost "
          "more, it stalled everything, the controller included. Now the cost of a "
          "third layer is one more small draw.\n\n"
          "The 2D modules still upload a finished frame, because their pixels "
          "genuinely start in main memory. Even then it is skipped for a layer that "
          "has produced nothing new: a module running at 30 fps is uploaded 30 times "
          "a second, not 60.\n\n"
          "The FUSION PASS costs the same at any size in CPU terms — it is one "
          "shader over the output. The LAYERS do not: a module render costs area, "
          "which is what DETAIL is for. Capped, three layers cost three modules at "
          "a bounded size, and enlarging the window past the cap adds nothing at "
          "all." }
    }) {}
};
