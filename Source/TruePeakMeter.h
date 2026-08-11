/*
  ==============================================================================

    TruePeakMeter.h
    Inter-sample (TRUE) peak metering per ITU-R BS.1770 / EBU R128.

    A plain sample peak ( max |x[n]| ) misses peaks that occur BETWEEN samples;
    after D/A reconstruction those inter-sample peaks are real and can clip even
    when every stored sample is ≤ 0 dBFS. True peak estimates them by band-limited
    oversampling (here 4×, the BS.1770 minimum for fs ≤ 48 kHz) and taking the
    maximum of the reconstructed signal.

    This implementation upsamples 4× with a windowed-sinc polyphase FIR. It is
    real-time safe (no allocation in process) and self-contained, so the exact
    same file can be dropped into the AlterListener / AlterCreator plugins so the
    transmitted peak is a genuine true-peak value.

  ==============================================================================
*/

#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

class TruePeakMeter
{
public:
    /** Allocate per-channel state and design the filter. Call once (not on the
        audio thread) before processing, e.g. in prepareToPlay / device-open. */
    void prepare (int numChannels)
    {
        buildFilter();
        const int ch = (std::max) (1, numChannels);
        state.assign ((size_t) ch, std::vector<float> ((size_t) tapsPerPhase, 0.0f));
    }

    /** Clear the filter history (e.g. on transport start / mode switch). */
    void reset()
    {
        for (auto& s : state)
            std::fill (s.begin(), s.end(), 0.0f);
    }

    /** Feed ONE sample of one channel; returns the largest |inter-sample value|
        produced by reconstructing around it (always ≥ |x|, so true peak can
        never read below sample peak). */
    float processSample (int channel, float x) noexcept
    {
        if (channel < 0 || channel >= (int) state.size())
            return std::abs (x);

        auto& z = state[(size_t) channel];           // z[0] = newest .. z[taps-1] = oldest

        for (int t = tapsPerPhase - 1; t > 0; --t)    // shift the delay line
            z[(size_t) t] = z[(size_t) t - 1];
        z[0] = x;

        float pk = std::abs (x);                      // exact sample positions

        // ALL kOS fractional phases: the prototype centre (23.5 taps) is not an
        // integer, so every phase — including p = 0 — is a genuine inter-sample
        // interpolator (offsets ≈ 1/8, 3/8, 5/8, 7/8 of a sample period).
        // The old code skipped p = 0, leaving a coverage gap around the samples.
        for (int p = 0; p < kOS; ++p)
        {
            const float* c = &poly[(size_t) (p * tapsPerPhase)];
            float acc = 0.0f;
            for (int t = 0; t < tapsPerPhase; ++t)
                acc += c[t] * z[(size_t) t];
            pk = (std::max) (pk, std::abs (acc));
        }
        return pk;
    }

    /** Convenience: process a whole channel block, return its max true peak. */
    float processBlock (int channel, const float* data, int numSamples) noexcept
    {
        float pk = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            pk = (std::max) (pk, processSample (channel, data[i]));
        return pk;
    }

private:
    static constexpr int kOS = 4;     // 4× oversampling (BS.1770 minimum @ ≤48 kHz)
    int tapsPerPhase = 12;            // → 48-tap prototype, matching the BS.1770 length

    std::vector<float> poly;                       // kOS * tapsPerPhase polyphase coeffs
    std::vector<std::vector<float>> state;         // per-channel delay line

    void buildFilter()
    {
        constexpr double kPi = 3.14159265358979323846;
        const int protoLen = kOS * tapsPerPhase;
        std::vector<double> h ((size_t) protoLen);

        const double center = (protoLen - 1) / 2.0;

        // Cutoff = ORIGINAL Nyquist at the oversampled rate = 0.5 / kOS.
        // (The old value 1/kOS put the cutoff at the original SAMPLE RATE — twice
        // Nyquist — so the interpolation phases passed the image band and the
        // meter systematically OVER-read true peaks by several dB on bright
        // material. This is the standard 4× interpolator cutoff per BS.1770.)
        const double fc     = 0.5 / (double) kOS;

        for (int n = 0; n < protoLen; ++n)
        {
            const double m = (double) n - center;
            const double sinc = (std::abs (m) < 1.0e-9)
                                  ? 2.0 * fc
                                  : std::sin (2.0 * kPi * fc * m) / (kPi * m);
            // Blackman window for low passband ripple / good stopband
            const double w = 0.42
                           - 0.5  * std::cos (2.0 * kPi * (double) n / (double) (protoLen - 1))
                           + 0.08 * std::cos (4.0 * kPi * (double) n / (double) (protoLen - 1));
            h[(size_t) n] = sinc * w;
        }

        // Polyphase split; normalise each phase to unity DC gain so the passband
        // level is preserved (only genuine inter-sample overshoot exceeds 0 dBFS).
        poly.assign ((size_t) protoLen, 0.0f);
        for (int p = 0; p < kOS; ++p)
        {
            double g = 0.0;
            for (int t = 0; t < tapsPerPhase; ++t)
                g += h[(size_t) (t * kOS + p)];
            if (std::abs (g) < 1.0e-12) g = 1.0;
            for (int t = 0; t < tapsPerPhase; ++t)
                poly[(size_t) (p * tapsPerPhase + t)] = (float) (h[(size_t) (t * kOS + p)] / g);
        }
    }
};
