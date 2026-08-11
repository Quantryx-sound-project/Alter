/*
  ==============================================================================

    AudioSourceInterface.h
    Audio source abstraction - allows switching between UDP and System Audio

  ==============================================================================
*/

#pragma once

#include <vector>
#include <cstdint>

/**
 * Interface for audio data sources (UDP receiver, system audio, etc.)
 * All audio modules use this interface, so they work with any source!
 */
class IAudioSource
{
public:
    virtual ~IAudioSource() = default;

    /**
     * Get the last RMS value (0.0 - 1.0)
     */
    virtual float getLastRms() const noexcept = 0;

    /**
     * Get the last FFT spectrum (magnitude only)
     * @param out Vector to fill with FFT bins
     * @return Number of bins (0 if no data available)
     */
    virtual int getLastFft (std::vector<float>& out) const = 0;

    /**
     * Get the last stereo waveform (raw time-domain samples)
     * @param left Vector to fill with left channel samples
     * @param right Vector to fill with right channel samples
     * @return Number of samples per channel (0 if no data available)
     */
    virtual int getLastWaveform (std::vector<float>& left, std::vector<float>& right) const = 0;

    /**
     * Get detected BPM (60-200)
     */
    virtual float getBPM() const noexcept = 0;

    /**
     * Get the last True Peak value (linear amplitude, 0.0+).
     * TRUE peak = inter-sample peak per ITU-R BS.1770 (4x oversampled
     * reconstruction) — can read 0..3 dB ABOVE the stored sample maximum.
     * Default returns RMS for backward compatibility.
     */
    virtual float getLastPeak() const noexcept { return getLastRms(); }

    /**
     * Get the last SAMPLE peak (plain max |x[n]|, linear amplitude, 0.0+).
     * Always <= true peak. Default: sources without a separate sample-peak
     * measurement fall back to the true peak.
     */
    virtual float getLastSamplePeak() const noexcept { return getLastPeak(); }

    /**
     * Get the last LUFS momentary value (dB-like, ITU-R BS.1770, typically -60 to 0)
     * Default returns -100 (silence).
     */
    virtual float getLastLufs() const noexcept { return -100.0f; }

    /**
     * Get FFT packets per second (for diagnostics)
     */
    virtual int getFftPacketsPerSecond() const noexcept = 0;

    /**
     * Get the last constant-Q (multi-resolution) spectrum, log-spaced bins.
     * Mapping is fixed (ConstantQAnalyzer): bin k centre = 27.5 * 2^(k/24) Hz.
     * Default: not supported → 0 bins.
     */
    virtual int getLastCqt (std::vector<float>& out) const { out.clear(); return 0; }

    /**
     * True while a module needs the constant-Q stream, so sources can compute it
     * (it is expensive) only on demand. Default: never.
     */
    virtual void setCqtNeeded (bool) {}

    /**
     * Actual sample rate of the audio the FFT/CQT were computed at. Modules use it
     * to convert bins → Hz (Nyquist = sampleRate/2). Default 48 kHz.
     */
    virtual double getSampleRate() const noexcept { return 48000.0; }

    /**
     * Continuous, GAP-FREE mono sample stream (for hop-driven STFT analysis,
     * e.g. the spectrogram). Unlike getLastWaveform (a snapshot of the most
     * recent N samples), this delivers every sample exactly once.
     *
     * @param ioTotal In: the absolute sample counter returned by the previous
     *                call (pass 0 the first time). Out: the new counter.
     *                If the caller fell too far behind the ring capacity the
     *                oldest samples are silently skipped.
     * @param out     Filled with all samples produced since ioTotal.
     * @return        true if the source supports streaming (out may still be
     *                empty), false if not (caller should fall back to
     *                getLastWaveform snapshots).
     */
    virtual bool getMonoStream (std::uint64_t& ioTotal, std::vector<float>& out) const
    {
        (void) ioTotal;
        out.clear();
        return false;
    }

    /**
     * DAW transport sync, streamed by the Creator plugin ('T' packets):
     * tempo + beat position (PPQ; 1.1.1 = 0.0) + playing flag. BPM-synced
     * visual modes phase-lock to this grid, so their beats land exactly on
     * the host's beats. ageMs = how old the last update is.
     */
    struct HostSyncInfo
    {
        float         bpm     = 0.0f;
        double        ppq     = -1.0;
        bool          playing = false;
        std::uint32_t ageMs   = 0xFFFFFFFFu;
        // Sync IMPULSE: pulseId increments each time the DAW fires a transport
        // (re)start impulse. A consumer that sees pulseId change re-anchors its beat
        // phase to (pulseAgeMs ago). Lets BPM/division stay manual on the visual.
        std::uint32_t pulseId    = 0;
        std::uint32_t pulseAgeMs = 0xFFFFFFFFu;
    };

    /** @return true if live host-transport info is available. Default: none. */
    virtual bool getHostSync (HostSyncInfo&) const { return false; }

    /**
     * TRUE stereo constant-Q: per-channel (L/R) multi-resolution spectra with the
     * same fixed bin mapping as getLastCqt. Extra analysis cost, so sources compute
     * it only while setCqtStereoNeeded(true). Default: not supported → 0 bins.
     */
    virtual int getLastCqtStereo (std::vector<float>& outL, std::vector<float>& outR) const
    {
        outL.clear(); outR.clear(); return 0;
    }

    /** True while a module needs the STEREO constant-Q streams. Default: never. */
    virtual void setCqtStereoNeeded (bool) {}

    /** Held MIDI note (exact data from the DAW, via the plugin's 'M' packets). */
    struct MidiNote
    {
        std::uint8_t note;       // MIDI note number 0..127
        std::uint8_t velocity;   // 1..127
    };

    /**
     * Currently held MIDI notes on the source's track.
     * @return true  = MIDI is PRESENT on this source (an 'M' packet arrived within
     *                 ~2 s) → modules should use these exact notes instead of
     *                 FFT/CQT detection (out may be empty = nothing held right now);
     *         false = no MIDI on the track → fall back to the audio detection.
     */
    virtual bool getMidiNotes (std::vector<MidiNote>& out) const
    {
        out.clear();
        return false;
    }
};
