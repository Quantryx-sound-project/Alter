/*
  ==============================================================================

    InfoWindowSpectrogram.h
    Educational guide for the Spectrogram module.

  ==============================================================================
*/

#pragma once
#include "InfoWindowAudioMeters.h"   // ModuleInfoWindow + TextInfoWindow

class SpectrogramInfoWindow : public TextInfoWindow
{
public:
    SpectrogramInfoWindow() : TextInfoWindow ("Spectrogram - Educational Guide",
    {
        { "WHAT IT MEASURES",
          "A time-frequency image of the signal: time runs left to right (newest at the "
          "right edge), frequency runs bottom to top on a log scale 16 Hz - 20 kHz, and "
          "colour shows energy over a 100 dB range. Spectrum shows the current frequency "
          "snapshot; Spectrogram shows how that shape moves, repeats and drifts through "
          "time. If something is hiding in the mix - a click, hum, resonance or codec "
          "cutoff - this is the module that catches it." },

        { "THREE ANALYSIS ENGINES - PICK THE QUESTION FIRST",
          "LINEAR (default): classic STFT rows. Balanced detail, lowest CPU, right for "
          "general mix reading. Physics limit: one FFT bin covers a large musical distance "
          "in the sub range, so deep bass rows are naturally coarser.\n\n"
          "CONSTANT-Q: multi-resolution analysis - a long window for bass, medium for "
          "mids, short for highs. Every log row shows true per-band energy, so each "
          "semitone from sub-bass up to 20 kHz gets fair resolution. Use it for "
          "basslines, chords and harmony. Trade-off: long bass windows react more slowly.\n\n"
          "ENHANCED FREQ (spectral reassignment): the forensic mode. Every sample is "
          "analysed gap-free with heavy overlap, and each bin's energy is re-assigned to "
          "its TRUE instantaneous frequency and time. Steady tones collapse into "
          "razor-thin lines, transients stay sharp verticals - the professional-analyzer "
          "look. Use it for tuning checks, pitch drift, vibrato, hum harmonics and click "
          "forensics. Constant-Q and Enhanced Freq are mutually exclusive." },

        { "HOW TO READ IT",
          "- Horizontal lines = sustained tones; evenly stacked lines above one are its "
          "harmonics. A weak or missing fundamental shows immediately.\n"
          "- Vertical stripes = transients: drum hits, clicks, mouth noises, edit pops.\n"
          "- Solid low band = kick/bass energy; check it stays controlled BETWEEN hits - "
          "constant glow means rumble eating headroom.\n"
          "- Bright wash at the top = cymbals/air; if it never stops, suspect hiss, "
          "harshness or over-bright hats.\n"
          "- Hum shows as razor-thin lines at 50/60 Hz and harmonics.\n"
          "- Lossy-codec damage shows as a hard cutoff ceiling around 16-20 kHz.\n"
          "- Dark gaps are good: space is what makes impact possible." },

        { "MIXING & MASTERING WORKFLOW",
          "Mixing: hunt resonances, clicks, sibilance and low-end overlap. If kick, bass, "
          "pad and vocal all paint the same 120-300 Hz cloud, that is the mud - EQ by ear, "
          "but let the image tell you where to dig.\n"
          "Mastering: final scan for tonal balance and hidden garbage - constant HF fizz, "
          "codec-looking cutoffs, rumble between hits, or sections that go spectrally "
          "flat because the limiter crushed all movement.\n"
          "DJ/live prep: long windows compare tracks - less sub? more noise? denser mids? "
          "You see why the next track feels weak before the crowd does.\n"
          "Sound design: a fingerprint scanner - impacts, risers, FM tones and granular "
          "textures each leave a distinct pattern." },

        { "WINDOW LENGTH - CHOOSE THE SCALE",
          "WINDOW sets the visible time span (1-120 s). Short windows (1-10 s) for clicks, "
          "transients and phrase-level movement; long windows (30-120 s) for arrangement "
          "structure and tonal-balance drift. Hunting a tiny click on a 120 s window is "
          "like using a world map to find a screw on the floor." },

        { "CONTROLS",
          "SMOOTH: temporal smoothing per row. 0 = raw snappy frames (repairs), higher = "
          "silkier motion (performance visuals).\n"
          "WINDOW: visible time span 1-120 s.\n"
          "ROW FILL: 0 = thin exact lines (analysis), 1 = filled band rows (stage look).\n"
          "CONSTANT-Q / ENHANCED FREQ: analysis engines (see above), mutually exclusive.\n"
          "MIRROR AXIS: flips the frequency axis (lows at the top).\n"
          "CUSTOM COLOR: your own gradient - COLOR tints mid intensities, COLOR 2 the "
          "loudest peaks. Keep quiet-vs-loud readable: if the viewer cannot tell them "
          "apart, the visual is lying.\n\n"
          "Under the hood: 1024 log-frequency rows, adaptive column density (~1 column "
          "per screen pixel, up to 240/s); Enhanced Freq runs a gap-free ~93 %-overlap "
          "STFT with full time+frequency reassignment." },

        { "COMMON MISTAKES",
          "- Bright does not always mean better: it can be detail or it can be hiss.\n"
          "- Do not ignore the low end because your speakers cannot play it - the "
          "spectrogram shows sub garbage anyway.\n"
          "- Too much smoothing hides fast problems.\n"
          "- One sine will not draw one infinitely thin row in Linear mode - finite "
          "windows spread energy slightly. Switch to Enhanced Freq when you need "
          "razor-thin truth.\n"
          "- It is not a loudness meter: for RMS/LUFS/True Peak use Audio Meters." }
    }) {}
};
