/*
  ==============================================================================

    InfoWindowSynesthesia.h
    Educational guide for the Synesthesia module.

  ==============================================================================
*/

#pragma once
#include "InfoWindowAudioMeters.h"   // ModuleInfoWindow + TextInfoWindow

class SynesthesiaInfoWindow : public TextInfoWindow
{
public:
    SynesthesiaInfoWindow() : TextInfoWindow ("Synesthesia - Educational Guide",
    {
        { "WHAT IT DOES",
          "An OpenGL fractal engine that translates music into colour, motion and "
          "symmetry - inspired by synesthesia, where one stimulus (sound) elicits "
          "another perception (colour). It is NOT a measurement tool: it is the "
          "expressive, performance-facing module of ALTER. Use it for VJ sets, streams, "
          "installations and studio inspiration; pair it with Spectrum or Audio Meters "
          "when the session needs numbers." },

        { "HOW SOUND BECOMES COLOUR",
          "The dominant tone is detected, converted to a musical pitch and reduced to a "
          "pitch CLASS (C, C#, ... B) - so C2 and C5 share the same colour family, and "
          "the wheel repeats every octave. The note picks the base hue; each shader "
          "layer then shifts the hue slightly, so one note becomes a glowing colour "
          "family instead of one flat stroke. The OCTAVE decides how many layers wake "
          "up: higher notes = more layers, more geometry, more secondary accents. "
          "Pitch is read from the musically meaningful low/mid range (~70 Hz - 2 kHz); "
          "hats, air and noise deliberately do NOT repaint the image.\n"
          "The wheel is shared with Chladni and Geometry, so one note is one colour "
          "across the whole app. MIRROR TONE COLOR reverses the direction the notes "
          "travel around it (C stays red, C# becomes orange instead of rose, and so "
          "on). It is per module, so you can deliberately run two modules on opposite "
          "palettes off the same tone." },

        { "SCULPTING CONTROLS",
          "FRAGMENT: morphs the fractal structure. It does NOT recolour the image - the "
          "note owns the colour, so the same tone reads the same in every module.\n"
          "TRANSMUTE: a second, symmetric morph - radial rings and lens-waves that keep "
          "the composition centred.\n"
          "CLEAR: reduces layer complexity from the top; calms the image while keeping "
          "reactivity.\n"
          "DENOISE: fuses the thin dashed secondary curves into continuous lines and "
          "dims them slightly, so back layers stay perceptually BEHIND the main stroke. "
          "Thickness and glow belong to BLOOM, not Denoise.\n"
          "MIRROR: reflective fold - clean left/right + top/bottom symmetry on top of "
          "the SYMMETRY kaleidoscope (1-8 fold)." },

        { "BPM SYNC - LIGHT ON THE BEAT",
          "BPM sync adds a light wave that sweeps from the centre outward on every beat "
          "division; it does not change the evolution speed (that is SPEED). BEAT DIV "
          "picks the division, BPM the tempo.\n\n"
          "With the ALTER Creator plugin on a track, the pulse PHASE-LOCKS to the DAW: "
          "the plugin streams the host tempo and song position (PPQ), so the flash lands "
          "exactly on Ableton's beats when you play from 1.1.1. The manual BPM is only "
          "the fallback when nothing is playing." },

        { "HOW TO USE IT",
          "- Live/VJ: detach the panel, go fullscreen on a second display; let SMOOTH "
          "and REACT set the temperament (twitchy vs flowing).\n"
          "- Studio: a peripheral-vision energy meter - busy visual = busy mix.\n"
          "- Sound design/education: watch how stable tones hold a colour family and "
          "how octaves change the attitude, not the colour.\n"
          "- Do not use it to judge loudness or tonal balance - wrong tool." },

        { "CONTROLS",
          "TONE SMOOTH: analysis inertia (pitch/colour changes). CURVE SMOOTH: motion "
          "inertia (ghosting/lag of the curves).\n"
          "ZOOM 0.5-2.0, ROTATION (bipolar spin), SYMMETRY 1-8, MIRROR fold.\n"
          "SATURATION: 0 = monochrome, 2 = hyper-neon. BLOOM: glow/thickness.\n"
          "SPEED: evolution speed, bipolar (-2..2; negative = reverse, 0 = still).\n"
          "REACT: audio-driven shake/jitter.\n"
          "FRAGMENT / TRANSMUTE / CLEAR / DENOISE: see Sculpting above.\n"
          "BPM SYNC + BPM + BEAT DIV: beat-locked light pulse (host-synced with the "
          "Creator plugin).\n"
          "TONE COLOUR vs manual COLOR: pitch-driven family vs your fixed colour.\n\n"
          "Note: module rotation (90/180/270) is not available here - OpenGL components "
          "cannot be transformed by the window system." }
    }) {}
};
