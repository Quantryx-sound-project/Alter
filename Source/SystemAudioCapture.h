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
#include "TruePeakMeter.h"
#include "ConstantQ.h"
#include <vector>
#include <atomic>

// Platform-specific includes
#if JUCE_WINDOWS
    #include "WasapiLoopbackCapture.h"
#elif JUCE_MAC
    #include "MacSystemAudioCapture.h"   // ScreenCaptureKit backend (macOS 13+)
#endif

class SystemAudioCapture : public IAudioSource, private juce::Timer, private juce::Thread
{
public:
    SystemAudioCapture() : juce::Thread ("ALTER CQT")
    {
        truePeak.prepare (2);   // stereo inter-sample (true) peak meter
        cqt.prepare (48000.0);  // constant-Q analyzer (ring + windows)
        cqtL.prepare (48000.0); // per-channel analyzers (stereo CQ overlay, on demand)
        cqtR.prepare (48000.0);
        // FFT window is (re)built lazily in computeFFT for the active size.

        // Platform detection
        #if JUCE_WINDOWS
            DBG("=== Platform: Windows ===");
            DBG("=== Initializing WASAPI Loopback ===");
            wasapiCapture = std::make_unique<WasapiLoopbackCapture>();
            if (wasapiCapture->isActive())
                DBG("[OK] System Audio Capture: READY");
            else
                DBG("[X] System Audio Capture: FAILED");
        #elif JUCE_MAC
            DBG("=== Platform: macOS ===");
            DBG("=== Initializing ScreenCaptureKit system audio ===");
            macCapture = std::make_unique<MacSystemAudioCapture>();
        #elif JUCE_LINUX
            DBG("=== Platform: Linux ===");
            DBG("System Audio Capture is NOT SUPPORTED on Linux");
            DBG("   Please use VST Plugin mode instead!");
        #else
            DBG("Unknown platform - System Audio Capture disabled");
        #endif

        // 60 Hz feed: the analysis timer matches the 60 fps visual workers, so the
        // Spectrum gets a FRESH FFT every frame (at 30 Hz every other frame was a
        // duplicate → visible stepping) and stream bursts to the Spectrogram halve.
        startTimerHz(60);

        // K-weighting for LUFS (assume 48 kHz WASAPI default)
        initKWeighting (48000.0);

        startThread (juce::Thread::Priority::low);   // constant-Q worker (off the timer thread)
    }

    ~SystemAudioCapture() override
    {
        stopThread (500);   // stop the CQT worker before tearing down its buffers
        stopTimer();
        #if JUCE_WINDOWS
            wasapiCapture.reset();
        #elif JUCE_MAC
            macCapture.reset();
        #endif
    }

    float getLastRms() const noexcept override { return lastRms.load(); }
    float getLastPeak() const noexcept override { return lastPeak.load(); }
    float getLastSamplePeak() const noexcept override { return lastSamplePeak.load(); }
    float getLastLufs() const noexcept override { return lastLufs.load(); }
    float getBPM() const noexcept override { return 120.0f; }

    void setGain (float dB) { systemAudioGainDB = juce::jlimit (0.0f, 20.0f, dB); }

    /** Global FFT order (12..14 → 4096..16384-pt → 2048..8192 bins). Applied
        lazily on the capture/analysis thread inside computeFFT. */
    void setFftOrder (int order) noexcept { requestedLocalOrder.store (juce::jlimit (12, 14, order)); }

    int getLastFft(std::vector<float>& out) const override
    {
        const juce::ScopedLock sl(fftLock);
        if (!hasFft) return 0;
        out = lastFft;
        return (int)out.size();
    }

    int getLastCqt (std::vector<float>& out) const override
    {
        const juce::ScopedLock sl (cqtLock);
        if (! hasCqt) { out.clear(); return 0; }
        out = lastCqt;
        return (int) out.size();
    }

    void setCqtNeeded (bool on) override { cqtNeeded.store (on); }

    int getLastCqtStereo (std::vector<float>& outL, std::vector<float>& outR) const override
    {
        const juce::ScopedLock sl (cqtLock);
        if (! hasCqtStereo) { outL.clear(); outR.clear(); return 0; }
        outL = lastCqtL;
        outR = lastCqtR;
        return (int) juce::jmin (outL.size(), outR.size());
    }

    void setCqtStereoNeeded (bool on) override { cqtStereoNeeded.store (on); }

    /** The device's actual capture rate (WASAPI/CoreAudio native), so bin→Hz is correct. */
    double currentSr() const noexcept
    {
        #if JUCE_WINDOWS
            if (wasapiCapture) { const int sr = wasapiCapture->getSampleRate(); if (sr > 0) return (double) sr; }
        #elif JUCE_MAC
            if (macCapture) { const int sr = macCapture->getSampleRate(); if (sr > 0) return (double) sr; }
        #endif
        return 48000.0;
    }
    double getSampleRate() const noexcept override { return currentSr(); }

    int getLastWaveform (std::vector<float>& left, std::vector<float>& right) const override
    {
        const juce::ScopedLock sl (waveLock);   // worker threads may read this concurrently
        const size_t wp = bufferWritePos.load();
        const int count = (int) audioBuffer.size();
        left.resize ((size_t) count);
        right.resize ((size_t) count);
        for (int i = 0; i < count; ++i)
        {
            const size_t idx = (wp + (size_t) i) % audioBuffer.size();
            left[(size_t) i]  = audioBufferL[idx];   // real stereo
            right[(size_t) i] = audioBufferR[idx];
        }
        return count;
    }

    int getFftPacketsPerSecond() const noexcept override { return 60; }

    /** Gap-free mono stream (spectrogram STFT). Every captured sample is
        delivered exactly once — no snapshot duplicates, no timer jitter. */
    bool getMonoStream (std::uint64_t& ioTotal, std::vector<float>& out) const override
    {
        const juce::ScopedLock sl (waveLock);
        const std::uint64_t total = streamTotal;
        std::uint64_t start = juce::jmin (ioTotal, total);
        if (total - start > (std::uint64_t) kStreamRingSize)
            start = total - (std::uint64_t) kStreamRingSize;   // fell behind → skip oldest

        out.resize ((size_t) (total - start));
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = streamRing[(size_t) ((start + i) % kStreamRingSize)];

        ioTotal = total;
        return true;
    }

private:
    void timerCallback() override
    {
        std::vector<float> interleaved;   // L,R,L,R...

        #if JUCE_WINDOWS
            if (wasapiCapture && wasapiCapture->getStereoData (interleaved) && ! interleaved.empty())
                processStereo (interleaved);
        #elif JUCE_MAC
            if (macCapture && macCapture->getStereoData (interleaved) > 0 && ! interleaved.empty())
                processStereo (interleaved);
        #endif

        computeFFT();
        // Constant-Q is computed on the worker thread (run()), not here.
    }

    // Constant-Q worker: heavy multi-resolution FFTs off the timer/message thread,
    // so a slow machine never stutters the UI and a fast one is unaffected. Reads the
    // analyzer ring (filled on the timer thread; benign race) only while CQT is needed.
    void run() override
    {
        std::vector<float> tmp, tmpL, tmpR;
        juce::uint32 lastMs = 0;
        while (! threadShouldExit())
        {
            const auto now = juce::Time::getMillisecondCounter();
            const bool wantMono   = cqtNeeded.load();
            const bool wantStereo = cqtStereoNeeded.load();
            if ((wantMono || wantStereo) && now - lastMs >= 33)   // ~30 Hz
            {
                lastMs = now;
                // Re-prepare the CQT to the device's ACTUAL rate (WASAPI may be 44.1 kHz,
                // not the assumed 48 kHz) so bin→Hz mapping — and the detected note — is right.
                const double sr = currentSr();

                if (wantMono)
                {
                    if (std::abs (sr - cqtPreparedSr) > 1.0) { cqt.prepare (sr); cqtPreparedSr = sr; }
                    cqt.compute (tmp);
                    const juce::ScopedLock sl (cqtLock);
                    lastCqt.swap (tmp);   // tmp keeps the old capacity → no realloc next time
                    hasCqt = true;
                }

                if (wantStereo)   // TRUE stereo constant-Q (Spectrum stereo + CQ overlay)
                {
                    if (std::abs (sr - cqtStereoPreparedSr) > 1.0)
                    {
                        cqtL.prepare (sr);
                        cqtR.prepare (sr);
                        cqtStereoPreparedSr = sr;
                    }
                    cqtL.compute (tmpL);
                    cqtR.compute (tmpR);
                    const juce::ScopedLock sl (cqtLock);
                    lastCqtL.swap (tmpL);
                    lastCqtR.swap (tmpR);
                    hasCqtStereo = true;
                }
            }
            else
                juce::Thread::sleep (3);
        }
    }

    // Stereo analysis: keep L/R for the waveform; mono mix for FFT/RMS/LUFS.
    void processStereo (const std::vector<float>& interleaved)
    {
        const float linearGain = std::pow (10.0f, systemAudioGainDB / 20.0f);

        // OVERLOAD GUARD: after a stall (device switch, modal dialog, debugger,
        // system hiccup) the capture can hand us SECONDS of audio in one chunk.
        // Analyse only the newest ~170 ms — dropping the stale part keeps the
        // meters truthful "now" and avoids a multi-millisecond hitch here.
        const float* data  = interleaved.data();
        size_t frames = interleaved.size() / 2;
        constexpr size_t kMaxFramesPerTick = 8192;
        if (frames > kMaxFramesPerTick)
        {
            data  += 2 * (frames - kMaxFramesPerTick);
            frames = kMaxFramesPerTick;
        }
        if (frames == 0) return;

        auto softClip = [] (float v) -> float
        {
            if (v > 1.0f)       return 1.0f - (1.0f - v) * 0.5f;
            if (v < -1.0f)      return -1.0f - (-1.0f - v) * 0.5f;
            return v;
        };

        float rms = 0.0f, peak = 0.0f, sPeak = 0.0f;

        const juce::ScopedLock sl (waveLock);   // guards audioBufferL/R/audioBuffer vs worker readers

        for (size_t i = 0; i < frames; ++i)
        {
            // RAW capture signal → MEASUREMENT (true peak / sample peak / LUFS).
            // The SYSTEM GAIN control is a VISUAL boost only: with it inside the
            // metering path the meter read above 0 dBTP while DAW meters showed
            // -1/-2 dBTP. Meters must always report the real signal.
            const float rawL = data[2 * i];
            const float rawR = data[2 * i + 1];

            const float L = softClip (rawL * linearGain);    // gained → visuals
            const float R = softClip (rawR * linearGain);
            const float mono = 0.5f * (L + R);

            audioBufferL[bufferWritePos] = L;
            audioBufferR[bufferWritePos] = R;
            audioBuffer[bufferWritePos]  = mono;            // mono mix (legacy waveform ring)
            bufferWritePos = (bufferWritePos + 1) % audioBuffer.size();

            // gap-free mono stream ring (spectrogram STFT)
            streamRing[(size_t) (streamTotal % kStreamRingSize)] = mono;
            ++streamTotal;

            // Separate, larger mono ring for the configurable-size FFT.
            monoFft[monoWrite] = mono;
            monoWrite = (monoWrite + 1) % monoFft.size();

            if (cqtNeeded.load())
                cqt.pushSamples (&mono, 1);   // feed the constant-Q ring
            if (cqtStereoNeeded.load())       // per-channel rings (stereo CQ overlay)
            {
                cqtL.pushSamples (&L, 1);
                cqtR.pushSamples (&R, 1);
            }

            rms += mono * mono;

            // SAMPLE peak (max |x[n]|) + TRUE (inter-sample) peak, 4× oversampled
            // per ITU-R BS.1770 — both on the RAW (pre-gain, pre-clip) signal.
            sPeak = juce::jmax (sPeak, std::abs (rawL), std::abs (rawR));
            peak  = juce::jmax (peak, truePeak.processSample (0, rawL),
                                       truePeak.processSample (1, rawR));

            // LUFS on the RAW mono mix (measurement, not visual)
            float kw = 0.5f * (rawL + rawR);
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

        lastRms.store (std::sqrt (rms / (float) frames));

        // Short max-hold (~130 ms of ticks): consumers polling slower than the
        // 60 Hz capture (meters run at 30 Hz) would otherwise MISS peaks that
        // landed in ticks between their reads → underread "max" values.
        peakHoldRing[(size_t) peakHoldIdx] = peak;
        spHoldRing  [(size_t) peakHoldIdx] = sPeak;
        peakHoldIdx = (peakHoldIdx + 1) % kPeakHoldTicks;
        float pkMax = 0.0f, spMax = 0.0f;
        for (int k = 0; k < kPeakHoldTicks; ++k)
        {
            pkMax = juce::jmax (pkMax, peakHoldRing[(size_t) k]);
            spMax = juce::jmax (spMax, spHoldRing[(size_t) k]);
        }
        lastPeak.store (pkMax);
        lastSamplePeak.store (spMax);

        const double meanSq = (lufsBufferSize > 0) ? lufsRunSum / (double) lufsBufferSize : 0.0;
        float lufs = -100.0f;
        if (meanSq > 1.0e-10)
            lufs = (float) (-0.691 + 10.0 * std::log10 (meanSq));
        lastLufs.store (lufs);
    }

    void computeFFT()
    {
        // Apply a pending FFT-size change (same thread as the read below).
        const int want = juce::jlimit (12, 14, requestedLocalOrder.load());
        if (want != localOrder)
        {
            localOrder   = want;
            localFftSize = 1 << want;
            localBins    = localFftSize / 2;
            fft          = juce::dsp::FFT (want);
            for (int i = 0; i < localFftSize; ++i)
                windowFunction[(size_t) i] = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi
                                                  * (float) i / (float) localFftSize));
        }

        // Read the most recent localFftSize samples from the mono ring.
        const size_t start = (monoWrite + monoFft.size() - (size_t) localFftSize) % monoFft.size();
        for (int i = 0; i < localFftSize; ++i)
        {
            const size_t readPos = (start + (size_t) i) % monoFft.size();
            fftData[(size_t) i] = monoFft[readPos] * windowFunction[(size_t) i];
        }

        fft.performFrequencyOnlyForwardTransform (fftData.data());

        // reuse the scratch buffer (ping-pongs with lastFft via swap below) →
        // zero steady-state allocation on this 30 Hz path
        fftScratch.resize ((size_t) localBins);
        auto& tmpFft = fftScratch;
        const float scale = 2.0f / (float) localFftSize;

        // dBFS normalization: -90 dBFS (silence) .. 0 dBFS (max).
        // MUST match the display decode floor (Spectrum assumedDbFloor = -90 and
        // the CQT path): with the old -96 here the readouts were skewed by up to
        // ~3 dB mid-scale (0..1 encoded over 96 dB, decoded over 90 dB).
        const float minDB = -90.0f, dbRange = 90.0f;
        for (int i = 0; i < localBins; ++i)
        {
            const float magnitude  = std::abs (fftData[(size_t) i]) * scale;
            const float db         = 20.0f * std::log10 (magnitude + 1e-8f);
            const float normalized = (db - minDB) / dbRange;
            tmpFft[(size_t) i] = juce::jlimit (0.0f, 1.0f, normalized);
        }

        const juce::ScopedLock sl (fftLock);
        lastFft.swap (tmpFft);
        hasFft = true;
    }

    // Platform-specific capture objects
    #if JUCE_WINDOWS
        std::unique_ptr<WasapiLoopbackCapture> wasapiCapture;
    #elif JUCE_MAC
        std::unique_ptr<MacSystemAudioCapture> macCapture;
    #endif

    std::array<float, 4096> audioBuffer{};    // mono mix (FFT)
    std::array<float, 4096> audioBufferL{};   // real stereo (waveform)
    std::array<float, 4096> audioBufferR{};
    std::atomic<size_t> bufferWritePos{ 0 };

    // Gap-free mono stream ring (guarded by waveLock). ~680 ms @ 48 kHz —
    // plenty for a 60 Hz UI consumer even with hiccups.
    static constexpr size_t kStreamRingSize = 32768;
    std::array<float, kStreamRingSize> streamRing{};
    std::uint64_t streamTotal { 0 };

    // Configurable FFT (size set globally; default order 12 = 2048 bins).
    static constexpr int kMaxLocalFftSize = 16384;       // order 14
    juce::dsp::FFT fft{ 12 };
    std::atomic<int> requestedLocalOrder{ 12 };
    int localOrder   = -1;                                // -1 → build on first computeFFT
    int localFftSize = 4096;
    int localBins    = 2048;
    std::array<float, kMaxLocalFftSize>     monoFft{};    // mono ring for the FFT
    size_t                                  monoWrite{ 0 };
    std::array<float, 2 * kMaxLocalFftSize> fftData{};    // performFrequencyOnly needs 2N
    std::array<float, kMaxLocalFftSize>     windowFunction{};
    mutable juce::CriticalSection fftLock;
    mutable juce::CriticalSection waveLock;   // guards audioBufferL/R for worker-thread readers
    std::vector<float> lastFft;
    std::vector<float> fftScratch;            // computeFFT scratch (swapped with lastFft)
    bool hasFft = false;

    std::atomic<float> lastRms{ 0.0f };
    std::atomic<float> lastPeak{ 0.0f };
    std::atomic<float> lastSamplePeak{ 0.0f };
    std::atomic<float> lastLufs{ -100.0f };

    // short max-hold rings for peak values (see processStereo)
    static constexpr int kPeakHoldTicks = 8;
    std::array<float, kPeakHoldTicks> peakHoldRing{};
    std::array<float, kPeakHoldTicks> spHoldRing{};
    int peakHoldIdx { 0 };

    TruePeakMeter truePeak;   // inter-sample (true) peak, 4× oversampled

    // Constant-Q (multi-resolution) — computed only while a Constant-Q view is active.
    ConstantQAnalyzer            cqt;
    std::atomic<bool>            cqtNeeded { false };
    double                       cqtPreparedSr { 48000.0 };   // rate the CQT windows were built for
    mutable juce::CriticalSection cqtLock;
    std::vector<float>           lastCqt;
    bool                         hasCqt = false;

    // TRUE stereo Constant-Q (per-channel L/R analyzers) — only while a Spectrum
    // shows the stereo overlay in CQ mode (extra analysis cost, on demand).
    ConstantQAnalyzer            cqtL, cqtR;
    std::atomic<bool>            cqtStereoNeeded { false };
    double                       cqtStereoPreparedSr { 48000.0 };
    std::vector<float>           lastCqtL, lastCqtR;   // guarded by cqtLock
    bool                         hasCqtStereo = false;

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
