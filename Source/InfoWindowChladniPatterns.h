/*
  ==============================================================================

    InfoWindowChladniPatterns.h
    Created: 21 Apr 2026 10:55:23am
    Author:  Martin Peroncik

  ==============================================================================
*/

#pragma once
#include "InfoWindowAudioMeters.h"   // ModuleInfoWindow + TextInfoWindow

class ChladniInfoWindow : public TextInfoWindow
{
public:
    ChladniInfoWindow() : TextInfoWindow ("Chladni Patterns - Educational Guide",
    {
        { "WHAT ARE CHLADNI PATTERNS?",
          "When a plate vibrates at one of its resonance frequencies, sand sprinkled on it "
          "migrates away from the moving regions and collects along the NODAL LINES - places "
          "where the plate stands still. Each resonance mode (m,n) produces its own geometric "
          "figure. Ernst Chladni demonstrated this in 1787; the same physics shapes violin "
          "tops, speaker cones and cymatics art.\n\n"
          "Plate model: free-edge rectangular plate (Leissa 1969):\n"
          "Z(x,y) = cos(n pi x) cos(m pi y / r) - cos(m pi x) cos(n pi y / r)" },

        { "THE SIMULATION",
          "Thousands of sand grains random-walk with step size proportional to the local "
          "plate amplitude |Z| and drift toward the nearest nodal line. Signal level (RMS) "
          "drives the vibration: loud passages shake the sand into motion, silence lets it "
          "rest. The figure that emerges IS the nodal-line set of the current (m,n) mode." },

        { "AUDIO REACTIVE - PHYSICAL MODEL",
          "Exactly like a real plate: the DOMINANT FREQUENCY in the audio excites the "
          "eigenmode whose resonance f(m,n) = k(m^2/r^2 + n^2) lies nearest to it. "
          "Low notes land on low modes (simple figures), high content on high modes "
          "(fine dense figures).\n\n"
          "Because k depends on the plate, MATERIAL and ASPECT RATIO change which pattern "
          "a given sound produces - play the same bass note on aluminium and on acrylic "
          "and you get two different figures, as in the physical world. A short "
          "hysteresis stops flicker; on a mode change the sand is shaken up and "
          "resettles, like tapping a real plate." },

        { "HOW TO USE IT (sound engineering)",
          "- Drop it on a master bus as an intuitive 'what is the energy doing' display: "
          "drops and builds read instantly as figure changes.\n"
          "- Solo a synth or bass: watch n track the patch brightness as you open a filter.\n"
          "- Live visuals: AR + material + colour-by-tone make it a self-running visual "
          "instrument that follows the performance." },

        { "CONTROLS",
          "AUDIO REACTIVE: automatic (m,n) - the dominant tone chooses the plate's "
          "natural response. Off = manual control.\n"
          "SHIFT (0-6): pushes the matched (m,n) pair upward TOGETHER, so the detected "
          "response draws a richer, more complex cousin of itself - reactivity is "
          "preserved, the figure family just gets fancier.\n"
          "PRESET: classic figures to explore by hand (selecting one switches to manual).\n"
          "m, n: mode indices 1-12. m = n is degenerate (blank) in this plate model.\n"
          "ASPECT RATIO: plate proportions 0.25-4.0 - stretches the figure.\n"
          "SAND SMOOTH: grain movement. 0 = fast nervous sand, 1 = slow fluid settling.\n"
          "PARTICLES: number of sand grains (1000-15000). Each grain lives 10-20 s and "
          "then respawns elsewhere, so sand keeps circulating and never piles up "
          "permanently.\n"
          "MATERIAL: aluminium/steel/glass/acrylic - changes the displayed resonance "
          "frequency (plate stiffness and density).\n"
          "COLOR BY TONE (on by default): hue follows the dominant pitch class around "
          "the colour wheel shared with Synesthesia and Geometry - C=red, C#=rose, "
          "D=magenta ... B=orange - so one note is one colour across the whole app. "
          "The pitch behind the COLOUR is read from ~65 Hz - 2 kHz so the fundamental "
          "wins; the pattern itself still tracks the perceptually weighted peak.\n"
          "MIRROR TONE COLOR: reverses the direction the notes travel around the wheel "
          "(C stays red, C# becomes orange). Per module.\n"
          "COLOR: the fixed sand colour, used when COLOR BY TONE is off.\n\n"
          "Bottom strip: ~(m,n) = reactive, frequency = theoretical resonance of the mode "
          "for the chosen material, AR = aspect ratio." }
    }) {}
};
