/*
  ==============================================================================

    InfoWindowSpectrum.h
    Spectrum Analyzer – Educational Guide
  ==============================================================================
*/

#pragma once
#include "InfoWindowAudioMeters.h"

class SpectrumInfoWindow : public ModuleInfoWindow
{
public:
    SpectrumInfoWindow()
        : ModuleInfoWindow ("Spectrum Analyzer - Educational Guide")
    {
        auto* content = new ContentPanel();
        auto* viewport = new juce::Viewport();
        viewport->setViewedComponent (content, true);
        viewport->setScrollBarsShown (true, false);
        setContentOwned (viewport, true);
        setSize (640, 920);
        centreWithSize (getWidth(), getHeight());
    }

    void resized() override
    {
        ModuleInfoWindow::resized();
        if (auto* vp = dynamic_cast<juce::Viewport*> (getContentComponent()))
            if (auto* content = vp->getViewedComponent())
                content->setSize (vp->getMaximumVisibleWidth(), content->getHeight());
    }

private:
    class ContentPanel : public juce::Component
    {
    public:
        ContentPanel() { setSize (640, 6600); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xFF1E1E1E));
            auto area = getLocalBounds().reduced (20);

            // TITLE
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (24.0f, juce::Font::bold));
            g.drawText ("Spectrum Analyzer", area.removeFromTop (40), juce::Justification::centred);
            area.removeFromTop (4);
            g.setColour (juce::Colour (0xFF888888));
            g.setFont (juce::Font (12.0f));
            g.drawText ("Real-time FFT visualization with psychoacoustic correction",
                        area.removeFromTop (20), juce::Justification::centred);
            area.removeFromTop (16);

            // SECTION: AXES
            drawSectionTitle (g, area, "SPECTRUM AXES");
            drawBody (g, area,
                "X axis  (horizontal) - frequency on a logarithmic scale\n"
                "  Range: 20 Hz - 20,000 Hz (full human hearing spectrum)\n"
                "  Log scale: each octave occupies equal width (matches human perception)\n"
                "  Grid: 20 / 50 / 100 / 200 / 500 / 1k / 2k / 5k / 10k / 20k Hz\n\n"
                "Y axis  (vertical) - level in dBFS (decibels Full Scale)\n"
                "  0 dBFS  = top of the digital scale = maximum level before clipping\n"
                "  -6 dBFS = half the amplitude (6 dB reduction = 2x smaller voltage)\n"
                "  -18 dBFS = typical working level during mixing\n"
                "  -90 dBFS = display floor (typical DAW noise floor)\n\n"
                "  Each point on the curve = FFT bin magnitude converted to dBFS\n"
                "  and normalized: 0.0 = -90 dBFS (silence), 1.0 = 0 dBFS (maximum)");
            area.removeFromTop (8);

            // SECTION: FFT / BIN / MAGNITUDE
            drawSectionTitle (g, area, "FFT, BINS AND MAGNITUDE");
            drawBody (g, area,
                "FFT (Fast Fourier Transform)\n"
                "  Mathematical transform: time-domain signal (waveform) -> frequency content\n"
                "  The plugin sends the result as a UDP packet with prefix ALTF: array of floats 0..1\n"
                "  Each float is the normalized magnitude of one frequency bin.\n\n"
                "Bin\n"
                "  FFT divides the spectrum into N evenly spaced bands (bins)\n"
                "  Bin width = (Sample Rate / 2) / number of bins\n"
                "  E.g. at SR=48kHz and N=2048: bin width = 24000 / 2048 = ~11.7 Hz/bin\n"
                "  Smaller bins = higher frequency resolution (better detail)\n"
                "  Larger FFT window = narrower bins, but slower time response (latency)\n\n"
                "Magnitude (0..1)\n"
                "  Bin value = signal strength in that frequency band\n"
                "  0.0 = absolute silence (below noise floor)\n"
                "  1.0 = full level = 0 dBFS\n"
                "  Conversion: dBFS = floor + value * (0 - floor)\n"
                "  Example: value=0.8, floor=-90 dB: dBFS = -90 + 0.8*90 = -18 dBFS");
            area.removeFromTop (8);

            // SECTION: EXAMPLE
            drawSectionTitle (g, area, "EXAMPLE - TYPICAL MUSIC SPECTRUM");
            drawSpectrumExample (g, area.removeFromTop (90));
            area.removeFromTop (8);

            // SECTION: SMOOTHING
            drawSectionTitle (g, area, "SMOOTHING (EMA - EXPONENTIAL MOVING AVERAGE)");
            drawBody (g, area,
                "The spectrum uses dual EMA: input + output (attack/release)\n\n"
                "Input EMA (Input alpha)\n"
                "  Primary smoothing of incoming FFT signal\n"
                "  Reduces flickering caused by FFT instability\n\n"
                "Attack alpha\n"
                "  Response speed when level increases (fast rise)\n"
                "  High value: fast response to transients (kick, snare)\n\n"
                "Release alpha\n"
                "  Decay speed after signal ends (slow fall - SPAN-like)\n"
                "  Low value: longer tail, visually smoother\n\n"
                "Smooth slider (0..1):\n"
                "  0 = instant response (raw FFT, may appear nervous)\n"
                "  0.5 = balanced - recommended for monitoring\n"
                "  1.0 = maximum smoothing (fluid, trend-like)");
            area.removeFromTop (8);

            // SECTION: INTERPOLATION
            drawSectionTitle (g, area, "CURVE INTERPOLATION");
            drawBody (g, area,
                "Resampling: the FFT bins are remapped to log-X render points. For each\n"
                "render point the frequency is calculated and the corresponding bin is\n"
                "found with interpolation between neighbors.\n\n"
                "Visual mode  ->  Catmull-Rom interpolation\n"
                "  Cubic curve passing through 4 control points\n"
                "  Result: smooth, aesthetic curve\n"
                "  Note: may slightly overshoot at sharp transitions\n"
                "  Best for: visualization, live performance, VJing\n\n"
                "Measurement mode  ->  Linear interpolation\n"
                "  Direct transition between two adjacent bins\n"
                "  No overshoot - every value is real\n"
                "  Best for: analysis, mastering, EQ decisions");
            area.removeFromTop (8);

            // SECTION: GLOBAL FFT RESOLUTION + CONSTANT-Q
            drawSectionTitle (g, area, "GLOBAL FFT RESOLUTION & CONSTANT-Q");
            drawBody (g, area,
                "The display always uses the FULL resolution the source provides. The\n"
                "GLOBAL FFT Resolution setting (controller footer) selects it for every\n"
                "module and connected plugin at once:\n\n"
                "2048 bins (4096-pt FFT)   ->  fast, light, ~11.7 Hz/bin @ 48 kHz\n"
                "4096 bins (8192-pt FFT)   ->  finer detail, ~5.9 Hz/bin\n"
                "8192 bins (16384-pt FFT)  ->  maximum detail, ~2.9 Hz/bin, more latency\n\n"
                "CONSTANT-Q (same menu) replaces the single FFT with a multi-resolution\n"
                "analysis: a LONG window for bass, MEDIUM for mids, SHORT for highs.\n"
                "Every log-spaced band is read from the analysis that actually resolves\n"
                "it, so each semitone from sub-bass up to 20 kHz is readable - the low\n"
                "end stops being one wide hill, and the highs stay snappy.\n\n"
                "Level calibration: TONES read the same dB in every band (the high band\n"
                "is not attenuated), and each log band aggregates the peak of all FFT\n"
                "bins it covers - a high tone can never fall between two samples and\n"
                "read low. In stereo + CQ both channels use the same rule and the curve\n"
                "is blended with interpolation, so it stays continuous (no staircase).\n"
                "  Use case: bass tuning, 808 pitch, chord voicings, decisions < 200 Hz.\n"
                "  Trade-off: long bass windows respond more slowly (physics, not a bug).");
            area.removeFromTop (8);

            // SECTION: HARMONICS
            drawSectionTitle (g, area, "HARMONIC MARKING (HARMONICS OVERLAY)");
            drawBody (g, area,
                "The spectrum automatically detects peak frequencies and calculates harmonics.\n"
                "Harmonic = integer multiple of the fundamental frequency (f0):\n"
                "  1st harmonic = f0 (fundamental)\n"
                "  2nd harmonic = 2 * f0 (octave)\n"
                "  3rd harmonic = 3 * f0 (fifth + octave)\n"
                "  4th harmonic = 4 * f0 (2 octaves) ... up to the 12th harmonic\n\n"
                "Visualization:\n"
                "  Fundamental frequency: thin, subdued column in the panel color\n"
                "  Harmonics: highlighted columns with increased color saturation\n"
                "  Peak neighborhood: subtle gradient fade\n\n"
                "IMPORTANT: Harmonic marking does NOT change curve values!\n"
                "  The curve always remains accurate - only the color/alpha of columns changes.\n"
                "  Use this to assess the harmonic structure of a sound.");
            area.removeFromTop (8);

            // SECTION: PEAK READOUT
            drawSectionTitle (g, area, "PEAK READOUT");
            drawBody (g, area,
                "The strongest frequency of the current frame is shown in the overlay\n"
                "label (top-right corner, next to bins/mode/ALTF/s) as frequency + dBFS.\n"
                "The label is always horizontal and readable regardless of module rotation.\n\n"
                "Use cases:\n"
                "  Identify resonances and problematic bands at a glance\n"
                "  Tune kicks/808s: read the exact dominant frequency");
            area.removeFromTop (8);

            // SECTION: PSYCHOACOUSTICS
            drawSectionTitle (g, area, "PSYCHOACOUSTIC CURVES");
            drawBody (g, area,
                "Flat (off)\n"
                "  Raw FFT values with no correction. Shows physical energy of the signal.\n"
                "  Use case: technical analysis, debugging, sound design\n\n"
                "A-weight\n"
                "  IEC 61672 standard. Corrects the spectrum for ear sensitivity at medium\n"
                "  loudness. Attenuates low (<100 Hz) and high (>10k Hz) frequencies\n"
                "  where the ear is less sensitive.\n"
                "  Correction: -20 dB at 20 Hz, 0 dB at 1 kHz, -10 dB at 20 kHz\n"
                "  Use case: noise measurement, broadcast, LUFS-like monitoring\n\n"
                "ISO 226 (Fletcher-Munson)\n"
                "  More accurate than A-weight. Accounts for phon level (loudness).\n"
                "  Different phon levels = different curve shapes:\n"
                "    20 phon (quiet): strong attenuation of low and high frequencies\n"
                "    60 phon (medium): moderate correction - closest to human perception\n"
                "    80 phon (loud): relatively flatter curve, bass more proportionate\n"
                "  Reference: 1 kHz is always the normalization point (correction = 0 dB)\n"
                "  Use case: mastering, mixing for various listening conditions\n\n"
                "aStrength = 1.0: Full correction strength (standard). Curve follows the norm exactly.");
            area.removeFromTop (8);

            // SECTION: TRACK REQUIREMENTS
            drawSectionTitle (g, area, "TRACK REQUIREMENTS - SPECTRAL TARGETS");
            drawBody (g, area,
                "STREAMING (Spotify, Apple Music, YouTube)\n"
                "  Bass (20-200 Hz): -20 to -12 dBFS. Kickdrum peak max -6 dBFS.\n"
                "  Mids (200 Hz-2 kHz): -18 to -6 dBFS. Main energy of melody/vocals.\n"
                "  Highs (2k-10k Hz): -24 to -12 dBFS. Presence, clarity, not harsh.\n"
                "  Air (10k-20k Hz): -36 to -18 dBFS. Subtle airiness, not sibilant.\n\n"
                "MASTERING TARGETS (Flat mode)\n"
                "  Spectrum should be relatively even - no extreme peaks.\n"
                "  Tilt: slight downward slope from low to high frequencies (natural)\n"
                "  Red peaks: resonance - needs EQ or notch filter\n"
                "  Empty bands: missing content - add saturation or layers\n\n"
                "BROADCAST (TV, Radio) - EBU R128\n"
                "  Overall spectrum at A-weight: -23 LUFS target\n"
                "  Low frequencies: HPF below 80 Hz (mono compatibility)\n"
                "  Dialog/vocal region (1k-4k Hz): dominant, intelligible\n\n"
                "DANCE / EDM\n"
                "  Kick (50-80 Hz): strong peak, -6 dBFS minimum\n"
                "  Sub bass (20-50 Hz): energy but below -12 dBFS (mono compatibility!)\n"
                "  High frequencies (8k-16k Hz): liveliness, air, control sibilance\n\n"
                "COMMON ISSUES\n"
                "  Sibilance (6k-9k Hz high peaks): de-esser on vocals/overheads\n"
                "  Sub boom (30-60 Hz): bass trap in studio, HPF on non-bass instruments\n"
                "  Boxiness (200-400 Hz): typical vocal/guitar resonance - EQ cut\n"
                "  Mud (80-250 Hz): too many layers without EQ - side-chain, hi-shelf cut");
            area.removeFromTop (8);

            // SECTION: MEASUREMENT MODE
            drawSectionTitle (g, area, "MEASUREMENT MODE (SPAN-LIKE)");
            drawBody (g, area,
                "Measurement mode switches the Spectrum into an analytical display:\n\n"
                "1. Power-average downsample (instead of maximum)\n"
                "   Bin groups are averaged by energy (power), not maximum.\n"
                "   Power = 10^(dB/10), average, convert back to dB.\n"
                "   Result: more faithful to signal energy, fewer artifacts.\n\n"
                "2. Linear interpolation (instead of Catmull-Rom)\n"
                "   No overshoot - every value is real.\n"
                "   Allows precise dBFS readout.\n\n"
                "Recommendation:\n"
                "  Visual mode: live performance, real-time monitoring\n"
                "  Measurement mode: mastering, export check, referencing");
            area.removeFromTop (8);

            // SECTION: STEREO / PEAK HOLD / REFERENCES
            drawSectionTitle (g, area, "STEREO MODE, PEAK HOLD & REFERENCE CURVES");
            drawBody (g, area,
                "STEREO (L+R): overlays the left and right spectra. Use it to catch\n"
                "one-sided hi-hats, asymmetric low mids or an over-wide top end that\n"
                "turns to soup in mono. R COLOUR picks complementary or analogous tint.\n\n"
                "PEAK HOLD: a slowly decaying maximum curve over the live spectrum -\n"
                "the memory line. Fast peaks vanish before your brain registers them;\n"
                "peak hold shows what happened recently. Use for resonance hunting,\n"
                "kick tuning, harsh consonants and checking a drop at max energy.\n\n"
                "REFERENCE CURVES (EDM / Bass / House / Hip-Hop / Pop / Rock): broad\n"
                "genre silhouettes, not standards. Use the dashed curve to ask better\n"
                "questions (is my bass far above typical? presence region dead?), not\n"
                "as a law to obey. Compare, then decide by ear.\n\n"
                "RBW note: narrower FFT bins hold less broadband energy (-3 dB per\n"
                "halving of bin width), so the SAME track sits lower at higher Max-bins\n"
                "settings. The reference curve auto-shifts with the resolution (anchored\n"
                "at 2048 bins @ 48 kHz), so your distance to the target reads the same\n"
                "at every setting. Pure tones are unaffected by bin width.");
            area.removeFromTop (16);

            // FOOTER
            g.setColour (juce::Colour (0xFF555555));
            g.drawHorizontalLine (area.getY(), (float) area.getX(), (float) area.getRight());
            area.removeFromTop (8);
            g.setFont (juce::Font (11.0f));
            g.setColour (juce::Colour (0xFF777777));
            g.drawText ("Alter Spectrum Analyzer  -  ISO 226:2003  -  IEC 61672 A-weight  -  FFT-based",
                        area.removeFromTop (18), juce::Justification::centred);
        }

    private:
        static void drawSectionTitle (juce::Graphics& g, juce::Rectangle<int>& area, const juce::String& title)
        {
            if (area.getHeight() < 32) return;
            area.removeFromTop (6);
            g.setFont (juce::Font (15.0f, juce::Font::bold));
            g.setColour (juce::Colour (0xFF00BFFF));
            g.drawText (title, area.removeFromTop (26), juce::Justification::bottomLeft);
            g.setColour (juce::Colour (0xFF00BFFF).withAlpha (0.35f));
            g.drawHorizontalLine (area.getY(), (float) area.getX(), (float) area.getRight());
            area.removeFromTop (6);
        }

        static void drawBody (juce::Graphics& g, juce::Rectangle<int>& area, const juce::String& text)
        {
            if (area.getHeight() < 10) return;
            g.setFont (juce::Font (13.0f));
            g.setColour (juce::Colours::lightgrey);
            int newlines = 0;
            for (int i = 0; i < text.length(); ++i)
                if (text[i] == '\n') ++newlines;
            const int estLines = juce::jmax (1, (int)(text.length() / 60) + newlines + 1);
            const int estH     = juce::jmin (area.getHeight(), estLines * 16 + 8);
            if (estH <= 0) return;
            g.drawFittedText (text, area.removeFromTop (estH), juce::Justification::topLeft, estLines + 2);
        }

        static void drawSpectrumExample (juce::Graphics& g, juce::Rectangle<int> area)
        {
            g.setColour (juce::Colour (0xFF2A2A2A));
            g.fillRect (area);
            g.setColour (juce::Colours::dimgrey.withAlpha (0.4f));
            g.drawRect (area);

            const int W = area.getWidth();
            const int H = area.getHeight();
            const float bx = (float) area.getX();
            const float by = (float) area.getY();

            struct Band { float freqNorm; float mag; };
            const Band bands[] = {
                {0.00f,0.35f},{0.04f,0.72f},{0.09f,0.85f},{0.14f,0.78f},
                {0.20f,0.65f},{0.28f,0.70f},{0.36f,0.75f},{0.44f,0.68f},
                {0.52f,0.60f},{0.60f,0.55f},{0.68f,0.45f},{0.76f,0.38f},
                {0.84f,0.30f},{0.90f,0.22f},{0.95f,0.15f},{1.00f,0.08f}
            };
            const int N = 16;

            juce::Path path;
            for (int i = 0; i < N; ++i)
            {
                const float x = bx + bands[i].freqNorm * (float)W;
                const float y = by + (float)H * (1.0f - bands[i].mag);
                if (i == 0) path.startNewSubPath (x, y);
                else        path.lineTo (x, y);
            }
            path.lineTo (bx + (float)W, by + (float)H);
            path.lineTo (bx, by + (float)H);
            path.closeSubPath();

            juce::ColourGradient grad (juce::Colour(0xFF00BFFF).withAlpha(0.35f), bx, by,
                                       juce::Colour(0xFF00BFFF).withAlpha(0.05f), bx, by+(float)H, false);
            g.setGradientFill (grad);
            g.fillPath (path);

            g.setColour (juce::Colour (0xFF00BFFF).withAlpha (0.85f));
            juce::Path outline;
            for (int i = 0; i < N; ++i)
            {
                const float x = bx + bands[i].freqNorm * (float)W;
                const float y = by + (float)H * (1.0f - bands[i].mag);
                if (i == 0) outline.startNewSubPath (x, y);
                else        outline.lineTo (x, y);
            }
            g.strokePath (outline, juce::PathStrokeType (1.5f));

            g.setFont (juce::Font (10.0f));
            g.setColour (juce::Colours::grey);
            const char* labels[] = { "20", "100", "1k", "10k", "20k" };
            const float lpos[]   = { 0.0f, 0.14f, 0.44f, 0.84f, 1.0f };
            for (int i = 0; i < 5; ++i)
                g.drawText (labels[i], (int)(bx + lpos[i]*(float)W) + 2, (int)(by+H)-14, 30, 12, juce::Justification::left);

            g.setColour (juce::Colours::grey);
            g.drawText ("0 dBFS",   area.getX(), area.getY(),        50, 12, juce::Justification::left);
            g.drawText ("-90 dBFS", area.getX(), area.getBottom()-12, 60, 12, juce::Justification::left);
        }
    };
};
