/*
  ==============================================================================

    InfoWindowStereoscope.h
    Educational guide for the Stereoscope module.

  ==============================================================================
*/

#pragma once
#include "InfoWindowAudioMeters.h"   // ModuleInfoWindow + TextInfoWindow

class StereoscopeInfoWindow : public TextInfoWindow
{
public:
    StereoscopeInfoWindow() : TextInfoWindow ("Stereoscope - Educational Guide",
    {
        { "WHAT IT MEASURES",
          "The STEREO truth of your audio: width, phase, balance, mid/side energy and "
          "mono compatibility. It does not measure loudness or pitch; it answers 'how "
          "wide is it, where does it sit, and will it survive when the club sums it to "
          "mono?'. Mid = L+R (the shared centre: kick, sub, vocal). Side = L-R (the "
          "difference: reverbs, wideners, hard-panned content)." },

        { "PARTICLES MODE - FREQUENCY WIDTH MAP",
          "Shows where stereo width lives across the spectrum. Frequency runs bottom-to-"
          "top on a log scale (20 Hz - 20 kHz checkpoints); the centre vertical line is "
          "the mono axis. Each band's cloud is placed by its L/R PAN (only-left = far "
          "left, only-right = far right) and spread by its true STEREO WIDTH: the module "
          "compares the complex Mid (L+R) and Side (L-R) spectra per band, so genuinely "
          "decorrelated stereo fans outward even when both channels are equally loud, "
          "while mono content collapses to a tight centre dot. DENSITY sets the particle "
          "count.\n\n"
          "Practical read: sub and kick should sit as a stable centre column below "
          "~120 Hz; width belongs to the mids and highs (reverb, pads, cymbals). A low "
          "end spraying sideways = wideners/chorus/Haas on the bass - impressive in "
          "headphones, gone on a mono sub rig." },

        { "GONIOMETER MODE - CLASSIC VECTORSCOPE",
          "The Lissajous mid/side scope on a circular grid with L/R/M guides. The circle "
          "is the clipping level (the trace is clipped to it). Vertical shape = strong "
          "mono centre; tilt toward a diagonal = channel dominance / panning; wide "
          "horizontal smear = heavy side energy or anti-phase risk. PARTICLES switches "
          "between a clean line and a particle cloud with motion-blur trails; DENSITY "
          "sets the cloud size." },

        { "POLAR MODE - PHASE DIRECTION",
          "Samples shoot from the bottom centre: straight up = mono alignment, radius = "
          "level (the arc = full scale). The +/-45 degree guides are practical warning "
          "lines - a hard-panned mono element may legitimately sit near one, but "
          "IMPORTANT material living beyond them for long means side energy is "
          "overpowering the mid. Points past the guides are highlighted." },

        { "CORRELATION MODE - MONO SURVIVAL HISTORY",
          "A scrolling history of L/R phase correlation: +1 = highly similar, mono-safe; "
          "0 = decorrelated / wide / ambient; -1 = opposite polarity - cancels hard in "
          "mono. The thin secondary line tracks L/R balance. Momentary dips are normal; "
          "SUSTAINED negative correlation on important material is the warning." },

        { "CORRELOMETER MODE - PER-BAND MONO MAP",
          "The spectral correlometer: an analog-style LED bar wall showing L/R "
          "correlation PER FREQUENCY BAND on a log axis (20 Hz - 20 kHz). Bar height "
          "1.0 (top) = that band is fully mono / in phase; 0.0 (bottom) = fully "
          "stereo (decorrelated or anti-phase). Silent bands stay dark.\n\n"
          "Practical read: the low end should sit near the TOP (tall bars = mono-"
          "safe); short bars below ~120 Hz mean the bass will thin out on a mono "
          "rig. Short bars in the mids/highs are usually intentional width (reverb, "
          "pads, cymbals).\n\n"
          "Resolution is 512 FFT bins by default; enable USE CONTROLLER BINS to "
          "follow the global Max-bins setting (top right of the controller) for "
          "finer low-frequency detail. SMOOTH sets the correlation averaging time - "
          "higher = steadier, more trustworthy reading." },

        { "HOW TO USE IT",
          "Producing: watch Particles while building - if the low end sprays sideways, "
          "check stereo wideners, unison synths, stereo samples and Haas delays.\n"
          "Mixing: keep kick, sub, bass body, lead vocal and main snare centred; run "
          "Correlation on buses - hovering around 0 can be fine, living below 0 means "
          "solo it and mono-check it.\n"
          "Mastering: the master should keep width without sacrificing the centre. If a "
          "stereo enhancer pushes correlation below zero, you made the mix wider AND "
          "less reliable at the same time.\n"
          "DJ/live prep: compare tracks - a wild stereo low end sounds huge in "
          "headphones and loses weight on club mono subs.\n"
          "Sound design: creative anti-phase and huge side energy are allowed - just "
          "know it is a choice, not an accident." },

        { "CONTROLS",
          "MODE: Particles / Goniometer / Polar / Correlation / Correlometer.\n"
          "SMOOTH: frame smoothing; in the scope modes it also sets trail persistence, "
          "in the Correlometer the correlation averaging time.\n"
          "PARTICLES (Goniometer): clean line vs particle cloud.\n"
          "DENSITY: particle count (Particles mode + Goniometer cloud).\n"
          "BRIGHTNESS (Goniometer / Polar): trace intensity, from faint to "
          "white-hot; 0.5 = neutral.\n"
          "USE CONTROLLER BINS (Correlometer): follow the global Max-bins setting "
          "instead of the default 512 bins.\n"
          "COLOR: trace/particle colour (the Correlometer builds its analog gradient "
          "from it).\n\n"
          "There is no gain control on purpose: every mode shows the absolute, true "
          "level, so the circle / edge always means real full-scale." }
    }) {}
};
