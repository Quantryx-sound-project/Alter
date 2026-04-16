/*
  ==============================================================================

    SystemAudioCapture.h
    System Audio Capture - Platform-specific implementation

    ✅ Windows: WASAPI Loopback (Native, works out-of-the-box!)
    ⚠️ macOS:   Not supported (use VST Plugin mode instead)
    ⚠️ Linux:   Not supported (use VST Plugin mode instead)

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "AudioSourceInterface.h"
#include <vector>
#include <atomic>

// Platform-specific includes
#if JUCE_WINDOWS
    #include "WasapiLoopbackCapture.h"
#endif

class SystemAudioCapture : public IAudioSource, private juce::Timer
{
public:
    SystemAudioCapture()
    {
        for (size_t i = 0; i < 2048; ++i)
            windowFunction[i] = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i / 2048.0f));

        // Platform detection
        #if JUCE_WINDOWS
            DBG("=== Platform: Windows ===");
            DBG("=== Initializing WASAPI Loopback ===");
            wasapiCapture = std::make_unique<WasapiLoopbackCapture>();
            if (wasapiCapture->isActive())
                DBG("✅ System Audio Capture: READY");
            else
                DBG("❌ System Audio Capture: FAILED");
        #elif JUCE_MAC
            DBG("=== Platform: macOS ===");
            DBG("⚠️ System Audio Capture is NOT SUPPORTED on macOS");
            DBG("   Please use VST Plugin mode instead!");
            DBG("   (VST Plugin works identically on Windows and macOS)");
        #elif JUCE_LINUX
            DBG("=== Platform: Linux ===");
            DBG("⚠️ System Audio Capture is NOT SUPPORTED on Linux");
            DBG("   Please use VST Plugin mode instead!");
        #else
            DBG("⚠️ Unknown platform - System Audio Capture disabled");
        #endif

        startTimerHz(30);

        // K-weighting for LUFS (assume 48 kHz WASAPI default)
        initKWeighting (48000.0);
    }

    ~SystemAudioCapture() override
    {
        stopTimer();
        #if JUCE_WINDOWS
            wasapiCapture.reset();
        #endif
    }

    float getLastRms() const noexcept override { return lastRms.load(); }
    float getLastPeak() const noexcept override { return lastPeak.load(); }
    float getLastLufs() const noexcept override { return lastLufs.load(); }
    float getBPM() const noexcept override { return 120.0f; }

    void setGain (float dB) { systemAudioGainDB = juce::jlimit (0.0f, 20.0f, dB); }

    int getLastFft(std::vector<float>& out) const override
    {
        const juce::ScopedLock sl(fftLock);
        if (!hasFft) return 0;
        out = lastFft;
        return (int)out.size();
    }

    int getLastWaveform (std::vector<float>& left, std::vector<float>& right) const override
    {
        // SystemAudioCapture already has raw samples in audioBuffer
        const size_t wp = bufferWritePos.load();
        const int count = (int) audioBuffer.size();
        left.resize ((size_t) count);
        right.resize ((size_t) count);
        for (int i = 0; i < count; ++i)
        {
            const size_t idx = (wp + (size_t) i) % audioBuffer.size();
            left[(size_t) i] = audioBuffer[idx];
            right[(size_t) i] = audioBuffer[idx];  // mono source -> L=R
        }
        return count;
    }

    int getFftPacketsPerSecond() const noexcept override { return 30; }

private:
    void timerCallback() override
    {
        #if JUCE_WINDOWS
            std::vector<float> samples;
            if (wasapiCapture && wasapiCapture->getAudioData(samples))
            {
                const float linearGain = std::pow (10.0f, systemAudioGainDB / 20.0f);
                float rms = 0.0f;
                float peak = 0.0f;

                for (auto sample : samples)
                {
                    float boosted = sample * linearGain;

                    // Soft clip to prevent harsh distortion
                    if (boosted > 1.0f)
                        boosted = 1.0f - (1.0f - boosted) * 0.5f;
                    else if (boosted < -1.0f)
                        boosted = -1.0f - (-1.0f - boosted) * 0.5f;

                    audioBuffer[bufferWritePos] = boosted;
                    bufferWritePos = (bufferWritePos + 1) % audioBuffer.size();
                    rms += boosted * boosted;

                    // True Peak: max |sample|
                    const float absVal = std::abs (boosted);
                    if (absVal > peak) peak = absVal;

                    // LUFS: K-weighted mean-square accumulation
                    float kw = boosted;
                    kw = kStage1.process (kw);
                    kw = kStage2.process (kw);
                    const float kSq = kw * kw;

                    if (lufsBufferSize > 0)
                    {
                        lufsRunSum -= (double) lufsBuffer[(size_t) lufsWriteIdx];
                        lufsBuffer[(size_t) lufsWriteIdx] = kSq;
                        lufsRunSum += (double) kSq;
                        if (lufsRunSum < 0.0) lufsRunSum = 0.0;
                        lufsWriteIdx = (lufsWriteIdx + 1) % lufsBufferSize;
                    }
                }

                if (!samples.empty())
                {
                    rms = std::sqrt(rms / samples.size());
                    lastRms.store(rms);
                    lastPeak.store(peak);

                    // LUFS momentary
                    const double meanSq = (lufsBufferSize > 0)
                                        ? lufsRunSum / (double) lufsBufferSize
                                        : 0.0;
                    float lufs = -100.0f;
                    if (meanSq > 1.0e-10)
                        lufs = (float) (-0.691 + 10.0 * std::log10 (meanSq));
                    lastLufs.store (lufs);
                }
            }
        #else
            // macOS/Linux: No system audio capture - stay silent
            // (VST Plugin mode works on all platforms!)
        #endif

        computeFFT();
    }

    void computeFFT()
    {
        const size_t writePos = bufferWritePos.load();
        for (size_t i = 0; i < 2048; ++i)
        {
            size_t readPos = (writePos + i) % audioBuffer.size();
            fftData[i] = audioBuffer[readPos] * windowFunction[i];
        }

        fft.performFrequencyOnlyForwardTransform(fftData.data());

        std::vector<float> tmpFft(1024);
        const float scale = 2.0f / 2048.0f;

        // Professional dBFS normalization (like Ableton/FL Studio)
        // Range: -96 dBFS (silence) to 0 dBFS (maximum)
        const float minDB = -96.0f;
        const float maxDB = 0.0f;
        const float dbRange = maxDB - minDB;

        for (size_t i = 0; i < 1024; ++i)
        {
            // Linear magnitude
            float magnitude = std::abs(fftData[i]) * scale;

            // Convert to dBFS: 20 * log10(magnitude)
            // Add tiny epsilon to avoid log(0)
            float db = 20.0f * std::log10(magnitude + 1e-8f);

            // Normalize from -96dB...0dB to 0.0...1.0
            float normalized = (db - minDB) / dbRange;

            // Clamp to valid range
            tmpFft[i] = juce::jlimit(0.0f, 1.0f, normalized);
        }

        const juce::ScopedLock sl(fftLock);
        lastFft.swap(tmpFft);
        hasFft = true;
    }

    // Platform-specific capture objects
    #if JUCE_WINDOWS
        std::unique_ptr<WasapiLoopbackCapture> wasapiCapture;
    #endif

    std::array<float, 4096> audioBuffer{};
    std::atomic<size_t> bufferWritePos{ 0 };

    juce::dsp::FFT fft{ 11 };
    std::array<float, 4096> fftData{};
    std::array<float, 2048> windowFunction{};
    mutable juce::CriticalSection fftLock;
    std::vector<float> lastFft;
    bool hasFft = false;

    std::atomic<float> lastRms{ 0.0f };
    std::atomic<float> lastPeak{ 0.0f };
    std::atomic<float> lastLufs{ -100.0f };

    // K-weighting biquad (ITU-R BS.1770)
    struct Biquad
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;
        void reset() { z1 = z2 = 0.0f; }
        float process (float x)
        {
            float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    Biquad kStage1, kStage2;  // mono K-weighting
    std::vector<float> lufsBuffer;
    int  lufsBufferSize = 0;
    int  lufsWriteIdx   = 0;
    double lufsRunSum   = 0.0;

    void initKWeighting (double fs)
    {
        const double pi = juce::MathConstants<double>::pi;

        // Stage 1: High-shelf
        {
            const double f0 = 1681.974450955533;
            const double G  = 3.999843853973347;
            const double Q  = 0.7071752369554196;
            const double A  = std::pow (10.0, G / 40.0);
            const double w0 = 2.0 * pi * f0 / fs;
            const double sw = std::sin (w0), cw = std::cos (w0);
            const double al = sw / (2.0 * Q), sqA = std::sqrt (A);
            const double a0 =        (A+1) - (A-1)*cw + 2*sqA*al;
            kStage1.b0 = (float)(A*((A+1)+(A-1)*cw+2*sqA*al) / a0);
            kStage1.b1 = (float)(-2*A*((A-1)+(A+1)*cw) / a0);
            kStage1.b2 = (float)(A*((A+1)+(A-1)*cw-2*sqA*al) / a0);
            kStage1.a1 = (float)(2*((A-1)-(A+1)*cw) / a0);
            kStage1.a2 = (float)(((A+1)-(A-1)*cw-2*sqA*al) / a0);
            kStage1.reset();
        }

        // Stage 2: High-pass (RLB weighting)
        {
            const double f0 = 38.13547087602444;
            const double Q  = 0.5003270373238773;
            const double w0 = 2.0 * pi * f0 / fs;
            const double sw = std::sin (w0), cw = std::cos (w0);
            const double al = sw / (2.0 * Q);
            const double a0 = 1.0 + al;
            kStage2.b0 = (float)((1+cw)/2.0 / a0);
            kStage2.b1 = (float)(-(1+cw) / a0);
            kStage2.b2 = (float)((1+cw)/2.0 / a0);
            kStage2.a1 = (float)(-2*cw / a0);
            kStage2.a2 = (float)((1-al) / a0);
            kStage2.reset();
        }

        // LUFS 400 ms buffer
        lufsBufferSize = (int) (fs * 0.4);
        lufsBuffer.assign ((size_t) lufsBufferSize, 0.0f);
        lufsWriteIdx = 0;
        lufsRunSum   = 0.0;
    }

    // System audio gain in dB (0 dB = unity/1x, +20 dB = 10x)
    float systemAudioGainDB = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SystemAudioCapture)
};