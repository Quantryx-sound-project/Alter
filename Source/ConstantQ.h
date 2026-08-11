/*
  ==============================================================================

    ConstantQ.h
    Multi-resolution constant-Q analyzer for professional tonal accuracy.

    A single linear FFT can't resolve low tones without a huge window that also
    makes the highs sluggish. Instead this runs THREE FFTs of different lengths —
    a long window for the bass, medium for the mids, short for the highs — and
    reads each output (log-spaced) bin from whichever band has the right
    resolution there. Result: every semitone resolvable from A0 (27.5 Hz) up to
    20 kHz, with snappy highs, from ~247 output bins.

    Per-band window length N needed for semitone resolution at the band's lowest
    frequency f:  N >= sampleRate / (f * (2^(1/12) - 1)).
      • Highs  2 kHz–20 kHz : 512-pt   (~11 ms)
      • Mids   250 Hz–2 kHz : 4096-pt  (~85 ms)
      • Bass   27.5 Hz–250 Hz: 32768-pt (~683 ms)

    Self-contained (needs only juce::dsp::FFT) so the same file drops into the
    AlterListener / AlterCreator plugins and the ALTER app. Heavy (the 32768 FFT),
    so callers should only run it while a Constant-Q view is actually active.

  ==============================================================================
*/

#pragma once

#include <vector>
#include <cmath>
#include <atomic>

class ConstantQAnalyzer
{
public:
    static constexpr int    kBinsPerOctave = 24;        // 2 per semitone (smooth display)
    static constexpr double kFMin          = 16.352;    // C0 — reaches down into the SUB-BASS
    static constexpr double kFMax          = 20000.0;   // full display range (Spectrum/Spectrogram go to 20 kHz)

    static constexpr int kBassOrder = 15;  // 32768
    static constexpr int kMidOrder  = 12;  // 4096
    static constexpr int kHighOrder = 9;   // 512
    static constexpr int kBassSize  = 1 << kBassOrder;
    static constexpr int kMidSize   = 1 << kMidOrder;
    static constexpr int kHighSize  = 1 << kHighOrder;

    static constexpr double kHighCross = 2000.0;   // >= → high band
    static constexpr double kMidCross  = 250.0;    // >= → mid band, else bass

    static int numBins()
    {
        return (int) std::floor (kBinsPerOctave * std::log2 (kFMax / kFMin)) + 1;  // ~221
    }

    /** Centre frequency of output bin k. */
    static float binFreq (int k)
    {
        return (float) (kFMin * std::pow (2.0, (double) k / (double) kBinsPerOctave));
    }

    void prepare (double sampleRate)
    {
        sr = (sampleRate > 0.0 ? sampleRate : 48000.0);
        ring.assign ((size_t) kBassSize, 0.0f);
        writePos = 0;
        buildWindow (winBass, kBassSize);
        buildWindow (winMid,  kMidSize);
        buildWindow (winHigh, kHighSize);
        scratch.assign ((size_t) (2 * kBassSize), 0.0f);   // reused by all bands (largest)
        bassMag.assign ((size_t) (kBassSize / 2), 0.0f);   // preallocate → no RT allocation
        midMag .assign ((size_t) (kMidSize  / 2), 0.0f);
        highMag.assign ((size_t) (kHighSize / 2), 0.0f);
    }

    /** Feed mono samples (audio thread). Cheap ring copy. */
    void pushSamples (const float* mono, int n) noexcept
    {
        if (ring.empty()) return;
        int wp = writePos.load (std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            ring[(size_t) wp] = mono[i];
            wp = (wp + 1) % kBassSize;
        }
        writePos.store (wp, std::memory_order_relaxed);   // audio → worker (benign)
    }

    /** Produce the constant-Q magnitudes (0..1, dB-normalised). Heavy: runs the
        three FFTs. Call on a hop, not every block. */
    void compute (std::vector<float>& out)
    {
        const int N = numBins();
        out.assign ((size_t) N, 0.0f);
        if (ring.empty()) return;

        // member buffers → reuse capacity (no allocation after the first call).
        // Bass (32768-pt) is the heavy one; its 683 ms window changes slowly, so we
        // refresh it at 1/3 the rate — visually identical, ~3x less of the heavy FFT.
        if (bassCounter == 0)
            bandMagnitudes (kBassSize, winBass, bassFft, bassMag);
        bassCounter = (bassCounter + 1) % 3;
        bandMagnitudes (kMidSize,  winMid,  midFft,  midMag);
        bandMagnitudes (kHighSize, winHigh, highFft, highMag);

        // Sample a band's spectrum for the CQT bin centred at f. When the CQT bin
        // is WIDER than the band's FFT bins (highs: log spacing grows, the 512-pt
        // band is coarse), take the MAX over all FFT bins the CQT bin covers — a
        // tone can no longer fall BETWEEN two log-spaced point samples and read
        // low (that under-sampling is a big part of why the highs looked cut).
        // When the CQT bin is narrower than an FFT bin, linear-interp as before.
        static const double halfStep = std::pow (2.0, 0.5 / (double) kBinsPerOctave);
        auto sampleBand = [&] (const std::vector<float>& mag, int bandSize, double f) -> float
        {
            const double binW = sr / (double) bandSize;
            const int    nb   = (int) mag.size();
            const double pLo  = (f / halfStep) / binW;
            const double pHi  = (f * halfStep) / binW;

            if (pHi - pLo > 1.0)   // CQT bin spans >1 FFT bin → peak-preserving aggregate
            {
                int iLo = (int) std::floor (pLo);
                int iHi = (int) std::ceil  (pHi);
                iLo = iLo < 0 ? 0 : (iLo > nb - 1 ? nb - 1 : iLo);
                iHi = iHi < iLo ? iLo : (iHi > nb - 1 ? nb - 1 : iHi);
                float m = 0.0f;
                for (int i = iLo; i <= iHi; ++i)
                    m = m > mag[(size_t) i] ? m : mag[(size_t) i];
                return m;
            }

            const double pos = f / binW;
            const int    i0  = (int) std::floor (pos);
            if (i0 >= 0 && i0 < nb - 1)
            {
                const float fr = (float) (pos - (double) i0);
                return mag[(size_t) i0] * (1.0f - fr) + mag[(size_t) (i0 + 1)] * fr;
            }
            return (i0 >= 0 && i0 < nb) ? mag[(size_t) i0] : 0.0f;
        };
        auto blend = [] (double f, double lo, double hi) -> float   // smoothstep over [lo,hi]
        {
            double t = std::log (f / lo) / std::log (hi / lo);
            t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
            return (float) (t * t * (3.0 - 2.0 * t));
        };

        // Crossfade zones (±1 octave) so the three bands blend over a wide, gradual
        // ramp and read as ONE continuous spectrum — never a visible 3-way split.
        const double rt  = 2.0;                    // a full octave each side of the crossover
        const double mLo = kMidCross  / rt, mHi = kMidCross  * rt;   // bass ↔ mid  (125–500 Hz)
        const double hLo = kHighCross / rt, hHi = kHighCross * rt;   // mid  ↔ high (1–4 kHz)

        // Per-band display gain: a longer window has narrower bins, so broadband
        // content reads lower there — that level offset is what made the bands look
        // like separate shelves. The bass keeps a mild bin-width compensation.
        // The HIGH band is NOT attenuated any more: the old (kHigh/kMid)^0.25
        // factor (~-4.5 dB) visibly "cut" the highs for tonal content — tones must
        // read true; the coherent-gain norm (4/size) already calibrates them.
        const float gBass = (float) std::pow ((double) kBassSize / (double) kMidSize, 0.25);
        const float gHigh = 1.0f;

        for (int k = 0; k < N; ++k)
        {
            const double f = (double) binFreq (k);
            float v;
            if      (f <= mLo) v = gBass * sampleBand (bassMag, kBassSize, f);
            else if (f <  mHi) { const float t = blend (f, mLo, mHi);
                                 v = gBass * sampleBand (bassMag, kBassSize, f) * (1.0f - t)
                                   +         sampleBand (midMag,  kMidSize,  f) * t; }
            else if (f <= hLo) v = sampleBand (midMag, kMidSize, f);
            else if (f <  hHi) { const float t = blend (f, hLo, hHi);
                                 v =         sampleBand (midMag,  kMidSize,  f) * (1.0f - t)
                                   + gHigh * sampleBand (highMag, kHighSize, f) * t; }
            else               v = gHigh * sampleBand (highMag, kHighSize, f);

            // dB normalise like the linear FFT path: -90..0 dBFS → 0..1
            float dB = 20.0f * std::log10 (v + 1.0e-9f);
            dB = dB < -90.0f ? -90.0f : (dB > 0.0f ? 0.0f : dB);
            out[(size_t) k] = (dB + 90.0f) / 90.0f;
        }

        // ── Per-TONE smoothing ──────────────────────────────────────────────────
        // Light 3-tap blend across the log-frequency bins to fuse the band
        // boundaries into one continuous curve. Deliberately mild: the old heavy
        // 5-tap kernel squashed narrow high-frequency peaks (a high tone spans
        // ~1 CQT bin) — combined with the band attenuation that read as "cut"
        // highs. The MAX aggregation in sampleBand already removes the per-bin
        // comb, so a strong kernel is no longer needed.
        if ((int) smoothScratch.size() != N) smoothScratch.assign ((size_t) N, 0.0f);
        static const float w[3] = { 0.22f, 0.56f, 0.22f };
        for (int k = 0; k < N; ++k)
        {
            float acc = 0.0f, wsum = 0.0f;
            for (int d = -1; d <= 1; ++d)
            {
                const int kk = k + d;
                if (kk < 0 || kk >= N) continue;
                const float wt = w[d + 1];
                acc += out[(size_t) kk] * wt; wsum += wt;
            }
            smoothScratch[(size_t) k] = (wsum > 0.0f) ? acc / wsum : out[(size_t) k];
        }
        out.swap (smoothScratch);
    }

private:
    std::vector<float> smoothScratch;   // per-tone smoothing scratch
    double sr = 48000.0;
    std::vector<float> ring;
    std::atomic<int> writePos { 0 };
    std::vector<float> winBass, winMid, winHigh, scratch;
    std::vector<float> bassMag, midMag, highMag;   // reused per compute()
    int bassCounter = 0;                            // bass refreshed every 3rd compute()

    juce::dsp::FFT bassFft { kBassOrder };
    juce::dsp::FFT midFft  { kMidOrder };
    juce::dsp::FFT highFft { kHighOrder };

    static void buildWindow (std::vector<float>& w, int n)
    {
        w.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
            w[(size_t) i] = 0.5f * (1.0f - std::cos (2.0f * 3.14159265358979f
                                                     * (float) i / (float) (n - 1)));
    }

    // Window the most-recent `size` samples of the ring and return magnitudes.
    void bandMagnitudes (int size, const std::vector<float>& win,
                         juce::dsp::FFT& fft, std::vector<float>& outMag)
    {
        // newest `size` samples end at writePos-1
        const int wp = writePos.load (std::memory_order_relaxed);
        const int start = ((wp - size) % kBassSize + kBassSize) % kBassSize;
        for (int i = 0; i < size; ++i)
            scratch[(size_t) i] = ring[(size_t) ((start + i) % kBassSize)] * win[(size_t) i];

        fft.performFrequencyOnlyForwardTransform (scratch.data());   // first size/2 = magnitudes

        const int bins = size / 2;
        const float norm = 4.0f / (float) size;   // ~matches the linear FFT path's level calibration
        outMag.resize ((size_t) bins);
        for (int i = 0; i < bins; ++i)
            outMag[(size_t) i] = scratch[(size_t) i] * norm;
    }
};
