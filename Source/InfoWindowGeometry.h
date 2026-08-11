/*
  ==============================================================================

    InfoWindowGeometry.h
    Educational guide for the Geometry module.

  ==============================================================================
*/

#pragma once
#include "InfoWindowAudioMeters.h"   // ModuleInfoWindow + TextInfoWindow

class GeometryInfoWindow : public TextInfoWindow
{
public:
    GeometryInfoWindow() : TextInfoWindow ("Geometry - Educational Guide",
    {
        { "WHAT IS GEOMETRY?",
          "A generative visual built from the simplest ingredients - triangle, square "
          "and circle - rushing toward you through a perspective tunnel. The audio does "
          "not draw the shapes directly: level fades the scene in, REACT adds pulse and "
          "shake, and Tone Colour can make the whole palette follow the dominant "
          "musical pitch. With no signal the tunnel fades out." },

        { "SHAPES, COMPLEXITY & RANDOM",
          "TRIANGLE / SQUARE / CIRCLE are probability weights - they set how often each "
          "shape is born, so one shape can own the identity or all three can mix. "
          "COMPLEXITY sets how many shapes exist at once (1-48; in BPM mode it becomes "
          "shapes-per-burst). RANDOM sets each shape's birth angle: at 0 every shape is "
          "born at the same angle - turn ROTATION up and they twist into a clean "
          "spiral; at 1 the angles are fully random." },

        { "FREE vs BPM - WHEN SHAPES ARE BORN",
          "FREE mode is a continuous ring: COMPLEXITY shapes evenly spaced in depth, "
          "SPEED sets the travel. BPM mode spawns shapes in bursts ON THE BEAT: "
          "BEAT DIV picks the musical division (with dotted/triplet variants), BPM sets "
          "the tempo, COMPLEXITY sets how many shapes per burst.\n\n"
          "HOST SYNC: with the ALTER Creator plugin on a track, the plugin streams the "
          "DAW tempo and song position (PPQ), and Geometry phase-locks its beat clock "
          "to that grid. Play from 1.1.1 and every burst lands exactly on Ableton's "
          "beat - the host clock overrides the manual BPM while the DAW is playing, so "
          "the visual cannot drift. Manual BPM is the fallback when nothing plays." },

        { "TUNNEL, APERTURE & DEPTH",
          "DEPTH = spawn distance: low = shallow tunnel (shapes appear close and big), "
          "high = deep tunnel (shapes start tiny and rush in). TUNNEL morphs the layout "
          "from shapes spread around the centre (0) into a fly-through tube (1). "
          "APERTURE is the iris: 1 fills the centre with incoming edges, 0 opens a "
          "clean ring. Shapes draw back-to-front so the depth always reads correctly." },

        { "COLOUR - BASE OR TONE",
          "Base mode: an analogous palette (neighbouring hues) derived from your COLOR - "
          "the layers fan around it across the tunnel depth.\n"
          "TONE COLOUR mode: the dominant pitch class picks the colour, on the wheel "
          "shared with Synesthesia and Chladni, so one note is one colour across the "
          "whole app. Depth still shades the shapes, but only by about +/-4 degrees of "
          "hue - enough to separate near from far, too little to read as another note. "
          "Octaves repeat the same colour - that is musically correct, not a bug. "
          "Detection works best ~200 Hz - 2 kHz; noisy/percussive material keeps the "
          "previous hue instead of flickering.\n"
          "MIRROR TONE COLOR: reverses the direction the notes travel around the wheel "
          "(C stays red, C# becomes orange instead of rose). Per module." },

        { "MOTION & LOOK",
          "SPEED: travel speed, bipolar (-2..2; negative = shapes recede, 0 = still).\n"
          "ROTATION: per-shape spin. MODULE ROT: spins the WHOLE module (bipolar).\n"
          "REACT: audio pulse + camera shake. SMOOTH: how fast level is followed.\n"
          "ZOOM: overall scale. SYMMETRY: 1-8 rotated copies. MIRROR: reflective fold.\n"
          "SATURATION: colour intensity. BLOOM: neon glow (auto-reduced when very many "
          "shapes are on screen, to keep the motion smooth)." },

        { "HOW TO USE IT",
          "- VJ/live: BPM mode + host sync is the star - bursts land on the grid even "
          "through tempo changes. Recipes: Triangle 1.0 / Complexity 24-44 / Random low "
          "= neon spiral; Circle high / Symmetry 6-8 / Speed low = orbital mandala.\n"
          "- Streams and studio: low Complexity, gentle React - a living backdrop that "
          "breathes with the mix without stealing focus.\n"
          "- It is a visual instrument, not a meter: for analysis use Spectrum, "
          "Spectrogram or Audio Meters." }
    }) {}
};
