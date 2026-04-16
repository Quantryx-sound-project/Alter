/*
  ==============================================================================

    AudioSourceInterface.h
    Audio source abstraction - allows switching between UDP and System Audio

  ==============================================================================
*/

#pragma once

#include <vector>

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
     * Get the last True Peak value (linear amplitude, 0.0+)
     * Default returns RMS for backward compatibility.
     */
    virtual float getLastPeak() const noexcept { return getLastRms(); }

    /**
     * Get the last LUFS momentary value (dB-like, ITU-R BS.1770, typically -60 to 0)
     * Default returns -100 (silence).
     */
    virtual float getLastLufs() const noexcept { return -100.0f; }

    /**
     * Get FFT packets per second (for diagnostics)
     */
    virtual int getFftPacketsPerSecond() const noexcept = 0;
};
