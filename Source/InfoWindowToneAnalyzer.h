/*
  ==============================================================================

    InfoWindowToneAnalyzer.h
    Educational guide for the Tone Analyzer module.

  ==============================================================================
*/

#pragma once
#include "InfoWindowAudioMeters.h"   // ModuleInfoWindow + TextInfoWindow

class ToneAnalyzerInfoWindow : public TextInfoWindow
{
public:
    ToneAnalyzerInfoWindow() : TextInfoWindow ("Tone Analyzer - Educational Guide",
    {
        { "WHAT IT DOES",
          "Shows which musical notes are sounding right now and the chord they form. "
          "The 12 columns are the pitch classes C..B with smoothed energy; above them the "
          "exact notes (e.g. 'A2  E3  C#4') and the detected chord name." },

        { "HOW DETECTION WORKS",
          "1. PEAK PICKING: strict local maxima in the 50 Hz - 5 kHz band, well above the "
          "noise floor, with parabolic interpolation for sub-bin frequency accuracy.\n"
          "2. HARMONIC SUPPRESSION: a real note produces overtones at 2x, 3x, 4x... its "
          "fundamental (piano partials are even stretched slightly sharp). These are "
          "recognised with a generous tolerance and NOT shown as extra notes.\n"
          "3. LEVEL GATE: only peaks within 18 dB of the strongest one count - reverb "
          "tails and resonances stay out.\n"
          "4. TEMPORAL GATE: a note must persist ~130 ms before it appears, so transients "
          "and noise never flicker in. Max 5 simultaneous notes." },

        { "CHORD DETECTION",
          "Active pitch classes are matched against templates: maj, min, dim, aug, sus2, "
          "sus4, 7, maj7, m7, m7b5, dim7, 6, m6, add9. All chord tones must be present; "
          "the lowest sounding note is preferred as the root; the name must stay stable "
          "for a few frames before it is displayed." },

        { "HOW TO USE IT (sound engineering)",
          "- Check the key/chords of an unlabeled sample or stem before comping it.\n"
          "- Tune 808s and kicks: the strip shows the exact note of the dominant low peak.\n"
          "- Verify vocal harmony stacks: each stacked voice should appear as its own "
          "pitch class.\n"
          "- Spot clashing resonances: a persistent unexpected note in a mix often points "
          "to a ringing room mode or a mistuned layer." },

        { "CONTROLS & TIPS",
          "SENSITIVITY: how far above the noise floor a peak must rise. Keep it LOW for "
          "dense mixes, distorted content or anything with long reverb; raise it for "
          "clean solo instruments.\n"
          "COLOR BY TONE: the accent follows the dominant note, so the module's colour "
          "says what its text says. MIRROR TONE COLOR reverses the wheel; TONE SMOOTH "
          "sets how fast the colour chases a new note.\n"
          "  Note it tracks the SHARED tone hue (the 65 Hz - 2 kHz peak), not this "
          "module's own detector, even though this one is more accurate. The point of "
          "tone colour is that the same note reads as the same colour in every module "
          "at once; on a chord the two detectors would pick different roots, and a HUD "
          "whose modules disagree about the colour of the music looks broken rather "
          "than more precise. The better detection is still what names the notes.\n"
          "COLOR: accent colour of the columns and chord glow (manual mode).\n\n"
          "Best results: solo the instrument you are analysing. Cymbal-heavy or heavily "
          "saturated material blurs the spectrum and naturally degrades note separation." }
    }) {}
};
