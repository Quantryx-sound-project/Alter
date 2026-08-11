/*
  ==============================================================================

    PitchUtils.h
    Helpers for turning an FFT magnitude spectrum into a frequency estimate.

    Two things matter for accurate low-frequency tone estimation:
      1. The bin width must be derived from the ACTUAL spectrum size, not a fixed
         constant — the FFT size is now chosen globally and can vary (2048..8192
         bins). binWidth = sampleRate / fftSize = sampleRate / (2 * numBins).
      2. Parabolic (sub-bin) interpolation refines the integer peak bin to a
         fractional bin by fitting a parabola through the peak and its two
         neighbours, giving sub-Hz precision without a larger FFT.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "AudioSourceInterface.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace PitchUtils
{
    /** Hz per bin for a magnitude spectrum of `numBins` (real FFT → fftSize = 2*numBins). */
    inline float binWidthHz (int numBins, double sampleRate = 48000.0)
    {
        const int fftSize = juce::jmax (2, numBins * 2);
        return (float) (sampleRate / (double) fftSize);
    }

    /** Refine an integer peak bin to sub-bin precision via parabolic interpolation
        over the peak and its two neighbours. Returns the fractional bin index.
        When `interpolate` is false (or at the edges) it returns the integer bin. */
    inline float refinePeakBin (const std::vector<float>& mag, int k, bool interpolate)
    {
        const int n = (int) mag.size();
        if (! interpolate || k <= 0 || k >= n - 1)
            return (float) k;

        const float a = mag[(size_t) (k - 1)];
        const float b = mag[(size_t) k];
        const float c = mag[(size_t) (k + 1)];
        const float denom = a - 2.0f * b + c;
        if (std::abs (denom) < 1.0e-9f)
            return (float) k;

        float delta = 0.5f * (a - c) / denom;        // peak offset in [-0.5, +0.5]
        delta = juce::jlimit (-0.5f, 0.5f, delta);
        return (float) k + delta;
    }

    /** Dominant frequency (Hz) from a magnitude spectrum, searching bins [lo, hi).
        Returns <= 0 if no peak is above `floorMag`. */
    inline float dominantFrequency (const std::vector<float>& mag, int lo, int hi,
                                    float floorMag, bool interpolate,
                                    double sampleRate = 48000.0)
    {
        const int n = (int) mag.size();
        hi = juce::jmin (hi, n);
        lo = juce::jmax (1, lo);
        int peak = -1; float peakMag = floorMag;
        for (int i = lo; i < hi; ++i)
            if (mag[(size_t) i] > peakMag) { peakMag = mag[(size_t) i]; peak = i; }
        if (peak < 0) return -1.0f;

        const float bin = refinePeakBin (mag, peak, interpolate);
        return bin * binWidthHz (n, sampleRate);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  SHARED TONE→COLOUR CONTRACT
    //
    //  Every "colour by tone" module (Synesthesia, Chladni, Geometry) must render
    //  the same semitone as the same PERCEIVED colour. Three things have to agree,
    //  not just one, and historically only the first did:
    //
    //    1. the hue wheel        — pitchClass / 12, C = red. Always agreed.
    //    2. the core saturation  — did NOT agree (Chladni sat 0.80 vs shader 1.00),
    //                              so the same note read as a pastel on the plate
    //                              and as a primary in the fractal.
    //    3. the hue SPREAD       — the per-layer / per-depth drift each module adds
    //                              on top of the base hue. Synesthesia spread ~0.15
    //                              of the wheel (≈1.8 semitones!) and Geometry 0.05,
    //                              so the dominant colour landed on a different note
    //                              in each module even though the base hue matched.
    //
    //  Anything that wants a subtle depth/layer gradient must scale it by
    //  kToneHueSpread, so the gradient stays visible but the DOMINANT hue is the
    //  note itself in every module.
    // ─────────────────────────────────────────────────────────────────────────

    /** Core saturation for a tone-derived colour. Reference = Synesthesia (full
        primary). Chladni matches this instead of its old 0.80 pastel. */
    inline constexpr float kToneSaturation = 1.0f;

    /** Maximum hue excursion (in wheel units) any module may add on top of the
        note's hue for depth/layer shading. 0.02 ≈ a quarter semitone: still a
        visible gradient, far too small to read as a different note. */
    inline constexpr float kToneHueSpread = 0.02f;

    // ── 'Tone smooth' → hue chase rate ───────────────────────────────────────
    //  The per-frame fraction of the remaining distance the hue moves toward a new
    //  note. Bounded at both ends for real reasons: at 0 the colour would freeze on
    //  the first note ever detected, and above ~0.5 a note change lands inside two
    //  or three frames and reads as a hard cut rather than a move.
    inline constexpr float kToneRateFast = 0.5f;    // slider 0 = least smoothing
    inline constexpr float kToneRateSlow = 0.02f;   // slider 1 = most smoothing

    /** 'Tone smooth' slider (0..1, HIGH = heavier smoothing) → chase rate.

        GEOMETRIC, not linear, and mapped across the WHOLE slider — both on purpose.

        The modules used to write `jlimit (0.02, 0.5, 1 - slider)`. Clamping AFTER
        the inversion threw away half the control: every position from 0 to 0.5
        inverted to something ≥ 0.5 and was pinned to the ceiling, so the entire
        lower half of the travel did exactly the same thing and all the real range
        was squeezed into 0.5..0.98. Mapping onto the endpoints instead of clipping
        against them is what gives the slider its lower half back.

        Geometric because the thing being felt is a time constant, and that goes as
        1/rate: linear in RATE would put almost the whole perceived change in the
        last few percent of the travel. Here each equal step multiplies the settling
        time by a constant factor — 0.5 → 0.1 → 0.02 is roughly 5, 22 and 114 frames,
        evenly spaced to the eye.

        Defaults are chosen per module so this curve reproduces the rate that module
        shipped with; see kToneSmooth in AlterState. */
    inline float toneSmoothToRate (float smooth01) noexcept
    {
        smooth01 = juce::jlimit (0.0f, 1.0f, smooth01);
        return kToneRateFast * std::pow (kToneRateSlow / kToneRateFast, smooth01);
    }

    /** Pitch class (0..11) → hue 0..1 on the shared 12-step wheel.
        C = red, then DESCENDING the wheel: C#=rose, D=magenta, D#=violet, E=blue,
        F=azure, F#=cyan, G=spring, G#=green, A=chartreuse, A#=yellow, B=orange.

        THE DESCENDING DIRECTION IS DELIBERATE — do not "correct" it to ascending.
        This is the mapping Synesthesia has always shipped, and it is the reference
        the whole app is tuned around. It originally came from its shader's hsv2rgb
        having the green and blue channel offsets swapped, which mirrored the wheel
        about red; that mirror is now expressed HERE, as an explicit property of the
        tone→hue mapping, instead of being hidden in a colour-space conversion.

        Putting it here rather than in hsv2rgb matters: the mirror must apply ONLY to
        tone-derived colour. The manual colour picker feeds a real hue straight to the
        shader, and while the mirror lived in hsv2rgb, picking green drew blue. */
    /** @param twist  'Mirror tone color': reverses the direction of travel around
                      the wheel, so the notes run UPWARD from red instead of down
                      (C#=orange, D=yellow, E=green, G#=blue …). Red and cyan are the
                      two fixed points, so C and F# look the same either way. Purely a
                      taste control — it does not change WHICH note maps to a distinct
                      colour, only which colour each note gets. Per module, so two
                      modules can deliberately run opposite palettes off one tone. */
    inline float pitchClassHue (int pitchClass, bool twist = false) noexcept
    {
        const int pc = ((pitchClass % 12) + 12) % 12;
        return (float) (twist ? pc : ((12 - pc) % 12)) / 12.0f;
    }

    /** Hz → hue 0..1 on the shared wheel. */
    inline float hueFromHz (float hz, bool twist = false) noexcept
    {
        if (hz <= 0.0f) return 0.0f;
        const int midi = (int) std::lround (69.0 + 12.0 * std::log2 ((double) hz / 440.0));
        return pitchClassHue (midi, twist);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  ToneHueTracker — the whole "colour by tone" pipeline in one object.
    //
    //  Synesthesia, Chladni and Geometry each grew their OWN copy of this: the
    //  MIDI override, the 65 Hz–2 kHz detector, the circular hue lerp. They agree
    //  today only because all three were hand-matched against each other, which is
    //  exactly the kind of agreement that rots the moment someone touches one of
    //  them. Every module added AFTER that clean-up uses this instead, so the
    //  contract at the top of this file is enforced by there being one
    //  implementation rather than by everyone remembering to copy the same one.
    //
    //  The three originals are deliberately NOT retrofitted onto it: each carries a
    //  quirk the others do not (Synesthesia scales the hue by 12 for its shader,
    //  Chladni runs a fixed 0.15 lerp and drives it from its own analysis pass), and
    //  rewriting three shipped, visually-tuned modules to save duplication would
    //  risk changing the reference look for no user-visible gain. New modules are
    //  matched to them THROUGH this class.
    //
    //  Cost: one getLastFft copy + one linear scan of ~1900 Hz worth of bins per
    //  update, and only while tone colour is actually switched on. Call update()
    //  once per frame from the module's existing analysis step — never per pixel,
    //  per bar or per particle. Read the result with hue()/colour(), which are pure.
    // ─────────────────────────────────────────────────────────────────────────
    class ToneHueTracker
    {
    public:
        /** @param src            the module's audio source
            @param twist          'Mirror tone color' — reverses the wheel direction
            @param toneSmooth01   the 'Tone smooth' SLIDER value, 0..1, exactly as the
                                  user set it: HIGH = heavier smoothing. Not a rate —
                                  toneSmoothToRate does that conversion, so the curve
                                  and its endpoints live in ONE place rather than
                                  being re-derived (and re-clamped differently) in
                                  every module.
            @param subBinInterp   parabolic sub-bin refinement of the peak

            No-ops (holding the current hue) when there is no usable pitch: silence,
            a source with no FFT yet, or MIDI present on the track with nothing held.
            Holding is the right failure mode — resetting to red on every gap between
            notes is what a naive version does and it strobes. */
        void update (const IAudioSource& src, bool twist, float toneSmooth01,
                     bool subBinInterp = true)
        {
            const float target = detect (src, twist, subBinInterp);
            if (target < 0.0f) return;

            if (! seeded) { smoothed = target; seeded = true; return; }

            // Circular lerp: 'd -= round(d)' takes the SHORT way round the wheel, so
            // B→C crosses the 1.0/0.0 seam directly instead of sweeping backwards
            // through the entire spectrum.
            float d = target - smoothed;
            d -= std::round (d);
            smoothed += d * toneSmoothToRate (toneSmooth01);
            smoothed -= std::floor (smoothed);
        }

        /** Current smoothed hue, 0..1 on the shared wheel. */
        float hue() const noexcept { return smoothed; }

        /** The note's colour at the shared reference saturation. `value` lets a
            module dim it (depth shading, level, falloff) without drifting off the
            note — scale the VALUE, never the hue. */
        juce::Colour colour (float value = 1.0f) const noexcept
        {
            return juce::Colour::fromHSV (smoothed, kToneSaturation,
                                          juce::jlimit (0.0f, 1.0f, value), 1.0f);
        }

        /** Drop the smoothing history, so the next update() snaps rather than
            slides. Use when the module was hidden/frozen and the held hue is stale. */
        void reset() noexcept { seeded = false; }

        /** Has a pitch EVER been detected since the last reset?

            Ask before using colour(), and fall back to the module's manual colour
            when this is false. Until the first successful detect() the hue is 0,
            which is not "no colour" — it is PURE RED, and a module that quietly
            paints itself red because it never received the data it needs looks
            broken in a way that points at the wrong thing entirely. (That is not
            hypothetical: a module in tone mode that forgets to ask its source for
            the FFT stream gets exactly zero bins forever.)

            It also covers the honest cases — the first frames after start-up, or a
            source that is silent — where the manual colour is the better answer
            than a note nobody played. */
        bool hasTone() const noexcept { return seeded; }

    private:
        /** @return hue 0..1, or -1 when there is nothing to update from. */
        float detect (const IAudioSource& src, bool twist, bool subBinInterp)
        {
            // Exact held MIDI beats every estimator — when the Creator plugin is
            // feeding 'M' packets we know the note instead of guessing it.
            if (src.getMidiNotes (midiScratch))
            {
                if (midiScratch.empty()) return -1.0f;      // MIDI on, nothing held
                const IAudioSource::MidiNote* top = &midiScratch.front();
                for (const auto& n : midiScratch)
                    if (n.velocity > top->velocity) top = &n;
                return hueFromHz (440.0f * std::pow (2.0f, ((float) top->note - 69.0f) / 12.0f),
                                  twist);
            }

            const int bins = src.getLastFft (fftScratch);
            if (bins <= 0) return -1.0f;

            // 65 Hz–2 kHz, flat magnitude. This band is Synesthesia's and it is the
            // band where the FUNDAMENTAL wins: search wider (or apply perceptual
            // weighting, as Chladni's physics detector does) and a 3rd or 5th
            // harmonic starts taking the peak, which lands on a different pitch
            // class and colours a C as a G.
            const double sr = src.getSampleRate();          // real device rate → correct pitch
            const float  bw = binWidthHz (bins, sr);
            const int    lo = juce::jmax (1, (int) std::round (65.0f   / bw));
            const int    hi =               (int) std::round (2000.0f / bw);

            const float freq = dominantFrequency (fftScratch, lo, hi, 0.01f, subBinInterp, sr);
            if (freq < 20.0f) return -1.0f;
            return hueFromHz (freq, twist);
        }

        float smoothed { 0.0f };
        bool  seeded   { false };

        // Kept as members so a per-frame update does not allocate.
        std::vector<float>                 fftScratch;
        std::vector<IAudioSource::MidiNote> midiScratch;
    };
}
