/*
  ==============================================================================

    InfoWindowOscilator.h
    Created: 29 Mar 2026 1:52:49pm
    Author:  Martin

    Educational guide for the Oscillator module (short term + long term).

  ==============================================================================
*/

#pragma once
#include "InfoWindowAudioMeters.h"   // ModuleInfoWindow + TextInfoWindow

class OscillatorInfoWindow : public TextInfoWindow
{
public:
    OscillatorInfoWindow() : TextInfoWindow ("Oscilloscope - Educational Guide",
    {
        { "TWO VIEW MODES (TERM)",
          "SHORT TERM: a classic oscilloscope. It draws the actual waveform "
          "(sample by sample) over a tiny window of 0.1-85 ms, with a zero-crossing "
          "trigger that keeps periodic signals phase-locked so the wave stands still.\n\n"
          "LONG WAVE: the RAW waveform itself, recorded continuously at full sample "
          "rate and drawn over 20 ms - 30 s (min/max per pixel when zoomed out, the "
          "actual sample curve when zoomed in). The important difference from short "
          "term is not the length: it is NOT peak-normalised, so the height means an "
          "actual dB value and the dB scale below is real. Wind it down to 20-50 ms "
          "and you have a scope that shows you the true level.\n\n"
          "(LEVEL HISTORY - the DAW-style peak + RMS loudness overview - now lives in "
          "the AUDIO METER module: pick it from the meter's Mode menu.)" },

        { "DISPLAY MODES",
          "MONO: true mono mix of both channels as one full-height shape.\n"
          "STEREO: L waveform in the top half, R in the bottom half - instantly shows "
          "channel imbalance or one-sided content.\n"
          "MIRROR / MIRROR STEREO: waveform mirrored around the centre line for a "
          "symmetric, meter-like body.\n"
          "SYMMETRY (long wave): draws the mirrored |peak| envelope instead of the raw "
          "shape - reads like a DAW clip overview." },

        { "HOW TO USE IT (sound engineering)",
          "- Short term: inspect waveshape, distortion, phase of a synth or bass.\n"
          "- Long wave ~0.5-2 s: watch compressor pumping and transient shapes.\n"
          "- Long wave 10-30 s: see arrangement dynamics - drops, builds, automation.\n"
          "- Stereo mode: catch L/R imbalance after M/S processing.\n"
          "- HOVER the module for a readout of the time and level under the pointer." },

        { "CLIPPING ZONE (long wave)",
          "Dashed lines top and bottom mark 0 dBFS - full scale, the ceiling - with a "
          "dB ruler down the right edge (0 / -6 / -12 / -20). Both are drawn in the "
          "MODULE'S colour, and follow the note in tone mode.\n\n"
          "WHEREVER THE AUDIO REACHES THE CEILING, A RED BAR IS LAID ALONG THE LINE. "
          "That is the part worth watching: the waveform itself is CLAMPED at full "
          "scale (exactly as a DAW clip overview is), so a column that went 6 dB over "
          "draws as a flat top on the boundary and looks identical to one that just "
          "touched it. The red marks tell them apart, and show you where in time it "
          "happened.\n\n"
          "The clamp is deliberate. Letting the trace run past the boundary would buy "
          "one number - HOW far over - at the cost of shrinking every waveform to "
          "leave room for the overshoot. That number belongs on a meter, not on a "
          "waveform: use True Peak (dBTP) on an Audio Meter, which also catches "
          "inter-sample peaks a sample-domain view cannot see at all.\n\n"
          "SHORT TERM does not get any of this, and that is deliberate too: it "
          "normalises the trace to its own peak so a quiet passage still fills the "
          "window, which means a given height there is a fraction of whatever the "
          "loudest sample happened to be. A '0 dB' line on it would sit at the top of "
          "the picture at every level. Long wave draws raw amplitude, so there the "
          "scale is real.\n\n"
          "Untick CLIPPING ZONE for the bare waveform." },

        { "CONTROLS",
          "TERM: Short term / Long wave.\n"
          "SMOOTH: short-term frame smoothing (EMA between refreshes).\n"
          "ZOOM (short term): time window 0.1-85 ms, log scale.\n"
          "WINDOW (long wave): visible history 20 ms - 30 s. TIP: the MOUSE WHEEL over "
          "the module zooms the window directly.\n"
          "  It stops at 20 ms because the raw ring is filled from a 60 Hz timer - "
          "about 17 ms of audio per frame. Below that the window is shorter than one "
          "frame's worth, and with no trigger on this view (unlike short term) the "
          "wave slides instead of standing still. At 20 ms-100 ms you get what short "
          "term cannot give you: waveform detail at TRUE amplitude, against a real dB "
          "scale.\n"
          "FILL: translucent fill of the waveform.\n"
          "DISPLAY: Mono / Stereo / Mirror.\n"
          "L/R COLOR (stereo): how far the R channel's hue sits from L - "
          "COMPLEMENTARY (opposite side of the wheel, maximum separation) or "
          "ANALOGOUS (a neighbour, so the two read as one instrument in two shades). "
          "Same choice, same distances, as the Spectrum's stereo colour.\n"
          "CLIPPING ZONE (long wave): 0 dBFS boundary, dB ruler, red over-scale marks.\n"
          "COLOR BY TONE: the whole trace takes the colour of the dominant note, "
          "shared with every other tone-coloured module on the HUD.\n"
          "COLOR: module colour (manual mode)." }
    }) {}
};
