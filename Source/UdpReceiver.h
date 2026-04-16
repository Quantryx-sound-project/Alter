#pragma once
#include <JuceHeader.h>
#include "AudioSourceInterface.h"
#include <vector>
#include <atomic>
#include <cstring>
#include <cmath>

class UdpReceiver : public IAudioSource,
                    private juce::Thread
{
public:
    explicit UdpReceiver (int listenPort)
        : juce::Thread ("UDP Receiver"),
          port (listenPort)
    {
        socket.setEnablePortReuse (true);
        socket.setMulticastLoopbackEnabled (false);
        const bool ok = socket.bindToPort (port);
        jassertquiet (ok);
        startThread();
    }

    ~UdpReceiver() override { stopThread (500); }

    // IAudioSource interface implementation
    float getLastRms() const noexcept override { return lastRms.load (std::memory_order_relaxed); }
    float getLastPeak() const noexcept override { return lastPeak.load (std::memory_order_relaxed); }
    float getLastLufs() const noexcept override { return lastLufs.load (std::memory_order_relaxed); }
    float getBPM() const noexcept override { return detectedBPM.load (std::memory_order_relaxed); }

    int getLastFft (std::vector<float>& out) const override
    {
        const juce::ScopedLock sl (fftLock);
        if (! hasFft) return 0;
        out = lastFft;
        return (int) out.size();
    }

    int getLastWaveform (std::vector<float>& left, std::vector<float>& right) const override
    {
        const juce::ScopedLock sl (waveLock);
        if (! hasWaveform) { left.clear(); right.clear(); return 0; }
        left = lastWaveL;
        right = lastWaveR;
        return (int) left.size();
    }

    int getFftPacketsPerSecond() const noexcept override { return fftPacketsPerSecond.load(); }

private:
    void run() override
    {
        uint8_t buf[65536]; // allow up to ~16k floats

        juce::int64 lastTickMs = juce::Time::getMillisecondCounter();
        int tickCount = 0;

        while (! threadShouldExit())
        {
            const int ready = socket.waitUntilReady (true, 200);
            if (ready <= 0)
                continue;

            const int n = socket.read (buf, (int) sizeof (buf), false);
            if (n <= 0) continue;

            // ALTR (RMS)
            if (n == 8 && buf[0]=='A' && buf[1]=='L' && buf[2]=='T' && buf[3]=='R')
            {
                float rms = 0.0f;
                std::memcpy (&rms, buf + 4, sizeof (float));
                lastRms.store (rms, std::memory_order_relaxed);

                detectOnset (rms);
            }
            // ALTP (True Peak)
            else if (n == 8 && buf[0]=='A' && buf[1]=='L' && buf[2]=='T' && buf[3]=='P')
            {
                float peak = 0.0f;
                std::memcpy (&peak, buf + 4, sizeof (float));
                lastPeak.store (peak, std::memory_order_relaxed);
            }
            // ALTL (LUFS momentary)
            else if (n == 8 && buf[0]=='A' && buf[1]=='L' && buf[2]=='T' && buf[3]=='L')
            {
                float lufs = -100.0f;
                std::memcpy (&lufs, buf + 4, sizeof (float));
                lastLufs.store (lufs, std::memory_order_relaxed);
            }
            // ALTF (FFT magnitudes only) - accept any number of bins (>=64)
            else if (n >= 4 + 64 * (int) sizeof (float)
                  && buf[0]=='A' && buf[1]=='L' && buf[2]=='T' && buf[3]=='F')
            {
                const int bytes = n - 4;
                if (bytes % (int) sizeof (float) != 0) continue;
                const int bins = bytes / (int) sizeof (float);

                if (bins < 64 || bins > 16384) continue;

                std::vector<float> tmp ((size_t) bins);
                std::memcpy (tmp.data(), buf + 4, (size_t) bytes);

                for (auto& v : tmp)
                {
                    if (! std::isfinite (v)) v = 0.0f;
                    v = juce::jlimit (0.0f, 1.0f, v);
                }

                {
                    const juce::ScopedLock sl (fftLock);
                    lastFft.swap (tmp);
                    hasFft = true;
                }

                ++tickCount;
            }
            // ALTW (stereo waveform) - interleaved L,R floats
            else if (n > 4 && buf[0]=='A' && buf[1]=='L' && buf[2]=='T' && buf[3]=='W')
            {
                const int bytes = n - 4;
                if (bytes % (int) sizeof (float) != 0) continue;
                const int floats = bytes / (int) sizeof (float);
                if (floats < 2 || floats % 2 != 0) continue;

                const int samplesPerCh = floats / 2;
                if (samplesPerCh > 16384) continue;

                std::vector<float> interleaved ((size_t) floats);
                std::memcpy (interleaved.data(), buf + 4, (size_t) bytes);

                std::vector<float> tmpL ((size_t) samplesPerCh);
                std::vector<float> tmpR ((size_t) samplesPerCh);

                for (int i = 0; i < samplesPerCh; ++i)
                {
                    float l = interleaved[(size_t) (i * 2 + 0)];
                    float r = interleaved[(size_t) (i * 2 + 1)];
                    if (! std::isfinite (l)) l = 0.0f;
                    if (! std::isfinite (r)) r = 0.0f;
                    tmpL[(size_t) i] = l;
                    tmpR[(size_t) i] = r;
                }

                {
                    const juce::ScopedLock sl (waveLock);
                    lastWaveL.swap (tmpL);
                    lastWaveR.swap (tmpR);
                    hasWaveform = true;
                }

                ++wavePacketCount;
            }

            // update ALTF/s once per second
            const auto now = juce::Time::getMillisecondCounter();
            if (now - lastTickMs >= 1000)
            {
                fftPacketsPerSecond.store (tickCount, std::memory_order_relaxed);
                DBG ("UDP/s  ALTF=" + juce::String (tickCount) + "  ALTW=" + juce::String (wavePacketCount));
                tickCount = 0;
                wavePacketCount = 0;
                lastTickMs = now;
            }
        }
    }

    // FFT state
    mutable juce::CriticalSection fftLock;
    std::vector<float> lastFft;
    bool hasFft = false;

    // Waveform state (stereo)
    mutable juce::CriticalSection waveLock;
    std::vector<float> lastWaveL;
    std::vector<float> lastWaveR;
    bool hasWaveform = false;

    juce::DatagramSocket socket;
    const int port;
    std::atomic<float> lastRms { 0.0f };
    std::atomic<float> lastPeak { 0.0f };
    std::atomic<float> lastLufs { -100.0f };
    std::atomic<int>   fftPacketsPerSecond { 0 };
    int wavePacketCount = 0;
    std::atomic<float> detectedBPM { 120.0f };

    std::vector<juce::int64> onsetTimes;
    juce::int64 lastOnsetTime = 0;
    float lastRmsForOnset = 0.0f;

    void detectOnset (float rms)
    {
        const auto now = juce::Time::getMillisecondCounter();
        const float threshold = lastRmsForOnset * 1.5f;

        if (rms > threshold && rms > 0.1f && (now - lastOnsetTime) > 200)
        {
            onsetTimes.push_back (now);
            lastOnsetTime = now;

            if (onsetTimes.size() > 8)
                onsetTimes.erase (onsetTimes.begin());

            if (onsetTimes.size() >= 4)
            {
                float avgInterval = 0.0f;
                for (size_t i = 1; i < onsetTimes.size(); ++i)
                    avgInterval += (float) (onsetTimes[i] - onsetTimes[i - 1]);

                avgInterval /= (float) (onsetTimes.size() - 1);

                if (avgInterval > 200.0f && avgInterval < 2000.0f)
                {
                    float bpm = 60000.0f / avgInterval;
                    bpm = juce::jlimit (60.0f, 200.0f, bpm);
                    detectedBPM.store (bpm, std::memory_order_relaxed);
                }
            }
        }

        lastRmsForOnset = rms * 0.9f + lastRmsForOnset * 0.1f;
    }
};
