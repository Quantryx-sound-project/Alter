/*
  ==============================================================================

    ToneAnalyzer.h
    Realtime note + chord detection from the FFT stream.

    - Peak picking with parabolic interpolation (sub-bin accuracy)
    - Harmonic suppression (overtones of a detected fundamental are skipped)
    - Pitch-class energy ring with attack/release smoothing
    - Chord matching against common templates (maj, min, 7ths, sus, dim, aug…)

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include "AudioSourceInterface.h"
#include "AlterTheme.h"
#include "PitchUtils.h"
#include <cmath>
#include <vector>
#include <atomic>
#include <array>

class ToneAnalyzerMeter : public ThemedBackground, public juce::Component, private juce::Timer
{
public:
    explicit ToneAnalyzerMeter (IAudioSource& src) : audioSource (src)
    {
        setOpaque (true);
        startTimerHz (30);
    }

    ~ToneAnalyzerMeter() override { stopTimer(); }

    /** 0..1 → detection sensitivity. Controls BOTH how far a peak must rise above the
        (median) noise floor AND the absolute quiet-gate, so the knob has a clear,
        audible effect across its whole range. */
    void setSensitivity (float s01)
    {
        const float s = juce::jlimit (0.0f, 1.0f, s01);
        // 0 → strict: +26 dB over floor & a high -58 dB quiet-gate
        // 1 → hot:    +6  dB over floor & a low  -82 dB quiet-gate (catches soft notes)
        peakThresholdDb = juce::jmap (s, 26.0f,  6.0f);
        absGateDb       = juce::jmap (s, -58.0f, -82.0f);
    }

    void setAccentColour (juce::Colour c) { accent = c; repaint(); }

    // ── Colour by tone ────────────────────────────────────────────────────────
    //  Every lit thing in this module — the pitch-class bars, the corner brackets,
    //  the readout glow and the tuner needle — is drawn from ONE accent colour, so
    //  tone colour is a single swap at the source and needs no per-element work.
    //
    //  IT DELIBERATELY DOES NOT USE THIS MODULE'S OWN DETECTED NOTE, even though it
    //  has the best note detector in the app. The shared contract in PitchUtils is
    //  that the same semitone reads as the same colour in EVERY module at once, and
    //  this analyser's detector is tuned differently (harmonic suppression, chord
    //  weighting, a wider band): on a chord it would settle on a root that
    //  Synesthesia and Chladni, both watching the 65 Hz–2 kHz peak, do not agree
    //  with — and a HUD whose modules disagree about what colour the music is looks
    //  broken, not more accurate. So it tracks the shared hue like everyone else,
    //  and keeps its superior detection for the thing it is actually for: the names
    //  printed in the readout.
    void setColourByTone (bool b)
    {
        if (colourByTone == b) return;
        colourByTone = b;
        // The held hue is as old as the last tone-mode frame; snap to what is
        // sounding now instead of sliding across the wheel from a dead note.
        if (b) toneHue.reset();
        repaint();
    }

    /** 'Mirror tone color' — reverses the tone→hue wheel (see PitchUtils). */
    void setToneTwist (bool b) { toneTwist = b; repaint(); }

    /** 'Tone smooth' 0..1: HIGH = heavier smoothing. Stored RAW —
        PitchUtils::toneSmoothToRate owns the curve. */
    void setToneSmooth (float s01) { toneSmooth = juce::jlimit (0.0f, 1.0f, s01); }

    /** Tuner mode: accurate monophonic pitch (McLeod/MPM) + needle for tuning. */
    void setTunerMode (bool on) { tunerMode.store (on); }

    /** The currently detected note/chord (live) for the host overlay. */
    juce::String getDetectedText() const
    {
        const juce::ScopedLock sl (resultLock);
        return chordName;
    }

    /** true = the analyzer is currently driven by exact MIDI notes (else FFT). */
    bool isMidiSource() const
    {
        const juce::ScopedLock sl (resultLock);
        return midiActive;
    }

    // ── Component ────────────────────────────────────────────────────────────
    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds();
        paintModuleBackground (g, b.toFloat());

        const int W = b.getWidth(), H = b.getHeight();
        if (W < 60 || H < 40) return;

        const float fs = juce::jlimit (9.0f, 13.0f, (float) juce::jmin (W, H) * 0.05f);

        // snapshot all shared state once, under the lock
        std::array<float, 12> pce {};
        bool midiMode;
        juce::String chord, tName;
        std::vector<DisplayNote> dn;
        float tCents = 0.0f, tHz = 0.0f;
        bool  tHas = false;
        const bool tuner = tunerMode.load();
        {
            const juce::ScopedLock sl (resultLock);
            for (int i = 0; i < 12; ++i) pce[(size_t) i] = pcEnergy[(size_t) i];
            midiMode = midiActive;
            chord    = chordName;
            dn       = displayNotes;
            tName = tunerName; tCents = tunerCents; tHz = tunerHz; tHas = tunerHasPitch;
        }

        // ── STATIC side bars: the frame is sized ONLY by the window; the audio
        //    moves the coloured fill, never the structure. 6 pitch classes/side. ──
        // jlimit with lower > upper is not defined, and that is exactly what this
        // was below 144 px wide: the floor of 48 outranked the ceiling of W/3. So
        // the ceiling wins and the floor only applies where there is room for it.
        const int sideW = juce::jmin (W / 3, juce::jmax (48, (int) (W * 0.24f)));
        auto leftR  = b.removeFromLeft  (sideW);
        auto rightR = b.removeFromRight (sideW);
        auto centre = b;

        static const char* pcN[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        auto drawSide = [&] (juce::Rectangle<int> region, int lo)
        {
            const float cw = (float) region.getWidth() / 6.0f;
            for (int k = 0; k < 6; ++k)
                drawCyberBar (g, juce::Rectangle<float> ((float) region.getX() + cw * (float) k,
                                                         (float) region.getY(), cw, (float) region.getHeight())
                                     .reduced (3.0f, 6.0f),
                              pcN[lo + k], juce::jlimit (0.0f, 1.0f, pce[(size_t)(lo + k)]), fs);
        };
        drawSide (leftR, 0);
        drawSide (rightR, 6);

        // cyber corner brackets framing the centre readout zone (HUD look)
        {
            auto cz = centre.toFloat().reduced (3.0f, 6.0f);
            const float bl = juce::jmin (18.0f, cz.getWidth() * 0.16f), th = 1.4f;
            g.setColour (activeAccent().withAlpha (0.5f));
            g.fillRect (cz.getX(),           cz.getY(),            bl, th);
            g.fillRect (cz.getX(),           cz.getY(),            th, bl);
            g.fillRect (cz.getRight() - bl,  cz.getY(),            bl, th);
            g.fillRect (cz.getRight() - th,  cz.getY(),            th, bl);
            g.fillRect (cz.getX(),           cz.getBottom() - th,  bl, th);
            g.fillRect (cz.getX(),           cz.getBottom() - bl,  th, bl);
            g.fillRect (cz.getRight() - bl,  cz.getBottom() - th,  bl, th);
            g.fillRect (cz.getRight() - th,  cz.getBottom() - bl,  th, bl);
        }

        // ── CENTRE: chord / tuner / notes + cents — and nothing else ──
        drawReadout (g, centre.reduced (8, 6), tuner, midiMode, chord, dn, tName, tCents, tHz, tHas, fs);

        // (pitch-class bars are drawn on the sides above; MIDI/FFT source is in
        //  the module info label top-right — no badge here)
    }

private:
    // ── Analysis ─────────────────────────────────────────────────────────────
    struct DetectedNote
    {
        float freq, db;
        int   midi;
    };

    // One row of the readout: note name + tuning offset. hasCents=false in MIDI
    // mode (notes are exact) → shown as a plain key. inTune = |cents| < 5.
    struct DisplayNote
    {
        juce::String name;
        float        cents   = 0.0f;
        bool         inTune  = true;
        bool         hasCents = false;
    };

    // Static vertical pitch-class bar. The frame/segment grid is fixed to the
    // window; only the coloured segmented fill rises/falls with the tone energy.
    void drawCyberBar (juce::Graphics& g, juce::Rectangle<float> r, const char* label, float e, float fs)
    {
        // A BAR TOO SMALL TO HAVE INSIDES DRAWS NOTHING.
        //
        // Everything below insets the rectangle by fixed pixel amounts — the grid
        // lines take 4 off the width, the segments 6, the label 18 off the height.
        // Those were written for a bar with room to spare, and they go NEGATIVE on
        // a narrow one: a side region is 48 px at its narrowest, split six ways is
        // 8, less the 6 that reduced() already took is 2 — and `getWidth() - 4`
        // is then -2. juce::Graphics asserts on a negative size rather than
        // silently drawing nothing, which is how squeezing this module by adding
        // another one to the HUD took the app down.
        //
        // Bailing out here is the honest answer: at that size there is no picture
        // to draw, only arithmetic to get wrong.
        if (r.getWidth() < 10.0f || r.getHeight() < 24.0f)
            return;

        auto labelR = r.removeFromBottom (15.0f);
        r.removeFromBottom (3.0f);
        const bool  active = e > 0.02f;
        const auto  neon   = active ? activeAccent() : AlterTheme::textDim;

        // ── STATIC frame: chamfered-top outline (angular), sized only by window ──
        const float ch = juce::jmin (7.0f, r.getWidth() * 0.34f);
        juce::Path frame;
        frame.startNewSubPath (r.getX(), r.getBottom());
        frame.lineTo (r.getX(),          r.getY() + ch);
        frame.lineTo (r.getX() + ch,     r.getY());
        frame.lineTo (r.getRight() - ch, r.getY());
        frame.lineTo (r.getRight(),      r.getY() + ch);
        frame.lineTo (r.getRight(),      r.getBottom());
        frame.closeSubPath();
        g.setColour (AlterTheme::bgRaised.withAlpha (0.22f));
        g.fillPath (frame);
        g.setColour (neon.withAlpha (active ? 0.6f : 0.22f));
        g.strokePath (frame, juce::PathStrokeType (1.2f));

        // static internal scale gridlines — the fixed "structure"
        g.setColour (AlterTheme::textDim.withAlpha (0.12f));
        const float gridW = juce::jmax (0.0f, r.getWidth() - 4.0f);
        for (int i = 1; i < 8; ++i)
            g.fillRect (r.getX() + 2.0f, r.getY() + r.getHeight() * (float) i / 8.0f, gridW, 0.6f);

        // static left-edge measurement notches
        g.setColour (neon.withAlpha (0.4f));
        for (int i = 0; i <= 4; ++i)
            g.fillRect (r.getX() - 3.5f, r.getBottom() - r.getHeight() * (float) i / 4.0f - 0.5f, 3.0f, 1.0f);

        // ── DYNAMIC part: only the neon segmented fill rises/falls with energy ──
        const int   segs   = 26;
        const float segGap = juce::jmax (1.0f, r.getHeight() * 0.008f);
        const float segH   = juce::jmax (1.4f, (r.getHeight() - 6.0f - (float)(segs - 1) * segGap) / (float) segs);
        const int   lit    = (int) std::round (e * (float) segs);
        const float ixp = r.getX() + 3.0f, iw = juce::jmax (0.0f, r.getWidth() - 6.0f),
                    floorY = r.getBottom() - 3.0f;

        for (int s = 0; s < lit; ++s)
        {
            const float t    = (float) s / (float) segs;
            const float yTop = floorY - (float)(s + 1) * segH - (float) s * segGap;
            g.setColour (activeAccent().brighter (0.7f * t).withAlpha (0.55f + 0.45f * e));
            g.fillRect (ixp, yTop, iw, segH);
        }
        if (active)   // glowing crest at the fill level
        {
            const float capY = juce::jmax (r.getY(), floorY - (float) lit * (segH + segGap));
            g.setColour (activeAccent().brighter (0.9f).withAlpha (0.32f));
            g.fillRect (ixp - 2.0f, capY - 3.0f, iw + 4.0f, 7.0f);       // halo
            g.setColour (AlterTheme::textBright.withAlpha (0.95f));
            g.fillRect (ixp, capY, iw, 1.8f);                            // crest line
        }

        // bracketed cyber label + accent underline when active
        g.setFont (juce::Font (juce::FontOptions (juce::jmin (fs, 11.0f)).withStyle ("Bold")));
        g.setColour (active ? AlterTheme::textBright : AlterTheme::textDim);
        g.drawText (label, labelR, juce::Justification::centred, false);
        if (active)
        {
            const float uw = juce::jmin (labelR.getWidth() * 0.55f, 16.0f);
            g.setColour (activeAccent().withAlpha (0.7f));
            g.fillRect (labelR.getCentreX() - uw * 0.5f, labelR.getBottom() - 1.0f, uw, 1.3f);
        }
    }

    // Centre readout: chord / tuner needle / per-note cents — vertically centred,
    // confined to the centre area (nothing spills into the side-bar regions).
    void drawReadout (juce::Graphics& g, juce::Rectangle<int> area, bool tuner, bool midiMode,
                      const juce::String& chord, const std::vector<DisplayNote>& dn,
                      const juce::String& tName, float tCents, float tHz, bool tHas, float fs)
    {
        const int cw = area.getWidth();
        const juce::String cent = juce::String (juce::CharPointer_UTF8 ("\xc2\xa2"));

        if (tuner)
        {
            if (! tHas)
            {
                g.setFont (juce::Font (juce::FontOptions (fs + 1.0f)));
                g.setColour (AlterTheme::textDim);
                g.drawText ("play a single note...", area, juce::Justification::centred, false);
                return;
            }
            const bool inTune = std::abs (tCents) < 5.0f;
            const float big = juce::jlimit (24.0f, 64.0f, (float) cw * 0.32f);
            const int noteH = (int) (big * 1.1f), needleH = juce::jmax (30, (int) (fs * 3.4f)), csH = (int) (fs * 1.9f);
            area.removeFromTop (juce::jmax (0, (area.getHeight() - (noteH + needleH + csH)) / 2));

            g.setFont (juce::Font (juce::FontOptions (big).withStyle ("Bold")));
            g.setColour (inTune ? AlterTheme::mintGlow : AlterTheme::textBright);
            g.drawText (tName, area.removeFromTop (noteH), juce::Justification::centred, false);

            auto strip = area.removeFromTop (needleH);
            const float cxc = (float) strip.getCentreX(), halfW = (float) strip.getWidth() * 0.44f, midY = (float) strip.getCentreY();
            g.setColour (AlterTheme::bgRaised);
            g.fillRect (cxc - halfW, midY - 1.5f, halfW * 2.0f, 3.0f);
            g.setColour (AlterTheme::textDim.withAlpha (0.6f));
            for (int ct = -50; ct <= 50; ct += 10)
            {
                const float tx = cxc + (float) ct / 50.0f * halfW;
                const float tk = (ct == 0) ? 10.0f : 5.0f;
                g.fillRect (tx - 0.5f, midY - tk, 1.0f, tk * 2.0f);
            }
            const float cc = juce::jlimit (-50.0f, 50.0f, tCents);
            const float nx = cxc + cc / 50.0f * halfW;
            g.setColour (inTune ? AlterTheme::mintGlow : (cc < 0 ? juce::Colour (0xffff9d6e) : juce::Colour (0xffffd27f)));
            juce::Path tri; tri.addTriangle (nx, midY - 12.0f, nx - 6.0f, midY - 23.0f, nx + 6.0f, midY - 23.0f);
            g.fillPath (tri);
            g.fillRect (nx - 1.5f, midY - 12.0f, 3.0f, 26.0f);

            juce::String cs = (tCents >= 0 ? "+" : "") + juce::String ((int) std::round (tCents)) + cent
                            + "   " + juce::String (tHz, 1) + " Hz";
            g.setFont (juce::Font (juce::FontOptions (fs + 1.0f)));
            g.setColour (inTune ? AlterTheme::mintGlow : AlterTheme::textDim);
            g.drawText (cs, area.removeFromTop (csH), juce::Justification::centred, false);
            return;
        }

        const float chordFs = juce::jlimit (18.0f, 48.0f, (float) cw * 0.2f);
        const float nameFs  = juce::jlimit (15.0f, 28.0f, (float) cw * 0.13f);
        const float centFs  = juce::jlimit (11.0f, 15.0f, (float) cw * 0.07f);
        const float midiFs  = juce::jlimit (16.0f, 32.0f, (float) cw * 0.15f);
        const int   rowH    = (int) (nameFs * 1.3f);
        const int   chordH  = chord.isNotEmpty() ? (int) (chordFs * 1.15f) : 0;
        int rowsH = 0;
        if (! dn.empty())
            rowsH = midiMode ? (int) (midiFs * 1.4f) + (int) (fs * 1.6f) : (int) dn.size() * rowH;
        if (chordH + rowsH > 0)
            area.removeFromTop (juce::jmax (0, (area.getHeight() - chordH - rowsH) / 2));

        if (chord.isNotEmpty())
        {
            auto cr = area.removeFromTop (chordH);
            g.setFont (juce::Font (juce::FontOptions (chordFs).withStyle ("Bold")));
            for (int i = 3; i >= 1; --i)
            {
                g.setColour (activeAccent().withAlpha (0.10f * (float) i));
                g.drawText (chord, cr.translated (0, i), juce::Justification::centred, false);
            }
            g.setColour (AlterTheme::textBright);
            g.drawText (chord, cr, juce::Justification::centred, false);
        }
        else if (dn.empty())
        {
            g.setFont (juce::Font (juce::FontOptions (fs + 1.0f)));
            g.setColour (AlterTheme::textDim);
            g.drawText ("listening...", area, juce::Justification::centred, false);
            return;
        }

        if (dn.empty()) return;

        if (midiMode)
        {
            juce::String row;
            for (auto& d : dn) { if (row.isNotEmpty()) row << "   "; row << d.name; }
            g.setFont (juce::Font (juce::FontOptions (midiFs).withStyle ("Bold")));
            g.setColour (AlterTheme::textBright);
            g.drawText (row, area.removeFromTop ((int) (midiFs * 1.4f)), juce::Justification::centred, false);
            g.setFont (juce::Font (juce::FontOptions (fs)));
            g.setColour (AlterTheme::mintGlow.withAlpha (0.8f));
            g.drawText ("exact (MIDI)", area.removeFromTop ((int) (fs * 1.6f)), juce::Justification::centred, false);
        }
        else
        {
            const juce::Colour flatCol (0xffff9d6e), sharpCol (0xffffd27f);
            for (auto& d : dn)
            {
                auto rr = area.removeFromTop (rowH);
                if (rr.getHeight() < 8) break;
                g.setFont (juce::Font (juce::FontOptions (nameFs).withStyle ("Bold")));
                g.setColour (d.inTune ? AlterTheme::mintGlow : AlterTheme::textBright);
                g.drawText (d.name, rr, juce::Justification::centred, false);
                if (d.hasCents)
                {
                    const int c = (int) std::round (d.cents);
                    g.setFont (juce::Font (juce::FontOptions (centFs)));
                    if (c < 0)
                    {
                        g.setColour (d.inTune ? AlterTheme::mintGlow : flatCol);
                        g.drawText (juce::String (c) + cent, rr.reduced (8, 0), juce::Justification::centredLeft, false);
                    }
                    else
                    {
                        g.setColour (c == 0 ? AlterTheme::mintGlow : (d.inTune ? AlterTheme::mintGlow : sharpCol));
                        g.drawText ((c > 0 ? "+" : "") + juce::String (c) + cent, rr.reduced (8, 0), juce::Justification::centredRight, false);
                    }
                }
            }
        }
    }

    void timerCallback() override
    {
        if (AlterTheme::hudFrozen.load()) return;   // HUD "Hold": keep last frame

        // ONE tone update per tick, and only while tone colour is on. paint() is the
        // wrong place: it also runs for repaints the module did not ask for (a
        // resize, an overlapping window, a Fusion pulling a frame), which would make
        // the hue chase the note at a rate set by whatever else is on screen.
        if (colourByTone)
            toneHue.update (audioSource, toneTwist, toneSmooth);

        analyse();
        repaint();
    }

    void analyse()
    {
        if (tunerMode.load()) updateTuner();   // accurate mono pitch for the needle

        // ── MIDI mode ("premium" auto): exact notes from the DAW track override
        //    the FFT detection whenever the source's plugin reports MIDI ('M'
        //    packets, fresh < 2 s). No peak-picking guesswork: note names, the
        //    chord and the pitch-class ring are built from the real notes.
        std::vector<IAudioSource::MidiNote>& mn = midiScratch;
        const bool midiMode = audioSource.getMidiNotes (mn);
        {
            const juce::ScopedLock sl (resultLock);
            midiActive = midiMode;
        }

        if (midiMode)
        {
            notes.clear();
            for (const auto& m : mn)
            {
                const int nn = (int) m.note;
                if (nn < 24 || nn > 108) continue;   // same display range as detection
                const float freq = 440.0f * std::pow (2.0f, ((float) nn - 69.0f) / 12.0f);
                // velocity 1..127 → pseudo-dB so the shared strength mapping
                // ((dB+60)/54) comes out as velocity/127:
                const float dB = -60.0f + 54.0f * (float) m.velocity / 127.0f;
                notes.push_back ({ freq, dB, nn });
            }

            {
                const juce::ScopedLock sl (resultLock);
                if (! notes.empty())
                {
                    midiEmptyFrames = 0;
                    const DetectedNote* top = &notes.front();   // loudest velocity
                    for (const auto& nd : notes)
                        if (nd.db > top->db) top = &nd;
                    dominantName  = midiToName (top->midi);
                    dominantCents = 0.0f;                        // exact by definition
                    dominantHz    = top->freq;
                }
                else if (++midiEmptyFrames > 12)
                {
                    dominantName.clear();
                }
            }

            std::sort (notes.begin(), notes.end(),
                       [] (const DetectedNote& x, const DetectedNote& y) { return x.midi < y.midi; });

            applyNotesAndDetect (true);   // instant gate — MIDI needs no debouncing
            return;
        }

        const int bins = audioSource.getLastFft (fft);
        if (bins < 256) { decayOnly(); return; }

        // Nyquist = sampleRate/2. Use the device's ACTUAL rate (may be 44.1 kHz, not the
        // assumed 48 kHz) so bin i → Hz — and the detected note — is correct.
        const float binHz = (float) (audioSource.getSampleRate() * 0.5) / (float) bins;

        // dB from normalised value: dB = v*90 - 90
        // noise floor = average dB of audible range
        const int iLo = juce::jmax (2,        (int) ( 50.0f / binHz));
        const int iHi = juce::jmin (bins - 3, (int) (5000.0f / binHz));
        if (iHi <= iLo) { decayOnly(); return; }

        // MEDIAN noise floor (robust): the mean floor is dragged up by strong tonal
        // peaks, which desensitises detection whenever the signal is loud. The median
        // of the band tracks the true broadband floor and ignores the peaks.
        floorScratch.assign (fft.begin() + iLo, fft.begin() + (iHi + 1));
        const size_t mid = floorScratch.size() / 2;
        std::nth_element (floorScratch.begin(), floorScratch.begin() + mid, floorScratch.end());
        const float floorDb = floorScratch[mid] * 90.0f - 90.0f;

        // ── 1) collect candidate peaks (strict local maxima) ────────────────
        notes.clear();
        float strongestDb = -120.0f;

        for (int i = iLo; i <= iHi; ++i)
        {
            const float v  = fft[(size_t) i];
            const float dB = v * 90.0f - 90.0f;

            if (dB < floorDb + peakThresholdDb || dB < absGateDb) continue;
            if (! (v > fft[(size_t)(i-1)] && v >= fft[(size_t)(i+1)]
                && v > fft[(size_t)(i-2)] && v >= fft[(size_t)(i+2)])) continue;

            // parabolic interpolation on dB for sub-bin frequency
            const float a = fft[(size_t)(i-1)] * 90.0f - 90.0f;
            const float c = fft[(size_t)(i+1)] * 90.0f - 90.0f;
            const float denom = a - 2.0f * dB + c;
            const float off = (std::abs (denom) > 1.0e-6f)
                            ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / denom) : 0.0f;
            const float freq = ((float) i + off) * binHz;
            if (freq < 55.0f || freq > 5200.0f) continue;

            const int midi = (int) std::lround (69.0 + 12.0 * std::log2 (freq / 440.0));
            if (midi < 24 || midi > 108) continue;

            notes.push_back ({ freq, dB, midi });
            strongestDb = juce::jmax (strongestDb, dB);
        }

        // ── 2) keep only strong peaks, strongest first, suppress harmonics ──
        //  - peak must be within 18 dB of the strongest one
        //  - generous 6%·k harmonic tolerance (piano partials are stretched!)
        //  - an overtone counts as a NEW note only if > +18 dB over its fundamental
        //  - peaks within 1 semitone of an accepted note are spectral leakage
        std::sort (notes.begin(), notes.end(),
                   [] (const DetectedNote& x, const DetectedNote& y) { return x.db > y.db; });

        std::vector<DetectedNote> accepted;
        for (const auto& cand : notes)
        {
            if (accepted.size() >= 5) break;
            if (cand.db < strongestDb - 18.0f) continue;

            bool reject = false;
            for (const auto& acc : accepted)
            {
                if (std::abs (cand.midi - acc.midi) <= 1) { reject = true; break; }  // dup/leakage

                const float lo = juce::jmin (acc.freq, cand.freq);
                const float hi = juce::jmax (acc.freq, cand.freq);
                const float ratio = hi / lo;
                const float k = std::round (ratio);
                if (k >= 2.0f && k <= 12.0f && std::abs (ratio - k) < 0.06f * k)
                {
                    // harmonically related to an already accepted (stronger) note
                    if (cand.db < acc.db + 18.0f) { reject = true; break; }
                }
            }
            if (! reject)
                accepted.push_back (cand);
        }
        notes.swap (accepted);

        // TUNING readout: the strongest partial's deviation (in cents) from the nearest
        // equal-tempered pitch (A4 = 440 Hz). notes is still strongest-first here.
        {
            const juce::ScopedLock sl (resultLock);
            if (! notes.empty())
            {
                const float f     = notes.front().freq;
                const float exact = 69.0f + 12.0f * std::log2 (f / 440.0f);
                const int   nrst  = (int) std::lround (exact);
                const float cents = (exact - (float) nrst) * 100.0f;
                dominantCents = dominantName == midiToName (nrst)
                              ? dominantCents * 0.6f + cents * 0.4f   // smooth when steady
                              : cents;                                 // snap on note change
                dominantName  = midiToName (nrst);
                dominantHz    = f;
            }
        }

        std::sort (notes.begin(), notes.end(),
                   [] (const DetectedNote& x, const DetectedNote& y) { return x.midi < y.midi; });

        applyNotesAndDetect (false);
    }

    // Shared tail for BOTH paths (FFT detection and MIDI mode): temporal gating,
    // pitch-class energies, note list text and chord matching. instantGate = MIDI
    // (exact notes need no ~130 ms persistence debounce — they gate immediately).
    void applyNotesAndDetect (bool instantGate)
    {
        // ── 3) temporal gating: a note must persist ~4 frames (~130 ms) ─────
        std::array<bool, 128>  seen {};
        std::array<float, 128> strength {};
        for (const auto& nd : notes)
        {
            seen[(size_t) nd.midi] = true;
            strength[(size_t) nd.midi] = juce::jlimit (0.0f, 1.0f, (nd.db + 60.0f) / 54.0f);
        }

        // per-note frequency lookup (FFT) for the cents readout
        float freqOf[128];
        for (auto& f : freqOf) f = 0.0f;
        for (const auto& nd : notes) freqOf[(size_t) nd.midi] = nd.freq;

        std::array<float, 12> hit {};
        juce::String txt;
        std::vector<DisplayNote> dn;
        int distinct = 0;
        const int inc = instantGate ? 4 : 1;

        for (int midiN = 24; midiN <= 108; ++midiN)
        {
            auto& cnt = noteFrames[(size_t) midiN];
            cnt = seen[(size_t) midiN] ? juce::jmin (cnt + inc, 10) : juce::jmax (cnt - 2, 0);

            if (cnt >= 4)   // gated active note
            {
                ++distinct;
                hit[(size_t)(midiN % 12)] = juce::jmax (hit[(size_t)(midiN % 12)],
                                                        strength[(size_t) midiN]);
                if (txt.isNotEmpty()) txt << "  ";
                txt << midiToName (midiN);

                DisplayNote d;
                d.name = midiToName (midiN);
                if (! instantGate && freqOf[(size_t) midiN] > 0.0f)   // FFT: exact cents
                {
                    const float exact = 69.0f + 12.0f * std::log2 (freqOf[(size_t) midiN] / 440.0f);
                    d.cents    = (exact - (float) midiN) * 100.0f;
                    d.hasCents = true;
                    d.inTune   = std::abs (d.cents) < 5.0f;
                }
                dn.push_back (d);   // sorted ascending: loop runs low → high
            }
        }

        {
            const juce::ScopedLock sl (resultLock);
            for (int pc = 0; pc < 12; ++pc)
            {
                const float target = hit[(size_t) pc];
                auto& e = pcEnergy[(size_t) pc];
                e = (target > e) ? e + 0.5f  * (target - e)   // fast attack
                                 : e * 0.86f;                 // smooth release
                if (e < 0.01f) e = 0.0f;
            }
            noteListText    = txt;
            activeNoteCount = distinct;
            displayNotes    = std::move (dn);
        }

        detectChord();
    }

    void decayOnly()
    {
        for (auto& c : noteFrames) c = juce::jmax (c - 2, 0);

        const juce::ScopedLock sl (resultLock);
        for (auto& e : pcEnergy) { e *= 0.86f; if (e < 0.01f) e = 0.0f; }
        activeNoteCount = 0;
        if (++emptyFrames > 12) { chordName.clear(); noteListText.clear(); dominantName.clear(); chordConfidence = 0.0f; displayNotes.clear(); }
    }

    // McLeod Pitch Method (NSDF): accurate monophonic fundamental for the tuner.
    // Far more precise & stable than FFT peak-picking on a single note/string.
    float detectMonoHz()
    {
        const int n = audioSource.getLastWaveform (waveL, waveR);
        if (n < 1024) return 0.0f;

        const double sr  = audioSource.getSampleRate();
        const int    N   = juce::jmin (n, 2048);
        const int    off = n - N;

        tunerBuf.resize ((size_t) N);
        double energy = 0.0;
        for (int i = 0; i < N; ++i)
        {
            const float m = 0.5f * (waveL[(size_t)(off + i)] + waveR[(size_t)(off + i)]);
            tunerBuf[(size_t) i] = m;
            energy += (double) m * m;
        }
        if (energy < 1.0e-4) return 0.0f;   // too quiet to tune

        const float* x = tunerBuf.data();
        const int minLag = juce::jmax (2,     (int) (sr / 1200.0));   // up to 1200 Hz
        const int maxLag = juce::jmin (N - 1, (int) (sr /   50.0));   // down to 50 Hz
        if (maxLag <= minLag + 2) return 0.0f;

        nsdfBuf.assign ((size_t) (maxLag + 1), 0.0f);
        for (int tau = minLag; tau <= maxLag; ++tau)
        {
            double ac = 0.0, den = 0.0;
            const int lim = N - tau;
            for (int i = 0; i < lim; ++i)
            {
                ac  += (double) x[i] * x[i + tau];
                den += (double) x[i] * x[i] + (double) x[i + tau] * x[i + tau];
            }
            nsdfBuf[(size_t) tau] = den > 1.0e-9 ? (float) (2.0 * ac / den) : 0.0f;
        }

        // first local maximum above 85% of the global peak → avoids octave errors
        float gmax = 0.0f;
        for (int t = minLag; t <= maxLag; ++t) gmax = juce::jmax (gmax, nsdfBuf[(size_t) t]);
        if (gmax < 0.45f) return 0.0f;   // no clear pitch (noisy / polyphonic)

        const float thresh = 0.85f * gmax;
        int lag = -1;
        for (int t = minLag + 1; t < maxLag; ++t)
            if (nsdfBuf[(size_t) t] > thresh
                && nsdfBuf[(size_t) t] >= nsdfBuf[(size_t)(t - 1)]
                && nsdfBuf[(size_t) t] >= nsdfBuf[(size_t)(t + 1)])
            { lag = t; break; }
        if (lag < 1) return 0.0f;

        // parabolic interpolation on the NSDF peak → sub-sample period
        const float a = nsdfBuf[(size_t)(lag - 1)], bb = nsdfBuf[(size_t) lag], c = nsdfBuf[(size_t)(lag + 1)];
        const float denom = a - 2.0f * bb + c;
        const float delta = std::abs (denom) > 1.0e-6f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / denom) : 0.0f;
        const float period = (float) lag + delta;
        return period > 0.0f ? (float) (sr / (double) period) : 0.0f;
    }

    void updateTuner()
    {
        const float hz = detectMonoHz();
        const juce::ScopedLock sl (resultLock);
        if (hz >= 20.0f)
        {
            const float exact = 69.0f + 12.0f * std::log2 (hz / 440.0f);
            const int   nn    = (int) std::lround (exact);
            const float cents = (exact - (float) nn) * 100.0f;
            const juce::String nm = midiToName (nn);
            tunerHz       = tunerHz > 0.0f ? tunerHz * 0.7f + hz * 0.3f : hz;   // smooth Hz
            tunerCents    = (nm == tunerName) ? tunerCents * 0.6f + cents * 0.4f : cents;
            tunerName     = nm;
            tunerHasPitch = true;
            tunerHold     = 0;
        }
        else if (++tunerHold > 15)   // ~0.5 s of silence → clear
        {
            tunerHasPitch = false;
            tunerName.clear();
            tunerHz = 0.0f;
        }
    }

    // ── Chord detection ──────────────────────────────────────────────────────
    void detectChord()
    {
        struct Template { const char* suffix; std::initializer_list<int> iv; };
        static const Template templates[] = {
            { "",      {0,4,7}       }, { "m",    {0,3,7}       },
            { "dim",   {0,3,6}       }, { "aug",  {0,4,8}       },
            { "sus2",  {0,2,7}       }, { "sus4", {0,5,7}       },
            { "7",     {0,4,7,10}    }, { "maj7", {0,4,7,11}    },
            { "m7",    {0,3,7,10}    }, { "m7b5", {0,3,6,10}    },
            { "dim7",  {0,3,6,9}     }, { "6",    {0,4,7,9}     },
            { "m6",    {0,3,7,9}     }, { "add9", {0,2,4,7}     },
        };
        static const char* roots[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };

        // Snapshot the smoothed pitch-class energies (the analysis basis for the chord).
        std::array<float, 12> E {};
        float total = 0.0f;
        {
            const juce::ScopedLock sl (resultLock);
            for (int pc = 0; pc < 12; ++pc) { E[(size_t) pc] = pcEnergy[(size_t) pc]; total += E[(size_t) pc]; }
        }

        std::array<bool, 12> on {};
        int onCount = 0;
        for (int pc = 0; pc < 12; ++pc)
            if (E[(size_t) pc] > 0.22f) { on[(size_t) pc] = true; ++onCount; }

        // Bass = lowest detected note (notes is sorted midi-ascending here) → slash chords.
        const int bassPc = notes.empty() ? -1 : (notes.front().midi % 12);

        juce::String best;
        float bestScore = 0.0f, bestConf = 0.0f;
        int   bestRoot  = -1;

        // WEIGHTED matching: score by how much of the total energy sits on the chord
        // tones vs strays off them, so the best-fitting template wins (e.g. Cmaj7 only
        // beats C when the major-7th really carries energy). Confidence = chord-tone
        // energy share. All chord tones are still required to be present.
        if (onCount >= 3 && onCount <= 6 && total > 1.0e-4f)
        {
            for (int root = 0; root < 12; ++root)
            {
                if (! on[(size_t) root]) continue;

                for (const auto& t : templates)
                {
                    const int tSize = (int) t.iv.size();
                    int   matched = 0;
                    float chordE  = 0.0f;
                    for (int iv : t.iv)
                    {
                        const int pc = (root + iv) % 12;
                        if (on[(size_t) pc]) ++matched;
                        chordE += E[(size_t) pc];
                    }
                    if (matched < tSize) continue;          // all chord tones required

                    const float offE  = total - chordE;
                    float score = chordE - 0.6f * offE + 0.04f * (float) tSize;
                    if (root == bassPc) score += 0.15f;     // root-position bonus

                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestConf  = juce::jlimit (0.0f, 1.0f, chordE / total);
                        bestRoot  = root;
                        best = juce::String (roots[root]) + t.suffix;
                    }
                }
            }
        }

        // Slash chord: a chord tone other than the root is sounding in the bass.
        if (best.isNotEmpty() && bassPc >= 0 && bassPc != bestRoot && on[(size_t) bassPc])
            best << "/" << roots[bassPc];

        const juce::ScopedLock sl (resultLock);

        // stability: a chord must persist a few frames before being shown
        if (best.isNotEmpty())
        {
            emptyFrames = 0;
            if (best == pendingChord) ++chordStableFrames;
            else { pendingChord = best; chordStableFrames = 0; }

            if (chordStableFrames >= 3)
            {
                chordName       = pendingChord;
                chordConfidence = bestConf;
            }
        }
        else if (++emptyFrames > 10)
        {
            chordName.clear();
            pendingChord.clear();
            chordStableFrames = 0;
            chordConfidence   = 0.0f;
        }
    }

    static juce::String midiToName (int midi)
    {
        static const char* names[12] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        return juce::String (names[midi % 12]) + juce::String (midi / 12 - 1);
    }

    // ── Members ──────────────────────────────────────────────────────────────
    IAudioSource& audioSource;

    std::vector<float>        fft;
    std::vector<float>        floorScratch;   // median-floor working buffer
    std::vector<DetectedNote> notes;
    std::vector<DisplayNote>  displayNotes;   // per-note readout, guarded by resultLock
    std::vector<IAudioSource::MidiNote> midiScratch;   // reused per frame
    bool midiActive      { false };   // guarded by resultLock (paint badge)
    int  midiEmptyFrames { 0 };       // MIDI mode: frames with no held notes

    // ── Tuner mode (accurate monophonic pitch via McLeod / NSDF) ──────────────
    std::atomic<bool>  tunerMode { false };
    std::vector<float> waveL, waveR, tunerBuf, nsdfBuf;   // detection scratch
    juce::String tunerName;             // guarded by resultLock
    float tunerCents    { 0.0f };
    float tunerHz       { 0.0f };
    bool  tunerHasPitch { false };
    int   tunerHold     { 0 };

    float peakThresholdDb { 16.0f };
    float absGateDb       { -70.0f };
    juce::Colour accent   { AlterTheme::caribbeanGreen };

    // Colour by tone. Updated once per timer tick; read by paint().
    bool  colourByTone { false };
    bool  toneTwist    { false };
    float toneSmooth   { 0.5f };    // the SLIDER value; the rate curve lives in PitchUtils
    PitchUtils::ToneHueTracker toneHue;

    /** The colour everything lit is derived from: the note in tone mode, the
        user's accent otherwise. */
    juce::Colour activeAccent() const noexcept
    {
        return (colourByTone && toneHue.hasTone()) ? toneHue.colour() : accent;
    }

    juce::CriticalSection   resultLock;
    std::array<int, 128>    noteFrames {};   // temporal gating counters
    std::array<float, 12>   pcEnergy {};
    juce::String            chordName, pendingChord, noteListText;
    juce::String            dominantName;     // strongest note (for the tuning readout)
    float dominantCents   { 0.0f };           // cents off equal temperament (A4=440)
    float dominantHz      { 0.0f };
    float chordConfidence { 0.0f };           // 0..1 chord-tone energy share
    int chordStableFrames { 0 };
    int emptyFrames       { 0 };
    int activeNoteCount   { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToneAnalyzerMeter)
};
