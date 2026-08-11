#pragma once
#include <JuceHeader.h>
#include "AudioSourceInterface.h"
#include "AlterTheme.h"
#include "PitchUtils.h"

// Professional audio meter with dB scale (like in DAW)
// Supports multiple modes: RMS, True Peak, LUFS
// Renamed from VisualRmsBar -> VisualAudioMeter to better reflect functionality
class VisualAudioMeter : public ThemedBackground, public juce::Component, private juce::Timer
{
public:
    enum class MeterMode
    {
        RMS,         // Root Mean Square (average energy)
        TruePeak,    // TRUE peak = inter-sample peak, ITU-R BS.1770 (dBTP) — NOT just max sample
        LUFS,        // Loudness Units Full Scale (perceptual loudness, ITU-R BS.1770)
        LevelHistory // DAW-style scrolling level envelope (peak silhouette + RMS core)
    };

    // Level-history stereo display layout (ported from the Oscilloscope module).
    enum class DisplayMode { Mono, Stereo, Mirror, MirrorStereo };

    enum class ColorMode
    {
        Standard,            // Fixed DAW colours (green, yellow, orange, red)
        CustomGradient,      // one hue, stepped down in brightness per zone
        CustomComplementary  // one hue, walked toward its complement per zone
    };

    explicit VisualAudioMeter (IAudioSource& r) : audioSource (r)
    {
        setOpaque (true);
        // kEnvelopeRate, not a loose 30: setSmoothAmount derives its coefficients
        // from dt = 1/kEnvelopeRate, so the timer and that maths must be the same
        // number or every time constant is quietly wrong by their ratio.
        startTimerHz (kEnvelopeRate);
        // Start consistent with the state default (kSmooth = 0.5) rather than with
        // alpha = 0, so a meter behaves the same before and after its first apply.
        setSmoothAmount (0.5f);
    }

    ~VisualAudioMeter() override
    {
        // CRITICAL: Stop timer before destruction to avoid crash!
        stopTimer();
    }

    /** ONE SMOOTHING SCALE FOR EVERY MODE.

        This used to be three unrelated mappings behind one slider, and the slider
        did not mean the same thing in any two of them:

          RMS        alpha 0.00 → 0.97   (at 0, no smoothing at all)
          True Peak  release 0.95 → 0.995 (at 0, already heavily smoothed)
          LUFS       alpha 0.85 → 0.98   (at 0, MORE smoothed than RMS is at 1)

        So "smooth 50%" was a different amount of smoothing in each mode, the same
        preset read differently depending on which mode a meter happened to be in,
        and two meters set identically did not behave identically. There was nothing
        to compare because there was no scale.

        Now the slider sets a TIME CONSTANT — how long the display takes to cover
        63% of a step — and every mode derives its coefficient from that one number
        at the tick rate. 10 ms to 2 s, GEOMETRIC, for the same reason PitchUtils
        maps its smoothing that way: what the eye judges is the settling TIME, and
        linear-in-coefficient crowds all the perceptible change into the last few
        percent of the travel. Equal steps of the slider now multiply the settling
        time by a constant factor.

        The modes still differ in CHARACTER, which is the part that should differ:
        True Peak rises instantly and falls at this time constant, as a peak meter
        must; RMS and LUFS move both ways at it. */
    static constexpr float kSmoothTauFast = 0.010f;   // seconds, slider at 0
    static constexpr float kSmoothTauSlow = 2.000f;   // seconds, slider at 1

    static float smoothTau (float s01) noexcept
    {
        s01 = juce::jlimit (0.0f, 1.0f, s01);
        return kSmoothTauFast * std::pow (kSmoothTauSlow / kSmoothTauFast, s01);
    }

    void setSmoothAmount (float s01)
    {
        smooth01 = juce::jlimit (0.0f, 1.0f, s01);

        const float dt  = 1.0f / (float) kEnvelopeRate;   // the tick this all runs at
        const float tau = smoothTau (smooth01);

        // Fraction of the old value kept per tick. One number, every mode.
        alpha        = std::exp (-dt / tau);
        releaseCoeff = alpha;      // True Peak: instant attack, this on the way down
        lhAlpha      = alpha;      // Level history envelope

        // The Trend EMA is written the other way round (weight of the NEW sample),
        // so it is the complement of the same retention — same time constant, same
        // slider position, same meaning.
        trendAlpha = 1.0f - alpha;
    }

    /** Settling time the slider currently asks for, seconds. */
    float getSmoothTauSec() const noexcept { return smoothTau (smooth01); }

    // Set meter mode (RMS, True Peak, LUFS)
    void setMeterMode (MeterMode mode)
    {
        if (meterMode != mode)
        {
            meterMode = mode;
            smoothed = (mode == MeterMode::LUFS) ? -100.0f : 0.0f;
            // The hold is in dB in every mode, so it re-arms to the same floor in
            // every mode — no per-mode unit to get wrong.
            peakHoldDb  = -200.0f;
            peakHoldAge = 0.0f;
            sessionMaxDb = -999.0f;   // the max readout must not mix units across modes
            lhSeeded     = false;     // the held envelope belongs to the old mode
            setSmoothAmount (smooth01);
            repaint();
        }
    }

    /** Double-click resets the session MAX readout and the peak-hold marker. */
    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        sessionMaxDb = -999.0f;
        peakHoldDb   = -200.0f;
        peakHoldAge  = 0.0f;
        repaint();
    }

    // Set display view: false = Momentary (bar), true = Trend (waveform over time)
    void setTrendMode (bool enabled)
    {
        if (trendMode != enabled)
        {
            trendMode = enabled;
            if (trendMode)
                measureSmoothed = -60.0f;
            repaint();
        }
    }

    bool getTrendMode() const noexcept { return trendMode; }

    MeterMode getMeterMode() const { return meterMode; }

    // ── Level-history controls (ported from the Oscilloscope) ───────────────
    static constexpr int kEnvelopeRate = 30;   // capture ticks/sec (== meter timer)
    static constexpr int kMaxWindowSec = 30;

    /** Notified when the user zooms the window with the mouse wheel. */
    std::function<void (float)> onLevelHistoryWindowChanged;

    void setLhDisplayMode (DisplayMode m) noexcept { lhDisplay = m; repaint(); }
    DisplayMode getLhDisplayMode() const noexcept { return lhDisplay; }

    /** Visible window for the level-history view, seconds (0.1–30).
        The envelope ring is sized to the window – nothing extra is stored. */
    void setLevelHistoryWindow (float seconds)
    {
        // THE WINDOW IS A VIEW ONTO THE RING, NOT THE SIZE OF IT.
        //
        // The ring used to be re-allocated to match: widening from 2 s to 10 s built
        // a bigger buffer holding only the 2 s that had been recorded, so eight
        // seconds of the module had no sample behind them. That is both faults in
        // one — the trace stopped short of the right-hand edge, and it grew there in
        // steps over the following seconds as real data arrived, instead of simply
        // showing the history that was already in hand.
        //
        // A fixed ring at the maximum window costs 30 s x 30 Hz x 24 bytes = 21 kB
        // and removes the whole problem: history is kept whether or not it is
        // currently being looked at, so changing the window is instant and shows
        // real recorded audio the moment it is asked for.
        lhWindowSec = juce::jlimit (0.1f, (float) kMaxWindowSec, seconds);
        ensureRing();
        repaint();
    }

    /** The ring, allocated once at its full capacity. */
    void ensureRing()
    {
        const int cap = kMaxWindowSec * kEnvelopeRate;
        if ((int) lhHistory.size() != cap)
        {
            lhHistory.assign ((size_t) cap, EnvSample{});
            lhWrite = 0; lhFilled = 0; lhTotal = 0; lhSeeded = false;
        }
    }

    void setLhColour (juce::Colour c) noexcept { lhColour = c; repaint(); }

    /** Level history, overlaid Stereo display: R-channel hue relative to L.
        0 = complementary, 1 = analogous — the same pair, and the same two hue
        distances, as the Spectrum and the Oscilloscope. This view was ported out of
        the Oscilloscope and shares its state keys; the colour relationship is one
        more of them rather than a second, differently-named setting for the
        identical choice. */
    void setLhStereoColourMode (int mode) noexcept
    {
        const bool a = (mode == 1);
        if (lhStereoAnalogous != a) { lhStereoAnalogous = a; repaint(); }
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        if (meterMode != MeterMode::LevelHistory) return;
        const float factor = (wheel.deltaY > 0) ? 0.8f : 1.25f;
        setLevelHistoryWindow (lhWindowSec * factor);
        if (onLevelHistoryWindowChanged)
            onLevelHistoryWindowChanged (lhWindowSec);
    }

    // ── Integrated measurement control ──────────────────────────────────
    // Start a new measurement: clears buffer, reserves memory, begins capturing.
    void startMeasurement()
    {
        measurementBuffer.clear();
        measurementBuffer.reserve ((size_t) kMaxMeasurePoints);
        measureSmoothed = -60.0f;
        measureSum = 0.0;
        isMeasuring = true;
        repaint();
    }

    // Stop measurement: halts capturing but keeps buffer intact for display.
    void stopMeasurement()
    {
        isMeasuring = false;
        repaint();
    }

    bool isMeasurementActive()    const noexcept { return isMeasuring; }
    int  getMeasurementPointCount() const noexcept { return (int) measurementBuffer.size(); }

    /** Mean of everything captured in the Trend view, dB. Returns false when there
        is nothing to average yet, so the caller can leave the readout off rather
        than print a placeholder.

        Kept as a RUNNING SUM rather than summed on demand: the info line is redrawn
        every frame and the buffer holds up to 30 minutes at 30 Hz, so summing it
        there would put 54 000 additions into each of them for a number that changes
        by a hair. Dividing a double that has accumulated 54 000 dB values is exact
        enough for a readout printed to one decimal. */
    bool getTrendAverageDb (float& outDb) const noexcept
    {
        if (measurementBuffer.empty()) return false;
        outDb = (float) (measureSum / (double) measurementBuffer.size());
        return true;
    }
    // Returns elapsed seconds (based on 30 Hz timer)
    float getMeasurementElapsedSec() const noexcept { return (float) measurementBuffer.size() / 30.0f; }

    // Public getter for info display
    float getCurrentValue() const
    {
        if (meterMode == MeterMode::TruePeak)
            return audioSource.getLastPeak();
        if (meterMode == MeterMode::LUFS)
        {
            const float lufs = audioSource.getLastLufs();
            return (lufs <= -60.0f) ? 0.0f : std::pow (10.0f, lufs / 20.0f);
        }
        return audioSource.getLastRms();
    }

    // Get the smoothed value (matches what the bar actually displays)
    float getSmoothedValue() const noexcept { return smoothed; }

    // Set custom color (from controller)
    void setBarColour (juce::Colour c) { barColour = c; repaint(); }

    // Set custom color mode (0=Standard, 1=Gradient, 2=Spectrum)
    void setCustomColorMode (bool useCustom) { colorMode = useCustom ? ColorMode::CustomGradient : ColorMode::Standard; repaint(); }

    // Set color mode (Standard / CustomGradient / CustomComplementary)
    void setColorMode (ColorMode mode) { colorMode = mode; repaint(); }

    // ── Colour by tone ────────────────────────────────────────────────────────
    //  A meter is not one colour, it is a LADDER of them: safe, warning, hot, clip.
    //  So tone colour cannot simply replace the bar colour the way it does in the
    //  scope or the spectrum — it replaces the colour the ladder is BUILT from, and
    //  the shade below decides how the rungs step away from it. That is why the two
    //  are separate settings and separate state keys: one says which colour, the
    //  other says how the zones differ from it.
    //
    //  Covers the bar view, the peak-hold bar and the Trend curve at once, because
    //  all three ask zoneTint() rather than reading a colour of their own.
    // The SAME two choices the manual colour offers, because it is the same
    // question — a colour has been picked, now how do the loudness zones step away
    // from it — and having tone mode answer it with a different set of words was
    // just two vocabularies for one idea.
    enum class ToneShade
    {
        Gradient,      // same hue, stepped down in brightness
        Complementary  // hot zones walk toward the far side of the wheel
    };

    void setColourByTone (bool b)
    {
        if (colourByTone == b) return;
        colourByTone = b;
        // The held hue is as old as the last tone-mode frame; snap to the note that
        // is sounding rather than sliding across the wheel from one that stopped.
        if (b) toneHue.reset();
        repaint();
    }

    /** 'Mirror tone color' — reverses the tone→hue wheel (see PitchUtils). */
    void setToneTwist (bool b) { toneTwist = b; repaint(); }

    /** 'Tone smooth' 0..1: HIGH = heavier smoothing. Stored RAW —
        PitchUtils::toneSmoothToRate owns the curve. */
    void setToneSmooth (float s01) { toneSmooth = juce::jlimit (0.0f, 1.0f, s01); }

    void setToneShade (ToneShade s) { toneShade = s; repaint(); }

    /** Draw the peak/max hold as a SECOND, narrow bar beside the smoothed one.

        The hold VALUE is not new — it is the same peakHold the marker line has
        always tracked, with the same per-mode hold time and decay. What changes is
        that it gets its own column instead of a one-pixel line that disappears into
        the fill exactly when the level is high enough to matter. */
    void setTwoBars (bool b) { if (twoBars != b) { twoBars = b; repaint(); } }
    bool getTwoBars() const noexcept { return twoBars; }

    /** Level history: the 0 dBFS boundary, the dB ruler at the right, and a red
        mark over every column that reached full scale. */
    void setClipZone (bool b) { if (showClipZone != b) { showClipZone = b; repaint(); } }

    /** Readout for the pointer at `p` (module-local), or empty when there is
        nothing to say. The host draws it — this view gets no mouse events itself,
        exactly as with the Spectrum.

        Reports what the POSITION means, not what the signal is doing there — in
        every view, because in every view the height IS a level. On the bars that is
        the dB scale printed down the side; in Level history it is the fixed absolute
        scale the envelope is drawn against (see kEnvVScale). Level history and Trend
        add the time, since their x axis carries one. */
    juce::String cursorText (juce::Point<float> p) const
    {
        // ── BAR AND TREND VIEWS: the dB the pointer's HEIGHT stands for ────────
        //
        //  Measured against lastBarArea, the rectangle paint() actually drew into,
        //  rather than against a second copy of the "bounds minus the readout strip,
        //  reduced by 2" arithmetic. Those two would agree today and drift the first
        //  time the strip height changes — and a readout that disagrees with the
        //  scale printed beside it is worse than none.
        if (meterMode != MeterMode::LevelHistory)
        {
            const auto bar = lastBarArea.toFloat();
            if (bar.getHeight() < 4.0f || ! bar.contains (p)) return {};

            const float norm = juce::jlimit (0.0f, 1.0f,
                                             (bar.getBottom() - p.y) / bar.getHeight());
            const float db   = getMinDb() + norm * (getMaxDb() - getMinDb());

            const char* unit = (meterMode == MeterMode::LUFS)     ? " LUFS"
                             : (meterMode == MeterMode::TruePeak) ? " dBTP"
                                                                  : " dBFS";
            juce::String t;

            // In Trend the x axis carries the capture's own clock, so the pointer
            // answers "how loud, and when" instead of only "how loud".
            if (trendMode && ! measurementBuffer.empty())
            {
                const float u = juce::jlimit (0.0f, 1.0f, (p.x - bar.getX()) / bar.getWidth());
                const float secs = u * (float) measurementBuffer.size() / 30.0f;
                const int   mins = (int) (secs / 60.0f);
                t << juce::String::formatted ("%02d:%02d", mins, ((int) secs) % 60) << "  |  ";
            }

            t << juce::String (db, 1) << unit;
            return t;
        }

        auto area = lastBarArea.toFloat();
        if (! area.contains (p) || area.getWidth() < 4.0f || area.getHeight() < 4.0f) return {};

        // MirrorStereo splits the module into two independent pictures; the pointer
        // belongs to whichever half it is in, and the centre it measures from is
        // that half's, not the module's.
        if (lhDisplay == DisplayMode::MirrorStereo)
        {
            auto top = area.withHeight (area.getHeight() * 0.5f);
            area = (p.y < top.getBottom()) ? top : top.translated (0.0f, top.getHeight());
        }

        const float u       = juce::jlimit (0.0f, 1.0f, (p.x - area.getX()) / area.getWidth());
        const float secsAgo = (1.0f - u) * lhVisibleSec ((int) std::lround (area.getWidth()));

        const float vScale = area.getHeight() * kEnvVScale;
        const float unit   = std::abs (p.y - area.getCentreY()) / juce::jmax (1.0f, vScale);
        const float db     = unitToDb (unit);

        juce::String t;
        t << (secsAgo < 0.05f ? juce::String ("now")
                              : "-" + juce::String (secsAgo, secsAgo < 10.0f ? 2 : 1) + " s")
          << "  |  "
          << (unit <= 0.001f ? juce::String ("-inf dB")
                             : juce::String (db, 1) + " dB");
        return t;
    }

    // Tell the meter what rotation the host is applying (so scale text can be skipped when rotated)
    void setHostRotation (int r) { hostRotation = juce::jlimit (0, 3, r); repaint(); }
    int  getHostRotation() const noexcept { return hostRotation; }

    // Expose scale data so PanelHost can draw labels in host space (always horizontal)
    struct ScaleInfo
    {
        float minDb, maxDb;
        std::vector<float> checkpoints;
    };

    ScaleInfo getScaleInfo() const
    {
        ScaleInfo info;
        info.minDb = -60.0f;

        if (meterMode == MeterMode::RMS)
        {
            info.maxDb = kScaleMaxDb;
            info.checkpoints = { -60.0f, -48.0f, -36.0f, -24.0f, -18.0f, -12.0f, -9.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f };
        }
        else if (meterMode == MeterMode::TruePeak)
        {
            info.maxDb = +6.0f;
            info.checkpoints = { -60.0f, -24.0f, -18.0f, -12.0f, -9.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f };
        }
        else // LUFS
        {
            info.maxDb = +6.0f;
            info.checkpoints = { -60.0f, -36.0f, -23.0f, -18.0f, -14.0f, -9.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f };
        }
        return info;
    }

    // Getters pre PanelHost strip
    float getSmoothedDb()   const noexcept { return smoothedDbCache; }
    float getSessionMaxDb() const noexcept { return sessionMaxDb; }

    /** The old one-line readout. The MODULE no longer paints it — the host draws
        the three-line stack instead (see getModeName below) — but it is still
        maintained and still the single place the Trend recording state is worded. */
    juce::String getStripText() const noexcept { return stripText; }

    // ── The readout, as three separate pieces ────────────────────────────────
    //
    //  The meter used to print "-23.4 dBTP  MAX -0.9" into a strip along its own
    //  bottom edge — a line of text set in a 96 px column, which is why it read as
    //  "SdBP 6'9'1-" in anything but a wide module. The three things it was trying
    //  to say are handed out separately now and stacked by the host, one per line,
    //  where there is width for them.
    juce::String getModeName() const
    {
        return meterMode == MeterMode::RMS          ? "RMS"
             : meterMode == MeterMode::TruePeak     ? "True Peak"
             : meterMode == MeterMode::LUFS         ? "LUFS"
                                                    : "Level history";
    }

    /** The number on its own — no unit, no label. */
    juce::String getValueText() const
    {
        return smoothedDbCache <= -59.0f ? juce::String ("-inf")
                                         : juce::String (smoothedDbCache, 1);
    }

    /** The unit on its own. */
    juce::String getUnitText() const { return juce::String (unitSuffix()).trim(); }

    float getMinDb() const noexcept { return kScaleMinDb; }

    /** Top of the scale — the same for every mode, headroom included. */
    float getMaxDb() const noexcept { return kScaleMaxDb; }

    std::vector<float> getCheckpoints() const
    {
        if (meterMode == MeterMode::RMS)
            return { -60.0f, -48.0f, -36.0f, -24.0f, -18.0f, -12.0f, -9.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f };
        else if (meterMode == MeterMode::TruePeak)
            return { -60.0f, -24.0f, -18.0f, -12.0f, -9.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f };
        else // LUFS
            return { -60.0f, -36.0f, -23.0f, -18.0f, -14.0f, -9.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f };
    }

    void timerCallback() override
    {
        if (AlterTheme::hudFrozen.load()) return;   // HUD "Hold": keep last frame

        // ONE tone update per tick, and only while tone colour is on. Not in paint():
        // that also runs for repaints the module did not ask for (a resize, an
        // overlapping window, a Fusion pulling a frame), which would tie how fast the
        // colour follows the music to what else is on screen.
        if (colourByTone)
            toneHue.update (audioSource, toneTwist, toneSmooth);

        // --- Read input ---
        float incoming;
        bool incomingIsDb = false;
        if (meterMode == MeterMode::TruePeak)
            incoming = audioSource.getLastPeak();
        else if (meterMode == MeterMode::LUFS)
        { incoming = audioSource.getLastLufs(); incomingIsDb = true; }
        else
            incoming = audioSource.getLastRms();

        const float safeIncoming = incomingIsDb
            ? juce::jlimit (-100.0f, 6.0f, incoming)
            : juce::jlimit (0.0f, 10.0f, incoming);

        // Smoothing (for bar animation only)
        if (meterMode == MeterMode::RMS)
            smoothed = smoothed * alpha + safeIncoming * (1.0f - alpha);
        else if (meterMode == MeterMode::TruePeak)
        {
            if (safeIncoming > smoothed) smoothed = safeIncoming;
            else smoothed = smoothed * releaseCoeff + safeIncoming * (1.0f - releaseCoeff);
        }
        else
            smoothed = smoothed * alpha + safeIncoming * (1.0f - alpha);

        // Cache dB value for strip
        smoothedDbCache = incomingIsDb ? smoothed : linearToDb (smoothed);

        // ── PEAK / MAX HOLD ─────────────────────────────────────────────────
        //
        //  KEPT IN dB, ALWAYS. It used to be kept in each mode's native unit and
        //  decayed by multiplying — which works for a linear amplitude and is
        //  nonsense for a dB value, because dB are NEGATIVE. Multiplying -14 LUFS by
        //  0.99 moves it toward zero, i.e. the hold marker climbed instead of
        //  falling; and the "faded to nothing" test `peakHold < 0.001f → 0.0f` fired
        //  on the very first frame of decay, snapping a quiet LUFS reading straight
        //  to 0 LUFS, the top of the scale. In LUFS the hold did not fail to fall,
        //  it jumped to the ceiling and stayed there.
        //
        //  One unit, one hold time, one fall rate, all three modes. The fall is
        //  LINEAR IN dB (a constant dB/second), which is what a falling meter marker
        //  is everywhere else in audio, and it stops at the live level rather than
        //  continuing to -inf: a hold marker below the current reading is not
        //  holding anything.
        if (smoothedDbCache > peakHoldDb)
        {
            peakHoldDb  = smoothedDbCache;
            peakHoldAge = 0.0f;
        }
        else
        {
            peakHoldAge += 1.0f / 30.0f;
            if (peakHoldAge > kPeakHoldSec)
                peakHoldDb = juce::jmax (smoothedDbCache,
                                         peakHoldDb - kPeakFallDbPerSec / 30.0f);
        }
        if (smoothedDbCache > sessionMaxDb)
        {
            sessionMaxDb = smoothedDbCache;
            sessionMaxAgeSec = 0.0f;
        }
        else
        {
            // AUTO-RESET: if no new maximum arrived for kMaxHoldSec, re-arm the
            // MAX readout to the current level (double-click still resets instantly)
            sessionMaxAgeSec += 1.0f / 30.0f;
            if (sessionMaxAgeSec >= kMaxHoldSec)
            {
                sessionMaxDb = smoothedDbCache;
                sessionMaxAgeSec = 0.0f;
            }
        }

        // Update strip text
        bool isLufs = (meterMode == MeterMode::LUFS);
        if (meterMode == MeterMode::TruePeak)
        {
            // Momentary dBTP + session MAX (what DAW meters like Youlean/DPMeter
            // print as their big number) + sample peak when the module is wide
            // enough. Double-click the meter to reset MAX.
            if (smoothedDbCache <= -59.0f)
                stripText = juce::String ("-inf dBTP");
            else
                stripText = juce::String (smoothedDbCache, 1) + " dBTP";

            if (sessionMaxDb > -59.0f)
                stripText << "  MAX " << juce::String (sessionMaxDb, 1);

            if (getWidth() >= 170)
            {
                const float sp = audioSource.getLastSamplePeak();
                const float spDb = linearToDb (sp);
                if (spDb > -59.0f)
                    stripText << "  SP " << juce::String (spDb, 1);
            }
        }
        else if (smoothedDbCache <= -59.0f)
            stripText = isLufs ? juce::String ("-inf LUFS") : juce::String ("-inf dBFS");
        else
            stripText = juce::String (smoothedDbCache, 1) + (isLufs ? " LUFS" : " dBFS");

        // ── Trend capture (works for any MeterMode – records smoothedDbCache over time) ──
        if (trendMode && isMeasuring)
        {
            measureSmoothed = measureSmoothed * (1.0f - trendAlpha)
                            + smoothedDbCache * trendAlpha;
            measurementBuffer.push_back (measureSmoothed);
            measureSum += (double) measureSmoothed;

            const int n = (int) measurementBuffer.size();
            const float elapsed = (float) n / 30.0f;
            const int mins  = (int) (elapsed / 60.0f);
            const int secs  = (int) elapsed % 60;
            const float avg = (float) (measureSum / (double) juce::jmax (1, n));
            char buf[48];
            // The running average LIVE, so the number you are waiting for is already
            // settling in front of you instead of appearing only once you stop.
            std::snprintf (buf, sizeof (buf), "REC %02d:%02d  %.1f", mins, secs, avg);
            stripText = juce::String (buf);

            if (n >= kMaxMeasurePoints)
            {
                isMeasuring = false;  // auto-stop at 30 min
                char buf2[48];
                std::snprintf (buf2, sizeof (buf2), "DONE  AVG %.1f%s", avg, unitSuffix());
                stripText = juce::String (buf2);
            }
        }
        else if (trendMode && !isMeasuring)
        {
            const int n = (int) measurementBuffer.size();
            if (n > 0)
            {
                // STOPPED: the AVERAGE, not the sample count.
                //
                // The average was already being published — but only into the module
                // info line at the top right, which on a meter is the one place it
                // cannot be read: the module is 96 px wide by default and that line
                // spends its width on the name, the mode and the smoothing before it
                // ever reaches the number. Meanwhile the strip — centred, always
                // visible, and the thing you are looking at when you hit Stop — was
                // reporting how many POINTS were captured, which is of no use to
                // anyone once the measurement is over. The mean of the pass is what
                // the whole Trend view exists to produce, so that is what it says.
                const float elapsed = (float) n / 30.0f;
                const int mins = (int) (elapsed / 60.0f);
                const int secs = (int) elapsed % 60;
                const float avg = (float) (measureSum / (double) n);
                char buf[48];
                std::snprintf (buf, sizeof (buf), "%02d:%02d  AVG %.1f%s",
                               mins, secs, avg, unitSuffix());
                stripText = juce::String (buf);
            }
            else
            {
                stripText = "Press Start";
            }
        }

        // ── Level-history envelope capture (only while that mode is active) ──
        if (meterMode == MeterMode::LevelHistory)
        {
            ensureRing();
            const int count = audioSource.getLastWaveform (lhWaveL, lhWaveR);
            captureEnvelope (count);
        }

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        paintModuleBackground (g, bounds.toFloat());

        const int W = bounds.getWidth();
        const int H = bounds.getHeight();
        if (W <= 0 || H <= 0) return;

        // Level history takes the whole module area (DAW-style scrolling envelope).
        if (meterMode == MeterMode::LevelHistory)
        {
            g.setColour (AlterTheme::navyEdge);
            g.drawRect (bounds.toFloat(), 1.0f);

            // The SAME rectangle the bar views draw into — bounds.reduced(2), not the
            // raw module. The two have to be measured from the same edges or the dB
            // positions are offset by the inset no matter what the scale says, which
            // is the other half of why a history and a bar did not line up.
            lastBarArea = bounds.reduced (2);
            paintLevelHistory (g, lastBarArea.toFloat());
            return;
        }

        const float fontSize = juce::jlimit (8.0f, 13.0f, (float) juce::jmin (W, H) * 0.06f);
        const float scaleFontSz = juce::jlimit (7.0f, 10.0f, fontSize * 0.85f);

        // NO BOTTOM STRIP any more — the readout moved to the module info, stacked
        // one item per line (see getModeName / getValueText / getUnitText). A meter
        // is a 96 px column by default and a line of text set across it was
        // unreadable; the bar gets that height back instead.

        // --- BAR AREA: vertical, bottom = minDb, top = maxDb ---
        auto barArea = bounds.reduced (2);
        // Remembered for cursorText, which must measure the pointer against the
        // rectangle that was actually drawn rather than re-deriving it.
        lastBarArea = barArea;
        if (barArea.isEmpty()) return;

        const bool incomingIsDb = (meterMode == MeterMode::LUFS);
        const float smoothedDb = incomingIsDb ? smoothed : linearToDb (smoothed);

        const float minDb = getMinDb();
        const float maxDb = getMaxDb();

        auto normalizeDb = [minDb, maxDb](float db) -> float {
            return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
        };

        // Vertical bar: level fills from bottom upward
        const int barH = barArea.getHeight();

        g.saveState();
        g.reduceClipRegion (barArea);

        // The UNFILLED part of the bar — the track. It reads as part of the meter
        // on a HUD panel and as an opaque rectangle over someone's footage, and it
        // is drawn in a theme background colour precisely because that is what it
        // is. The lit portion still shows the level without it.
        if (! isTransparentBackground())
        {
            g.setColour (AlterTheme::bgVoid);
            g.fillRect (barArea);
        }

        // ── TWO BARS: level on the left, hold on the right ──────────────────
        //
        // The split is computed but NOT taken out of barArea, because the dB scale
        // and its tick lines belong to the module, not to either bar — they must
        // still run the full width behind both. Only the two fill calls are given
        // the narrowed rectangles.
        //
        // A hold column needs somewhere to be: below ~24 px of bar there is no width
        // to give away without leaving two slivers, so the meter quietly stays
        // single-bar rather than drawing something unreadable. The default meter is
        // 96 px wide, so this only bites when it has been squeezed hard.
        const int holdW = (twoBars && ! trendMode && barArea.getWidth() >= 24)
                        ? juce::jlimit (5, 18, barArea.getWidth() / 4)
                        : 0;
        const auto levelArea = (holdW > 0) ? barArea.withTrimmedRight (holdW + 1) : barArea;
        const auto holdArea  = barArea.withLeft (barArea.getRight() - holdW);

        // The hold bar shows the SAME quantity in the same units as the level bar,
        // so it goes through the same renderer — the zones line up across the two
        // columns and a peak sitting in the red is red in both.
        const float holdDb = peakHoldDb;

        auto renderBar = [&] (juce::Rectangle<int> area, float db)
        {
            if (area.getWidth() <= 0) return;
            if (meterMode == MeterMode::RMS)           renderRmsVertical  (g, area, db, normalizeDb);
            else if (meterMode == MeterMode::TruePeak) renderPeakVertical (g, area, db, normalizeDb);
            else                                       renderLufsVertical (g, area, db, normalizeDb);
        };

        if (trendMode)
        {
            renderTrendVertical (g, barArea, normalizeDb);
        }
        else
        {
            renderBar (levelArea, smoothedDb);
            if (holdW > 0)
            {
                renderBar (holdArea, holdDb);
                g.setColour (AlterTheme::bgDeep.withAlpha (0.9f));
                g.drawVerticalLine (holdArea.getX() - 1, (float) barArea.getY(), (float) barArea.getBottom());
            }
        }

        // --- PEAK HOLD marker (horizontal line showing max peak) ---
        // Skip in Trend view (irrelevant). With the hold bar on, the marker is drawn
        // over the LEVEL column only: extending it across the hold column would draw
        // a line along the top edge of a bar that already ends exactly there.
        if (!trendMode)
        {
            const float phNorm = normalizeDb (holdDb);
            if (phNorm > 0.001f && phNorm < 1.0f)
            {
                const float phY = (float) barArea.getBottom() - phNorm * (float) barH;
                g.setColour (AlterTheme::textBright.withAlpha (0.85f));
                g.drawHorizontalLine ((int) std::round (phY),
                                      (float) levelArea.getX(),
                                      (float) levelArea.getRight());
            }
        }

        // --- SCALE: horizontal tick lines + labels inside bar (responsive) ---
        const auto checkpoints = getCheckpoints();
        const float lblH = scaleFontSz + 2.0f;
        g.setFont (juce::Font (juce::FontOptions (scaleFontSz)));

        // Pre-compute Y positions for all checkpoints
        struct LabelInfo { float db; float yPos; };
        std::vector<LabelInfo> labels;
        for (float db : checkpoints)
        {
            if (db < minDb || db > maxDb) continue;
            float norm = normalizeDb (db);
            float yPos = (float) barArea.getBottom() - norm * (float) barH;
            labels.push_back ({ db, yPos });
        }

        // Draw tick lines always
        for (auto& li : labels)
        {
            g.setColour (AlterTheme::textNormal.withAlpha (li.db == 0.0f ? 0.5f : 0.22f));
            g.drawHorizontalLine ((int) std::round (li.yPos),
                                  (float) barArea.getX(),
                                  (float) barArea.getRight());
        }

        // Draw labels only where they don't overlap (skip if too close to neighbors)
        // Two passes: 1) mark which labels fit, 2) draw them
        // Priority: 0 dBFS always drawn first, then outward
        {
            const int n = (int) labels.size();
            std::vector<bool> draw (n, false);

            // First pass: mark 0 dBFS (or closest to it) as always drawn
            int zeroIdx = -1;
            for (int i = 0; i < n; ++i)
                if (std::abs (labels[i].db) < 0.01f) { zeroIdx = i; draw[i] = true; break; }

            // Second pass: greedily add labels that don't overlap with already-drawn ones
            auto canFit = [&](int idx) -> bool {
                for (int j = 0; j < n; ++j)
                    if (draw[j] && std::abs (labels[idx].yPos - labels[j].yPos) < lblH * 0.7f)
                        return false;
                return true;
            };

            // Priority: musically important thresholds first
            std::vector<float> priority = { 0.0f, -3.0f, -6.0f, -9.0f, -12.0f,
                                            3.0f, 6.0f, -14.0f, -18.0f, -23.0f,
                                            -24.0f, -36.0f, -48.0f, -60.0f };
            for (float pDb : priority)
            {
                for (int i = 0; i < n; ++i)
                {
                    if (!draw[i] && std::abs (labels[i].db - pDb) < 0.01f && canFit (i))
                    {
                        draw[i] = true;
                        break;
                    }
                }
            }

            for (int i = 0; i < n; ++i)
            {
                if (!draw[i]) continue;

                juce::String lbl;
                if      (labels[i].db == 0.0f) lbl = "0";
                else if (labels[i].db == 3.0f) lbl = "+3";
                else if (labels[i].db == 6.0f) lbl = "+6";
                else                           lbl = juce::String ((int) labels[i].db);

                g.setColour (AlterTheme::textNormal.withAlpha (0.85f));
                g.drawText (lbl,
                            barArea.getX() + 2, (int) std::round (labels[i].yPos) - (int)(lblH / 2),
                            (int)(scaleFontSz * 4.5f), (int) lblH,
                            juce::Justification::centredLeft, false);
            }
        }

        g.restoreState();

        // Border
        g.setColour (AlterTheme::navyEdge);
        g.drawRect (getLocalBounds(), 1);
    }

private:
    // =====================================================================
    // LEVEL HISTORY (ported from the Oscilloscope) — DAW-style scrolling
    // level envelope: peak silhouette (min/max) outline + RMS energy core.
    // Symmetry is intentionally NOT included here.
    // =====================================================================
    struct EnvSample { float minL = 0, maxL = 0, rmsL = 0, minR = 0, maxR = 0, rmsR = 0; };
    std::vector<EnvSample> lhHistory;
    int          lhWrite { 0 };
    float        lhWindowSec { 10.0f };
    DisplayMode  lhDisplay { DisplayMode::Mono };
    juce::Colour lhColour { juce::Colours::violet };
    bool         lhShowFill { false };
    bool         lhStereoAnalogous { false };   // false = complementary R hue
    float        lhAlpha { 0.0f };              // per-tick retention, from setSmoothAmount
    EnvSample    lhPrev  {};                    // last stored sample, for the smoother
    bool         lhSeeded { false };

    /** Absolute count of envelope samples ever captured, and how many of them are
        still in the ring. resampleEnv names samples by their ABSOLUTE position so a
        column of the picture keeps covering the same audio for as long as it is on
        screen — see the note there. Without these the only name a sample had was its
        offset from the moving write head, which is why the past kept redrawing. */
    long long    lhTotal  { 0 };
    int          lhFilled { 0 };
    // Pixel columns that reached full scale this frame (1 = over). See paintLevelHistory.
    std::vector<char> lhOverCols;

    /** Lay a red bar along both boundaries wherever the audio reached full scale.
        Drawn as RUNS, not per pixel: a fill call per column is 1000+ tiny rects on a
        wide module, and consecutive over-columns are the normal case. */
    void drawOverMarks (juce::Graphics& g, juce::Rectangle<float> area,
                        float cy, float fsOffset) const
    {
        const int W = (int) lhOverCols.size();
        if (W < 2 || area.getWidth() < 2.0f) return;

        const float pxW = area.getWidth() / (float) (W - 1);
        g.setColour (juce::Colours::red.withAlpha (0.85f));

        for (int x = 0; x < W; )
        {
            if (! lhOverCols[(size_t) x]) { ++x; continue; }
            const int start = x;
            while (x < W && lhOverCols[(size_t) x]) ++x;

            const float x0 = area.getX() + (float) start * pxW;
            const float w  = juce::jmax (1.5f, (float) (x - start) * pxW);
            g.fillRect (x0, cy - fsOffset - 1.0f, w, 2.5f);
            g.fillRect (x0, cy + fsOffset - 1.5f, w, 2.5f);
        }
    }
    std::vector<float> lhWaveL, lhWaveR;   // reused capture buffers

    /** Half-height the envelope is drawn against, as a fraction of the area.

        EXACTLY A HALF, and that is a geometric requirement rather than a taste.
        Level history IS two bar meters mirrored about the centre line: each half
        runs from -60 dB at the centre to the top of the scale at the edge, exactly
        as a bar runs from -60 at its bottom to the top of the scale at its top. So a
        given dB has to land at the same FRACTION of the travel in both, and any
        value below 0.5 makes the history's travel shorter than the half it occupies
        — every mark on it then sits slightly inboard of where the same mark sits on
        a bar. At 0.48 that was a ~2.4% shortfall: small, permanent, and visible the
        moment a history and a bar were put side by side.

        Everything that draws a boundary or reads a position goes through this and
        fullScaleUnit(), so there is one definition of where a dB is. */
    static constexpr float kEnvVScale = 0.5f;

    /** THE METER'S dB SCALE — the floor every view shares, and full scale.

        LEVEL HISTORY IS DRAWN ON IT TOO, WHICH IT WAS NOT.
        
        It used to plot LINEAR amplitude, and that is why the history and the bar
        beside it never agreed: a -20 dBFS passage is 0.1 in amplitude, so it drew as
        a 10%-high ripple while the True Peak bar next to it stood two thirds of the
        way up. Two views of the same signal, disagreeing — and the history is
        supposed to be the RECORDING of what the bar just did.
        
        Same floor as the bars (-60 dB), and full scale at the top: 0 dBFS lands
        exactly on the edge of the picture, where the trace clamps, so nothing is drawn above it. */
    static constexpr float kScaleMinDb = -60.0f;

    /** SIX dB OF HEADROOM ABOVE FULL SCALE, IN EVERY MODE.

        True Peak needs it because an inter-sample peak genuinely goes there. The
        others were topped out at 0 dBFS, and that is what made a Level history and
        a True Peak bar side by side disagree about where 0 dB IS: the history put
        it at the very top of its picture, the bar put it nine tenths of the way up,
        and the same signal drew at two different heights. One number for all of
        them is the only way those two can line up.

        It also gives the 0 dB boundary somewhere to be crossed. While the top of
        the scale WAS full scale, the boundary sat exactly on the clamp and nothing
        could ever be drawn past it; now over-scale material rises above the line
        for real, up to +6, and flattens there instead. */
    static constexpr float kScaleMaxDb =  +6.0f;

    /** Where 0 dBFS sits, as a fraction of the half-height — below 1 exactly
        because of the headroom above. Everything that draws the full-scale
        boundary asks this rather than assuming the edge of the picture. */
    static float fullScaleUnit() noexcept { return ampToUnit (1.0f); }

    /** Signed amplitude → signed position in -1..+1 on the dB scale above.
        Magnitude decides the dB, the sign decides which half of the picture. */
    static float ampToUnit (float a) noexcept
    {
        const float mag = std::abs (a);
        if (mag <= 1.0e-6f) return 0.0f;
        const float db = juce::jlimit (kScaleMinDb, kScaleMaxDb, 20.0f * std::log10 (mag));
        const float u  = (db - kScaleMinDb) / (kScaleMaxDb - kScaleMinDb);
        return a < 0.0f ? -u : u;
    }

    /** The inverse, for the hover readout: 0..1 up the half-height → dBFS. */
    static float unitToDb (float u) noexcept
    {
        return kScaleMinDb + juce::jlimit (0.0f, 1.0f, u) * (kScaleMaxDb - kScaleMinDb);
    }

    void captureEnvelope (int count)
    {
        if (lhHistory.empty()) return;
        EnvSample s;
        if (count > 0)
        {
            // The source's OWN rate, not a hard-coded 48 kHz. At 96 k that constant
            // took half a tick's worth of audio and threw the rest away, so half the
            // transients never reached the envelope at all — and WHICH half depended
            // on where the snapshot boundary happened to fall, which is a second,
            // quieter source of jitter on exactly the peaks this view is for.
            const double sr = juce::jmax (8000.0, audioSource.getSampleRate());
            const int n = juce::jmin (count, (int) (sr / (double) kEnvelopeRate) + 16);
            double sumL = 0.0, sumR = 0.0;
            for (int i = count - n; i < count; ++i)
            {
                const float l = lhWaveL[(size_t) i], r = lhWaveR[(size_t) i];
                s.minL = juce::jmin (s.minL, l);  s.maxL = juce::jmax (s.maxL, l);
                s.minR = juce::jmin (s.minR, r);  s.maxR = juce::jmax (s.maxR, r);
                sumL += l * l;  sumR += r * r;
            }
            s.rmsL = (float) std::sqrt (sumL / n);
            s.rmsR = (float) std::sqrt (sumR / n);
        }
        // ── SMOOTHING ────────────────────────────────────────────────────────
        //  Applied to what gets STORED, so the recorded trace is smoothed rather
        //  than the drawing of it — zoom in afterwards and the shape is the same
        //  shape, not a differently-filtered one.
        //
        //  Peaks rise instantly and fall at the time constant, the way the True
        //  Peak meter behaves and for the same reason: a plain average over the
        //  min/max pair would flatten exactly the transients the outline exists to
        //  show. The RMS core is an average already, so it takes a plain EMA both
        //  ways. At slider 0 the retention is ~0.03 per tick and this is within a
        //  hair of the raw capture it always was.
        if (lhSeeded)
        {
            s.maxL = juce::jmax (s.maxL, lhPrev.maxL * lhAlpha);
            s.maxR = juce::jmax (s.maxR, lhPrev.maxR * lhAlpha);
            s.minL = juce::jmin (s.minL, lhPrev.minL * lhAlpha);
            s.minR = juce::jmin (s.minR, lhPrev.minR * lhAlpha);
            s.rmsL = lhPrev.rmsL * lhAlpha + s.rmsL * (1.0f - lhAlpha);
            s.rmsR = lhPrev.rmsR * lhAlpha + s.rmsR * (1.0f - lhAlpha);
        }
        lhPrev   = s;
        lhSeeded = true;

        lhHistory[(size_t) lhWrite] = s;
        lhWrite = (lhWrite + 1) % (int) lhHistory.size();
        lhFilled = juce::jmin (lhFilled + 1, (int) lhHistory.size());
        ++lhTotal;
    }

    // Resample one channel to W pixels: min/max-reduction when zoomed out,
    // Catmull-Rom when zoomed in. channel: 0 = L, 1 = R, 2 = mono mix.
    void resampleEnv (int W, int channel, std::vector<float>& outMin,
                      std::vector<float>& outMax, std::vector<float>& outRms) const
    {
        const int total = (int) lhHistory.size();
        outMin.assign ((size_t) W, 0.0f);
        outMax.assign ((size_t) W, 0.0f);
        outRms.assign ((size_t) W, 0.0f);
        if (total < 2 || W < 2) return;

        // HOW MANY SAMPLES THE WIDTH NEEDS — not how many the window nominally has.
        //
        // This is the gap. The step must be a whole number of pixels, so a width of
        // W and a step of `s` draws W/s samples, and that is almost never exactly
        // the window's sample count: at 631 px a 10 s window (300 samples) rounds to
        // 2 px each, which fills the width with 315. Asking for only the window's
        // 300 left the last 30 px with nothing behind them — and only for the
        // windows where the rounding went that way, which is why some lengths
        // looked right and others did not.
        //
        // The ring holds the full 30 s regardless of the window, so those extra
        // samples are simply there to be used. Take what the width needs and let
        // lhVisibleSec report the span that actually results. The only thing that
        // limits it now is history that has genuinely not been recorded yet.
        const int have  = juce::jlimit (0, total, lhFilled);
        const int step0 = lhStep (W);
        const int need  = (step0 > 0) ? (W / step0) : lhWantedSamples();
        const int shown = juce::jmin (need, have);
        if (shown < 2) return;

        const long long oldestAbs = lhTotal - (long long) shown;   // first sample drawn

        // ABSOLUTE indexing: `abs` counts samples ever captured, so a given sample
        // keeps the same name for as long as it is on screen. The ring slot is
        // derived from it, rather than the other way round.
        auto atAbs = [&] (long long absIdx) -> const EnvSample&
        {
            const long long delta = lhTotal - absIdx;              // 1 = newest
            const int idx = (int) (((lhWrite - delta) % total + total) % total);
            return lhHistory[(size_t) idx];
        };
        auto minOf = [&] (const EnvSample& e)
        { return channel == 0 ? e.minL : channel == 1 ? e.minR : juce::jmin (e.minL, e.minR); };
        auto maxOf = [&] (const EnvSample& e)
        { return channel == 0 ? e.maxL : channel == 1 ? e.maxR : juce::jmax (e.maxL, e.maxR); };
        auto rmsOf = [&] (const EnvSample& e)
        { return channel == 0 ? e.rmsL : channel == 1 ? e.rmsR : 0.5f * (e.rmsL + e.rmsR); };

        const float samplesPerPx = (float) shown / (float) W;

        if (samplesPerPx >= 1.0f)
        {
            // ── BINS ANCHORED TO ABSOLUTE SAMPLE POSITIONS ──────────────────
            //
            //  This is recorded history. Once a column has been drawn it must never
            //  change again, and it did: the bins used to be `x * samplesPerPx`
            //  offsets into an array indexed from the MOVING write head. Every tick
            //  shifted the whole array by one sample while the bin edges stayed at
            //  the same fractional offsets, so the set of samples landing in a given
            //  column changed each frame in a way that is not a clean scroll. A
            //  sharp peak hopped between neighbouring bins and its column jumped up
            //  and down — the past visibly rewriting itself, worst exactly where the
            //  signal changes fastest.
            //
            //  Anchoring the bins to absolute positions fixes it by construction: a
            //  completed bin covers a fixed set of samples forever, so it draws the
            //  same value forever and the picture only scrolls. Same reasoning, and
            //  the same shape of fix, as the Oscilloscope's Long Waveform.
            const int       binSize   = juce::jmax (1, (int) std::lround (samplesPerPx));
            const long long newestBin = (lhTotal - 1) / binSize;

            for (int x = 0; x < W; ++x)
            {
                const long long bin = newestBin - (long long) (W - 1 - x);
                if (bin < 0) continue;

                long long s0 = bin * (long long) binSize;
                long long s1 = s0 + (long long) binSize;
                s0 = juce::jmax (s0, oldestAbs);
                s1 = juce::jmin (s1, lhTotal);
                if (s0 >= s1) continue;         // not yet filled, or scrolled out

                float lo = 0.0f, hi = 0.0f, rm = 0.0f;
                for (long long a = s0; a < s1; ++a)
                {
                    const auto& e = atAbs (a);
                    lo = juce::jmin (lo, minOf (e));
                    hi = juce::jmax (hi, maxOf (e));
                    rm = juce::jmax (rm, rmsOf (e));
                }
                outMin[(size_t) x] = lo;
                outMax[(size_t) x] = hi;
                outRms[(size_t) x] = rm;
            }
        }
        else
        {
            // ── ZOOMED IN: A WHOLE NUMBER OF PIXELS PER SAMPLE ───────────────
            //
            //  This is what was left of the trembling. The bins above are stable and
            //  the past no longer changes, but the picture still advanced by
            //  W / samples pixels per update — 2.94 on a 1810 px module holding 615
            //  samples — and a fractional scroll means every vertex lands on a new
            //  sub-pixel position each frame. The anti-aliasing is then recomputed
            //  for all of them, so the whole trace jitters by about a pixel, thirty
            //  times a second, along its entire length. One pixel is small; one pixel
            //  on two thousand columns at 30 Hz is a trace with current running
            //  through it, and it is worst on steep edges because that is where a
            //  sub-pixel horizontal shift moves the most ink vertically.
            //
            //  Rounding the spacing to whole pixels fixes it at the root: samples sit
            //  on a fixed integer grid, an update moves everything by exactly `step`
            //  whole pixels, and no vertex ever changes its fractional position, so
            //  there is nothing left for the rasteriser to redraw differently.
            //
            //  It costs up to half a pixel per sample of time-axis accuracy — the
            //  visible span becomes (W / step) samples instead of `filled`, which is
            //  why the time label asks lhVisibleSec() rather than printing the
            //  requested window. Two percent on the axis, in exchange for a trace
            //  that holds still.
            // The SAME step the time label and the hover readout use, so all three
            // agree about how much time a pixel is worth.
            const int step = juce::jmax (1, step0);

            auto lerp = [] (float a, float b, float t) { return a + (b - a) * t; };

            for (int x = 0; x < W; ++x)
            {
                const int back = W - 1 - x;            // whole pixels back from "now"
                const int si   = back / step;          // whole samples back
                const float t  = (float) (back % step) / (float) step;

                const long long a0 = lhTotal - 1 - (long long) si;   // on the grid
                const long long a1 = a0 - 1;                          // one older
                if (a0 < oldestAbs) continue;                         // no history yet

                const auto& e0 = atAbs (a0);
                const auto& e1 = atAbs (a1 >= oldestAbs ? a1 : a0);

                outMin[(size_t) x] = lerp (minOf (e0), minOf (e1), t);
                outMax[(size_t) x] = lerp (maxOf (e0), maxOf (e1), t);
                outRms[(size_t) x] = lerp (rmsOf (e0), rmsOf (e1), t);
            }
        }
    }

    // outline = true min/max waveform shape; filled bright core = RMS energy.
    // mirror = symmetric |peak| silhouette (top/bottom mirrored), like Short-Term Scope.
    void drawEnvelope (juce::Graphics& g, juce::Rectangle<float> area,
                       const std::vector<float>& mn, const std::vector<float>& mx,
                       const std::vector<float>& rms, float alpha, juce::Colour col,
                       bool mirror = false) const
    {
        const int W = (int) mn.size();
        if (W < 2) return;
        const float cy = area.getCentreY();
        const float vScale = area.getHeight() * kEnvVScale;
        auto xAt = [&] (int x) { return area.getX() + (float) x / (float) (W - 1) * area.getWidth(); };

        // ampToUnit, not a raw clamp: the vertical axis is dB (see kScaleMinDb), so a
        // quiet passage occupies the same height here as it does on the bar.
        auto buildBand = [&] (auto topVal, auto botVal) -> juce::Path
        {
            juce::Path p;
            for (int x = 0; x < W; ++x)
            {
                const float py = cy - ampToUnit (topVal (x)) * vScale;
                if (x == 0) p.startNewSubPath (xAt (x), py); else p.lineTo (xAt (x), py);
            }
            for (int x = W - 1; x >= 0; --x)
                p.lineTo (xAt (x), cy - ampToUnit (botVal (x)) * vScale);
            p.closeSubPath();
            return p;
        };

        juce::Path outline, core;
        if (mirror)
        {
            // symmetric |peak| silhouette: top = +peak, bottom = -peak (mirrored)
            auto pk = [&] (int x) { return juce::jmax (std::abs (mn[(size_t) x]), std::abs (mx[(size_t) x])); };
            outline = buildBand ([&] (int x) { return  pk (x); },
                                 [&] (int x) { return -pk (x); });
            core    = buildBand ([&] (int x) { return  rms[(size_t) x]; },
                                 [&] (int x) { return -rms[(size_t) x]; });
        }
        else
        {
            // true waveform shape (asymmetric, like a DAW clip overview)
            outline = buildBand ([&] (int x) { return mx[(size_t) x]; },
                                 [&] (int x) { return mn[(size_t) x]; });
            core    = buildBand ([&] (int x) { return juce::jmin (rms[(size_t) x],  mx[(size_t) x]); },
                                 [&] (int x) { return juce::jmax (-rms[(size_t) x], mn[(size_t) x]); });
        }

        if (lhShowFill)
        {
            g.setColour (col.withAlpha (0.18f * alpha));
            g.fillPath (outline);
        }
        g.setColour (col.withAlpha (0.45f * alpha));
        g.strokePath (outline, juce::PathStrokeType (1.2f));
        g.setColour (col.withAlpha (0.30f * alpha));
        g.fillPath (core);
        g.setColour (col.withAlpha (alpha));
        g.strokePath (core, juce::PathStrokeType (2.0f));
    }

    /** Seconds actually on screen. Rounding the sample spacing to whole pixels (see
        resampleEnv) means the visible span is a whole number of samples per column
        group rather than exactly lhWindowSec — usually within a couple of percent.
        The axis prints THIS, because a label that states the requested window while
        the picture shows a slightly different one is a measuring instrument telling
        a small lie for the sake of a round number. */
    /** How many envelope samples the requested window asks for, capped by what has
        actually been recorded. Below this the picture is genuinely short of history
        — the app has not been running long enough — and columns with nothing behind
        them are left empty rather than invented. */
    int lhWantedSamples() const noexcept
    {
        const int want = juce::jmax (2, (int) std::lround ((double) lhWindowSec * (double) kEnvelopeRate));
        return juce::jmin (want, (int) lhHistory.size());
    }

    /** Whole pixels per sample when zoomed in, else 0 (one column per pixel).

        The step has to be a whole number of pixels or the trace shimmers (see
        resampleEnv), which means the window length is QUANTISED: only spans of
        W / step samples can be drawn exactly. Of the two integer steps either side
        of the ideal, this picks the one whose SPAN lands closer to what was asked
        for — rounding the step itself, as it did, minimises the wrong quantity and
        can be 25% out on the window. */
    int lhStep (int W) const noexcept
    {
        const int want = lhWantedSamples();
        if (want < 2 || W < 2) return 0;
        if (want >= W) return 0;                       // one column per pixel

        const int lo = juce::jmax (1, W / want);       // floor
        const int hi = lo + 1;
        return (std::abs (W / lo - want) <= std::abs (W / hi - want)) ? lo : hi;
    }

    float lhVisibleSec (int W) const noexcept
    {
        const int step = lhStep (W);
        if (step <= 0) return (float) lhWantedSamples() / (float) kEnvelopeRate;
        return (float) (W / step) / (float) kEnvelopeRate;
    }

    void drawLhTimeLabels (juce::Graphics& g, juce::Rectangle<float> bounds, float spanSec) const
    {
        g.setColour (juce::Colours::lightgrey.withAlpha (0.7f));
        g.setFont (11.0f);
        const int lblH = 14;
        const int lblY = (int) bounds.getBottom() - lblH - 1;
        g.drawText ("-" + juce::String (spanSec, spanSec < 10.0f ? 1 : 0) + " s",
                    (int) bounds.getX() + 3, lblY, 60, lblH, juce::Justification::centredLeft);
        g.drawText ("now", (int) bounds.getRight() - 44, lblY, 40, lblH, juce::Justification::centredRight);
    }

    // ── CLIPPING ZONE ────────────────────────────────────────────────────────
    //
    //  The envelope is drawn in LINEAR amplitude against a fixed half-height, so
    //  0 dBFS — full scale, the ceiling — is not somewhere approximate: it is
    //  fullScaleUnit() of the way out from the centre line, on the same dB scale the
    //  bars use. Without that mark the view has no absolute reference at all, and a
    //  loud take and a quiet one draw the same shape.    //
    //  THE TRACE IS CLAMPED TO FULL SCALE, ON PURPOSE. That is what a DAW does with a
    //  clip overview, and the flat plateau it produces is how an engineer reads "this
    //  is slammed" at a glance. Letting the trace run past the boundary would buy one
    //  number — HOW far over — at the cost of shrinking every waveform to leave room
    //  for the overshoot, and that number belongs on a meter: True Peak reports it in
    //  dBTP and also catches inter-sample peaks, which a sample-domain view cannot see
    //  at all.
    //
    //  What the clamp costs is that a column 6 dB over looks identical to one that
    //  merely touched. So the over-scale columns are MARKED instead — see
    //  drawOverMarks. Same information, no cost to the picture.
    //
    //  The ruler down the right answers the other question the boundary raises:
    //  where is everything else — including the +6 of headroom above it.
    void drawClipZone (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        if (! showClipZone) return;
        if (area.getHeight() < 40.0f || area.getWidth() < 60.0f) return;

        const float cy     = area.getCentreY();
        const float vScale = area.getHeight() * kEnvVScale;

        // THE MODULE'S OWN COLOUR, not a fixed red. The line is a SCALE MARK, not an
        // alarm — it says where 0 dBFS is whether or not anything is near it, and a
        // red rule burning across a violet envelope reads as a warning that is
        // permanently on. Drawn from the same colour as the trace (so it follows the
        // note in tone mode too), brightened and dashed so it stays legible against
        // the waveform it crosses without pretending to be part of it.
        // NOT the edge of the picture any more: the scale carries 6 dB above full
        // scale, so 0 dBFS is fullScaleUnit() of the way out and there is real room
        // beyond it for something to cross into.
        const float fsOffset = fullScaleUnit() * vScale;

        const auto clipCol = activeLhColour().brighter (0.4f).withAlpha (0.75f);
        g.setColour (clipCol);
        for (int sign = -1; sign <= 1; sign += 2)
        {
            const float y = cy - (float) sign * fsOffset;
            // Dashed, so it never reads as part of the waveform it crosses.
            const float dash[] = { 5.0f, 4.0f };
            g.drawDashedLine ({ area.getX(), y, area.getRight(), y }, dash, 2, 1.0f);
        }

        // ── OVER-SCALE MARKS ────────────────────────────────────────────────
        //  A solid red bar laid along the boundary exactly where the audio reached
        //  it — the clip overlay a DAW puts on a hot region. Red HERE and nowhere
        //  else in this view: the boundary line itself is a scale mark and stays the
        //  module's colour, so red never appears unless something hit the ceiling.
        drawOverMarks (g, area, cy, fsOffset);

        // Right-edge dB ruler. Only the marks that have room: below ~90 px of
        // half-height the labels would touch, and four overlapping numbers say less
        // than one clean one.
        static const float marks[]  = { 0.0f, 6.0f, -6.0f, -12.0f, -20.0f, -30.0f, -40.0f };
        const int          nMarks   = (area.getHeight() * kEnvVScale > 60.0f) ? 7
                                    : (area.getHeight() * kEnvVScale > 40.0f) ? 4 : 1;
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        for (int i = 0; i < nMarks; ++i)
        {
            // Straight off the same dB mapping the trace uses, so a label and the
            // waveform beside it mean the same thing.
            const float y = cy - ampToUnit (std::pow (10.0f, marks[i] / 20.0f)) * vScale;
            const bool  isZero = (i == 0);
            g.setColour (isZero ? clipCol : AlterTheme::textNormal.withAlpha (0.45f));
            if (! isZero)
                g.drawHorizontalLine ((int) std::round (y), area.getRight() - 6.0f, area.getRight());
            g.drawText (isZero ? juce::String ("0 dB")
                       : marks[i] > 0.0f ? "+" + juce::String ((int) marks[i])
                                         : juce::String ((int) marks[i]),
                        (int) area.getRight() - 42, (int) y + 1, 38, 11,
                        juce::Justification::centredRight, false);
        }
    }

    void paintLevelHistory (juce::Graphics& g, juce::Rectangle<float> bounds)
    {
        // ONE COLUMN PER PIXEL OF THE AREA BEING DRAWN INTO — not of the module.
        //
        // These two were the same thing until the drawing area became
        // bounds.reduced(2) and this stayed getWidth(). Four pixels of difference,
        // and drawEnvelope maps index x to `x / (W-1) * area.getWidth()`, so each
        // array index landed 0.97 px apart instead of 1.00. The bins advance by
        // exactly one index at a time, so a sharp peak then stepped across the
        // raster in fractional amounts and its anti-aliased shape changed on every
        // step: a peak that visibly trembles as it scrolls, worst where the curve is
        // steepest. Sampling one column per destination pixel makes index and pixel
        // the same thing again, and the scroll lands on whole pixels.
        const int W = juce::jmax (8, (int) std::lround (bounds.getWidth()));
        std::vector<float> mnA, mxA, rmsA, mnB, mxB, rmsB;

        // Which pixel columns reached full scale. drawEnvelope clamps to ±1.0, so a
        // column that went over draws as a flat top ON the boundary and looks the
        // same as one that merely touched it; this is what tells them apart.
        // Cleared per frame, ORed across whichever channels are shown.
        lhOverCols.assign ((size_t) W, 0);
        auto markOver = [this] (const std::vector<float>& mn, const std::vector<float>& mx)
        {
            const size_t n = juce::jmin (mn.size(), mx.size(), lhOverCols.size());
            for (size_t x = 0; x < n; ++x)
                if (mx[x] >= 1.0f || mn[x] <= -1.0f) lhOverCols[x] = 1;
        };

        const juce::Colour lhCol = activeLhColour();
        // Same two options, same hue distances and the same meaning as the
        // Spectrum's and the Oscilloscope's stereo channels.
        const juce::Colour rCol = lhCol.withRotatedHue (lhStereoAnalogous ? 0.12f : 0.45f);

        if (lhDisplay == DisplayMode::MirrorStereo)
        {
            resampleEnv (W, 0, mnA, mxA, rmsA);
            resampleEnv (W, 1, mnB, mxB, rmsB);
            markOver (mnA, mxA);  markOver (mnB, mxB);
            auto top    = bounds.withHeight (bounds.getHeight() * 0.5f);
            auto bottom = top.translated (0.0f, top.getHeight());
            drawEnvelope (g, top,    mnA, mxA, rmsA, 1.0f,  lhCol);
            drawEnvelope (g, bottom, mnB, mxB, rmsB, 0.75f, lhCol);
            // Each half is its own picture with its own centre line, so the clip
            // boundary has to be drawn per half — one pair across the whole module
            // would sit at ±0 dBFS of a scale neither channel is drawn against.
            drawClipZone (g, top);
            drawClipZone (g, bottom);
        }
        else if (lhDisplay == DisplayMode::Stereo)
        {
            resampleEnv (W, 0, mnA, mxA, rmsA);
            resampleEnv (W, 1, mnB, mxB, rmsB);
            markOver (mnA, mxA);  markOver (mnB, mxB);
            drawEnvelope (g, bounds, mnA, mxA, rmsA, 1.0f,  lhCol);
            drawEnvelope (g, bounds, mnB, mxB, rmsB, 0.85f, rCol);
            drawClipZone (g, bounds);
        }
        else // Mono = true shape; Mirror = symmetric |peak| silhouette
        {
            resampleEnv (W, 2, mnA, mxA, rmsA);
            markOver (mnA, mxA);
            drawEnvelope (g, bounds, mnA, mxA, rmsA, 1.0f, lhCol,
                          lhDisplay == DisplayMode::Mirror);
            drawClipZone (g, bounds);
        }

        g.setColour (lhCol.withAlpha (0.25f));
        g.fillRect (bounds.getX(), bounds.getCentreY() - 0.5f, bounds.getWidth(), 1.0f);
        drawLhTimeLabels (g, bounds, lhVisibleSec (W));
    }

    // ── Where every lit colour in this module comes from ────────────────────
    //
    //  A meter is a LADDER of colours, not one colour: safe, warning, hot, clip.
    //  Three renderers (bar, hold bar, trend curve) times four zones times three
    //  colour modes used to mean twelve hand-written `withBrightness(0.7f)` calls
    //  that had to be kept in step by hand — and they were not: the vertical
    //  renderers silently dropped the second custom branch the horizontal ones
    //  had, so picking it quietly drew the gradient instead.
    //
    //  Now the ladder is defined ONCE, here. Renderers ask for a rung by position
    //  and never name a colour operation themselves.
    //
    //  (The horizontal render*Mode variants that used to live here were dead — the
    //  meter has drawn bottom-to-top since it became a column, and paint() called
    //  only the vertical set. They are gone rather than left as a fourth copy to
    //  keep in step.)

    /** The colour the ladder is built from: the note in tone mode, the user's pick
        otherwise. */
    juce::Colour baseColour() const noexcept
    {
        return (colourByTone && toneHue.hasTone()) ? toneHue.colour() : barColour;
    }

    /** True when the zones are derived from a base colour rather than being the
        fixed DAW green/yellow/orange/red. */
    bool usesDerivedZones() const noexcept
    {
        return colourByTone || colorMode != ColorMode::Standard;
    }

    /** Gradient, or walk toward the complement? ONE question, asked of whichever
        setting is in charge — the tone shade while colour-by-tone is on, the colour
        mode otherwise. The renderers never need to know which. */
    bool useComplementaryZones() const noexcept
    {
        return colourByTone ? (toneShade  == ToneShade::Complementary)
                            : (colorMode  == ColorMode::CustomComplementary);
    }

    /** One rung of the ladder.

        @param position  1.0 = the base colour itself (the quietest zone), falling
                         toward 0 as the zone gets hotter.

        THE STEPS ARE DELIBERATELY LARGE. The first version moved the hue by 13-30°
        between zones and dimmed by a few percent, which is a difference you can
        measure and not one you can see — the bar looked like one flat colour and the
        zones may as well not have existed. A loudness zone is a THRESHOLD; crossing
        it has to be obvious at a glance, across a room, on a 96 px column. So:

          Gradient      — brightness 1.0 → 0.28. Same hue throughout, so the note (or
                          the picked colour) is never in doubt, but each zone is
                          plainly darker than the one below it.
          Complementary — up to a HALF-TURN of the wheel at the hottest zone, at
                          undimmed brightness, so the top of the bar is a different
                          colour rather than a darker one. */
    juce::Colour zoneTint (float position) const noexcept
    {
        const auto  base = baseColour();
        position = juce::jlimit (0.0f, 1.0f, position);
        const float step = 1.0f - position;          // 0 at the base, →1 at the hottest

        if (useComplementaryZones())
            return base.withRotatedHue (0.5f * step);

        return base.withBrightness (juce::jlimit (0.28f, 1.0f, position));
    }

    // ── The zone ladder, defined ONCE per meter mode ────────────────────────
    //
    //  Every mode has the same NUMBER of zones in derived colour as it has in
    //  Standard, and at the same thresholds. RMS did not: Standard drew three
    //  (green to -12, yellow to -6, orange to 0) and the derived path drew two,
    //  because it only ever checked -6. So switching a meter to a custom colour
    //  silently dropped a threshold — the -12 boundary vanished, and the reported
    //  "the zones do not react" was that missing step plus tints too close to tell
    //  apart. Bar and Trend both read this, so they cannot disagree either.
    struct Zone { float aboveDb; float tint; };

    /** Ordered LOUDEST FIRST; the first one whose threshold `db` exceeds wins.
        A value below every threshold is the base colour. */
    const std::vector<Zone>& zoneLadder() const
    {
        static const std::vector<Zone> rms  { { 0.0f, 0.28f }, { -6.0f, 0.46f }, { -12.0f, 0.68f } };
        static const std::vector<Zone> peak { {  0.0f, 0.28f }, {  -3.0f, 0.62f } };
        static const std::vector<Zone> lufs { { -9.0f, 0.28f }, { -14.0f, 0.46f }, { -23.0f, 0.68f } };

        return meterMode == MeterMode::RMS      ? rms
             : meterMode == MeterMode::TruePeak ? peak
                                                : lufs;
    }

    /** The colour of the level-history / envelope trace. Same rule as the bar: the
        note when tone colour is on, the picked colour otherwise. */
    juce::Colour activeLhColour() const noexcept
    {
        return (colourByTone && toneHue.hasTone()) ? toneHue.colour() : lhColour;
    }

    // ── Vertical render helpers (bottom-to-top) ────────────────────────

    /** DERIVED COLOUR, all three meter modes, one implementation.
        
        Walks zoneLadder() from the quietest band upward, so the bar shows exactly
        the same number of bands at exactly the same thresholds as Standard does.
        The three modes used to write this out by hand and RMS's copy checked one
        threshold fewer than its own Standard branch, which is why switching a meter
        to a custom colour quietly lost the -12 dB step. */
    void renderDerivedZones (juce::Graphics& g, juce::Rectangle<int> r,
                             int fillTop, const std::function<int(float)>& dbToY)
    {
        const auto& ladder = zoneLadder();
        if (ladder.empty()) return;

        int bandBottom = r.getBottom();

        // Base band: everything below the quietest threshold.
        {
            const int yTop = juce::jmax (fillTop, dbToY (ladder.back().aboveDb));
            if (yTop < bandBottom)
            {
                g.setColour (baseColour());
                g.fillRect (r.getX(), yTop, r.getWidth(), bandBottom - yTop);
            }
            bandBottom = juce::jmin (bandBottom, yTop);
        }

        // Then each rung, quietest first, each one reaching up to the next threshold
        // above it (or the top of the scale for the loudest).
        for (int i = (int) ladder.size() - 1; i >= 0; --i)
        {
            const float topDb = (i == 0) ? getMaxDb() : ladder[(size_t) (i - 1)].aboveDb;
            const int   yTop  = juce::jmax (fillTop, dbToY (topDb));
            if (yTop < bandBottom)
            {
                g.setColour (zoneTint (ladder[(size_t) i].tint));
                g.fillRect (r.getX(), yTop, r.getWidth(), bandBottom - yTop);
            }
            bandBottom = juce::jmin (bandBottom, yTop);
        }
    }

    void renderRmsVertical (juce::Graphics& g, juce::Rectangle<int> r,
                            float smoothedDb, std::function<float(float)> normalizeDb)
    {
        const int barH = r.getHeight();

        std::function<int(float)> dbToY = [&](float db) -> int {
            return r.getBottom() - (int)(normalizeDb (db) * barH);
        };

        const int fillBottom = r.getBottom();
        const int fillTop    = r.getBottom() - (int)(normalizeDb (smoothedDb) * barH);
        if (fillTop >= fillBottom) return;

        if (usesDerivedZones()) { renderDerivedZones (g, r, fillTop, dbToY); return; }

        const int y_m12 = dbToY (-12.0f);
        const int y_m6  = dbToY (-6.0f);
        const int y_0   = dbToY (0.0f);

        // Green zone: -60 to -12
        {
            int top = juce::jmax (fillTop, y_m12);
            if (top < fillBottom)
            {
                g.setColour (juce::Colours::limegreen);
                g.fillRect (r.getX(), top, r.getWidth(), fillBottom - top);
            }
        }
        // Yellow zone: -12 to -6
        if (fillTop < y_m12)
        {
            int top = juce::jmax (fillTop, y_m6);
            if (top < y_m12)
            {
                g.setColour (juce::Colour (0xFFFFDD00));
                g.fillRect (r.getX(), top, r.getWidth(), y_m12 - top);
            }
        }
        // Orange zone: -6 to 0
        if (fillTop < y_m6)
        {
            int top = juce::jmax (fillTop, y_0);
            if (top < y_m6)
            {
                g.setColour (juce::Colours::orange);
                g.fillRect (r.getX(), top, r.getWidth(), y_m6 - top);
            }
        }
        // Above 0: over full scale. RMS could not reach this before — its scale
        // stopped at 0 dBFS — so the band simply did not exist and anything up here
        // would have drawn as bare track. With the headroom it can, and does: an RMS
        // above full scale means samples beyond ±1.0, which is worth shouting about.
        if (fillTop < y_0)
        {
            g.setColour (juce::Colours::red.withAlpha (0.95f));
            g.fillRect (r.getX(), fillTop, r.getWidth(), y_0 - fillTop);
        }
    }

    void renderPeakVertical (juce::Graphics& g, juce::Rectangle<int> r,
                             float smoothedDb, std::function<float(float)> normalizeDb)
    {
        const int barH = r.getHeight();

        std::function<int(float)> dbToY = [&](float db) -> int {
            return r.getBottom() - (int)(normalizeDb (db) * barH);
        };

        const int fillTop = r.getBottom() - (int)(normalizeDb (smoothedDb) * barH);
        if (fillTop >= r.getBottom()) return;

        if (usesDerivedZones()) { renderDerivedZones (g, r, fillTop, dbToY); return; }

        const int y_m3 = dbToY (-3.0f);
        const int y_0  = dbToY (0.0f);

        // Below -3: safe
        int safeTop = juce::jmax (fillTop, y_m3);
        if (safeTop < r.getBottom())
        {
            g.setColour (baseColour());
            g.fillRect (r.getX(), safeTop, r.getWidth(), r.getBottom() - safeTop);
        }
        // -3 to 0: warning
        if (fillTop < y_m3)
        {
            int wTop = juce::jmax (fillTop, y_0);
            if (wTop < y_m3)
            {
                g.setColour (juce::Colours::yellow.withAlpha (0.9f));
                g.fillRect (r.getX(), wTop, r.getWidth(), y_m3 - wTop);
            }
        }
        // Above 0: clip
        if (fillTop < y_0)
        {
            g.setColour (juce::Colours::red.withAlpha (0.95f));
            g.fillRect (r.getX(), fillTop, r.getWidth(), y_0 - fillTop);
        }
    }

    void renderLufsVertical (juce::Graphics& g, juce::Rectangle<int> r,
                             float smoothedDb, std::function<float(float)> normalizeDb)
    {
        const int barH = r.getHeight();

        std::function<int(float)> dbToY = [&](float db) -> int {
            return r.getBottom() - (int)(normalizeDb (db) * barH);
        };

        const int fillTop = r.getBottom() - (int)(normalizeDb (smoothedDb) * barH);
        if (fillTop >= r.getBottom()) return;

        if (usesDerivedZones()) { renderDerivedZones (g, r, fillTop, dbToY); return; }

        const int y_23 = dbToY (-23.0f);
        const int y_14 = dbToY (-14.0f);
        const int y_9  = dbToY (-9.0f);

        // Below -23: safe
        int top = juce::jmax (fillTop, y_23);
        if (top < r.getBottom())
        {
            g.setColour (baseColour());
            g.fillRect (r.getX(), top, r.getWidth(), r.getBottom() - top);
        }
        // -23 to -14
        if (fillTop < y_23)
        {
            int t = juce::jmax (fillTop, y_14);
            if (t < y_23)
            {
                g.setColour (juce::Colours::yellow.withAlpha (0.9f));
                g.fillRect (r.getX(), t, r.getWidth(), y_23 - t);
            }
        }
        // -14 to -9
        if (fillTop < y_14)
        {
            int t = juce::jmax (fillTop, y_9);
            if (t < y_14)
            {
                g.setColour (juce::Colours::orange.withAlpha (0.9f));
                g.fillRect (r.getX(), t, r.getWidth(), y_14 - t);
            }
        }
        // Above -9
        if (fillTop < y_9)
        {
            g.setColour (juce::Colours::red.withAlpha (0.95f));
            g.fillRect (r.getX(), fillTop, r.getWidth(), y_9 - fillTop);
        }
    }

    // ── Trend view: waveform of dB over time, zone-colored ─────────────
    void renderTrendVertical (juce::Graphics& g, juce::Rectangle<int> r,
                              std::function<float(float)> normalizeDb)
    {
        const int W = r.getWidth();
        const int H = r.getHeight();
        const int n = (int) measurementBuffer.size();

        if (n == 0)
        {
            g.setColour (juce::Colours::grey.withAlpha (0.5f));
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText ("Press Start", r, juce::Justification::centred, false);
            return;
        }

        // Zone colour for a given dB value. It asks zoneTint() for exactly the
        // positions the bar renderers ask for, at exactly the same thresholds, so
        // the curve is coloured by the same ladder as the bar it replaces — and
        // colour by tone, gradient and spectrum all reach the Trend view for free
        // instead of needing a fourth copy of the rules here.
        auto zoneColour = [this](float db) -> juce::Colour
        {
            // In derived colour the curve walks the SAME ladder the bar does, so a
            // Trend recording crosses its thresholds in exactly the colours the bar
            // showed live — and RMS gets its -12 boundary here too, which the old
            // two-branch version dropped along with the bar's.
            if (usesDerivedZones())
            {
                for (const auto& z : zoneLadder())
                    if (db > z.aboveDb)
                        return zoneTint (z.tint);
                return baseColour();
            }

            if (meterMode == MeterMode::RMS)
            {
                if (db > 0.0f)   return juce::Colours::red;
                if (db > -6.0f)  return juce::Colours::orange;
                if (db > -12.0f) return juce::Colour (0xFFFFDD00);
                return juce::Colours::limegreen;
            }
            else if (meterMode == MeterMode::TruePeak)
            {
                if (db > 0.0f)  return juce::Colours::red;
                if (db > -3.0f) return juce::Colours::yellow;
                return baseColour();
            }
            else // LUFS
            {
                if (db > -9.0f)  return juce::Colours::red;
                if (db > -14.0f) return juce::Colours::orange;
                if (db > -23.0f) return juce::Colour (0xFFFFDD00);
                return baseColour();
            }
        };

        const float xScale = (float) W / (float) juce::jmax (n - 1, 1);
        const float lineAlpha = isMeasuring ? 0.92f : 0.65f;

        // Draw zone-coloured fill segments and stroke in one pass.
        // To avoid massive path count, batch consecutive points of same zone.
        auto getX = [&](int i) { return (float) r.getX() + (float) i * xScale; };
        auto getY = [&](int i)
        {
            float norm = normalizeDb (measurementBuffer[i]);
            return (float) r.getBottom() - norm * (float) H;
        };

        // --- filled area: single semi-transparent fill using dominant colour ---
        // (full fill in one colour avoids z-order artefacts; keep it subtle)
        {
            juce::Path fillPath;
            fillPath.startNewSubPath (getX (0), (float) r.getBottom());
            fillPath.lineTo          (getX (0), getY (0));
            for (int i = 1; i < n; ++i)
                fillPath.lineTo (getX (i), getY (i));
            fillPath.lineTo (getX (n - 1), (float) r.getBottom());
            fillPath.closeSubPath();
            g.setColour (baseColour().withAlpha (0.12f));
            g.fillPath (fillPath);
        }

        // --- zone-coloured stroke: split into segments per colour change ---
        {
            juce::Path seg;
            juce::Colour curColour = zoneColour (measurementBuffer[0]);
            seg.startNewSubPath (getX (0), getY (0));

            for (int i = 1; i < n; ++i)
            {
                juce::Colour c = zoneColour (measurementBuffer[i]);
                if (c != curColour)
                {
                    // Flush current segment
                    g.setColour (curColour.withAlpha (lineAlpha));
                    g.strokePath (seg, juce::PathStrokeType (1.8f,
                        juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    // Start new segment from the last point (no gap)
                    seg.clear();
                    seg.startNewSubPath (getX (i - 1), getY (i - 1));
                    curColour = c;
                }
                seg.lineTo (getX (i), getY (i));
            }
            // Flush last segment
            g.setColour (curColour.withAlpha (lineAlpha));
            g.strokePath (seg, juce::PathStrokeType (1.8f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Recording indicator dot (top-right corner)
        if (isMeasuring)
        {
            const float dotR = 4.0f;
            g.setColour (juce::Colours::red.withAlpha (0.85f));
            g.fillEllipse ((float) r.getRight() - dotR * 2.5f,
                           (float) r.getY() + dotR * 0.5f,
                           dotR * 2.0f, dotR * 2.0f);
        }
    }

    /** The unit the current mode's numbers are in. */
    const char* unitSuffix() const noexcept
    {
        return meterMode == MeterMode::LUFS     ? " LUFS"
             : meterMode == MeterMode::TruePeak ? " dBTP"
                                                : " dBFS";
    }

    // Convert linear amplitude (0..1+) to dB
    static float linearToDb (float linear)
    {
        if (linear <= 0.00001f) return -60.0f;
        return juce::jlimit (-60.0f, 60.0f, 20.0f * std::log10 (linear));
    }

    void mouseUp (const juce::MouseEvent&) override {}

    IAudioSource& audioSource;

    MeterMode meterMode { MeterMode::RMS };
    ColorMode colorMode { ColorMode::Standard };

    float smooth01 { 0.0f };
    float alpha { 0.0f };
    float releaseCoeff { 0.9f };
    float smoothed { 0.0f };
    float smoothedDbCache { -60.0f };

    // Peak / max hold — dB in every mode (see the note in timerCallback).
    float peakHoldDb  { -200.0f };
    float peakHoldAge { 0.0f };     // seconds since the hold was last raised

    /** How long the hold sits at a new maximum before it starts falling, and how
        fast it falls once it does. ONE pair of numbers for RMS, True Peak and LUFS:
        the hold answers the same question in all three ("how high did this just
        get?"), and three different answer speeds only made the modes harder to
        compare against each other. 7 s is long enough to catch a peak you were not
        watching for and then go and look at it; 12 dB/s is the classic falling-marker
        rate. */
    static constexpr float kPeakHoldSec       = 7.0f;
    static constexpr float kPeakFallDbPerSec  = 12.0f;

    float sessionMaxDb { -999.0f };
    float sessionMaxAgeSec { 0.0f };                 // time since the MAX was last raised
    // The printed MAX re-arms on the same clock as the bar's hold marker. They are
    // the same claim in two places — how high it just got — and when they ran on
    // different timers (8 s and 3 s) the number and the marker disagreed for five
    // seconds at a time, which reads as one of them being broken.
    static constexpr float kMaxHoldSec = kPeakHoldSec;   // 7 s

    juce::String stripText { "-inf dBFS" };

    juce::Colour barColour { juce::Colours::limegreen };
    int hostRotation { 0 };

    // Colour by tone. Updated once per timer tick; read by paint() and the ladder.
    bool      colourByTone { false };
    bool      toneTwist    { false };
    float     toneSmooth   { 0.5f };    // the SLIDER value; the rate curve lives in PitchUtils
    ToneShade toneShade    { ToneShade::Gradient };
    PitchUtils::ToneHueTracker toneHue;

    bool twoBars       { true };   // level bar + peak/max hold bar
    bool showClipZone { true };   // level history: 0 dB line pair + right-edge dB ruler

    // The bar rectangle from the last paint(), so the hover readout and the printed
    // dB scale cannot disagree. Written in paint, read in cursorText — both on the
    // message thread, so no synchronisation is needed.
    juce::Rectangle<int> lastBarArea;

    // ── Trend view + measurement ────────────────────────────────────────
    static constexpr int kMaxMeasurePoints = 54000;  // 30 min @ 30 Hz

    bool               trendMode       { false };   // Momentary=false, Trend=true
    std::vector<float> measurementBuffer;
    bool               isMeasuring     { false };
    double             measureSum      { 0.0 };     // running total → Trend average
    float              measureSmoothed { -60.0f };
    float              trendAlpha      { 0.15f };   // EMA – derived from smooth01
};

// Backwards compatibility: allow old code to still use the name VisualRmsBar
using VisualRmsBar = VisualAudioMeter;
