#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <vector>
#include "AudioSourceInterface.h"

// Real‑time spectrum render (safe):
// - UDP ALTF (N floats 0..1)
// - optional A‑weight (v dB doméne)
// - časové EMA smoothing (input+display)
// - log‑X resampling + Catmull‑Rom medzi binmi
// - dual‑Gaussian blur (wide v base, narrow vo výškach) s plynulým mixom
// - fixed gain s kGain, Y je vždy clampnuté + clip region

class VisualSpectrum : public juce::Component
{
public:
    explicit VisualSpectrum (IAudioSource& r) : audioSource (r)
    {
        setOpaque (true);
    }

    void setUseAWeight (bool on)    { useAWeight = on; repaint(); }

    void setDisplayBins (int b)
        {
            // povolíme len 3 hodnoty, aby sa to nerozbilo
            if (b != 512 && b != 1024 && b != 2048)
                b = 2048;

            displayBins = b;
            repaint(); // nech sa to hneď prekreslí
        }
    
    // smooth 0..1 (časové)
    void setSmoothAmount (float s)
    {
        s = juce::jlimit (0.0f, 1.0f, s);
        // 0 = rýchle, 1 = veľmi plynulé
        const float minAlpha = 0.04f, maxAlpha = 0.70f;           // ↑ dovolíme vyššie alpha
        float a = juce::jmap (s, 1.0f, 0.0f, minAlpha, maxAlpha); // s=0 → najrýchlejšie, s=1 → najplynulejšie
        a = juce::jlimit (0.001f, 0.95f, a);
        inputAlpha   = a;          // predtým *0.7; teraz nech je vstup rovnako svižný
    }

    void setAssumedDbFloor    (float dB)  { assumedDbFloor = dB; }
    void setAssumedSampleRate (double sr) { assumedSampleRateHz = (sr > 0.0 ? sr : 48000.0); needRebuildAWeight = true; }

    void setLineColour (juce::Colour c) { lineColour = c; repaint(); }

    // Public getters for info display
    int getDisplayBins() const { return displayBins; }
    bool isUsingAWeight() const { return useAWeight; }
    float getFftPacketsPerSecond() const { return audioSource.getFftPacketsPerSecond(); }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        g.fillAll (juce::Colours::black);
        g.setColour (juce::Colours::darkgrey);
        g.drawRect (bounds, 1);

        // plot area + clipping
        const int pad = 4;
        auto plot = bounds.reduced (pad);
        g.saveState();
        g.reduceClipRegion (plot);

        // načítaj FFT z audio source

        std::vector<float> in;
        const int bins = audioSource.getLastFft (in);
        if (bins <= 0)
        {
            g.restoreState();
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (14.0f);
            g.drawText ("ALTF/s: " + juce::String (audioSource.getFftPacketsPerSecond()),
                        bounds.reduced (6), juce::Justification::topLeft, false);
            return;
        }

        // reallocate buffre ak treba
        if ((int) prevInput.size() != bins)
        {
            prevInput.assign ((size_t) bins, 0.0f);
            disp.assign      ((size_t) bins, 0.0f);
            aWeightDb.assign ((size_t) bins, 0.0f);
            needRebuildAWeight = true;
        }

        // A‑weight tabuľka
        if (needRebuildAWeight)
        {
            buildAWeightTable (bins, assumedSampleRateHz);
            needRebuildAWeight = false;
        }

        // A‑weight v dB (voliteľné)
        if (useAWeight)
        {
            const double nyq   = assumedSampleRateHz * 0.5;
            const double binHz = nyq / (double) bins;
            const float cutoffHz  = 5000.0f;
             
            for (int i = 0; i < bins; ++i)
            {
                double dB = (double)assumedDbFloor + (double)in[(size_t)i] * (0.0 - (double)assumedDbFloor);
                const float aStrength = 2.0f;   // 1.0 = normál, 1.3..2.5 = silnejšie
                

                const double f = ( (double)i + 0.5 ) * binHz;
                
                if (f <= cutoffHz)
                    dB += (double)aStrength * (double)aWeightDb[(size_t)i];
                const double n = (dB - (double)assumedDbFloor) / (0.0 - (double)assumedDbFloor);
                in[(size_t)i] = (float) juce::jlimit (0.0, 1.0, n);
            }
        }

        // časové predvyhladenie (EMA) vstupu
        for (int i = 0; i < bins; ++i)
        {
            const float x = in[(size_t)i];
            const float p = prevInput[(size_t)i];
            prevInput[(size_t)i] = p + inputAlpha * (x - p);
        }

        // fixed gain (bez auto-gainu) + manuálny boost
        constexpr float kGain = 1.00f;

        // výstupné vyhladenie (EMA) do disp
        for (int i = 0; i < bins; ++i)
        {
            const float x = juce::jmin (prevInput[(size_t)i] * kGain, 1.0f);
            const float p = disp[(size_t)i];
            if (x > p)
                disp[(size_t)i] = p + attackAlpha  * (x - p);   // attack
            else
                disp[(size_t)i] = p + releaseAlpha * (x - p);  // release
        }
        
        
        
        // --- zmenšenie binov pre zobrazenie (2048 -> 1024 alebo 512) ---
        int drawBins = bins;
        std::vector<float> dispDraw; // bude obsahovať hodnoty pre drawBins

        if (displayBins > 0 && displayBins < bins && (bins % displayBins) == 0)
        {
            drawBins = displayBins;
            const int group = bins / drawBins; // 2 alebo 4
            dispDraw.assign ((size_t) drawBins, 0.0f);

            for (int i = 0; i < drawBins; ++i)
            {
                float m = 0.0f;
                const int start = i * group;
                for (int k = 0; k < group; ++k)
                    m = juce::jmax (m, disp[(size_t) (start + k)]); // berieme maximum

                dispDraw[(size_t) i] = m;
            }
        }
        else
        {
            // skopíruj prvých drawBins (== bins) hodnôt
            dispDraw.assign (disp.begin(), disp.begin() + (size_t) drawBins);
        }

        // -------- log‑resampling + Catmull‑Rom --------
        const int W = plot.getWidth();
        const int H = plot.getHeight();

        const double nyq   = assumedSampleRateHz * 0.5;
        const double binHz = nyq / (double) drawBins;
        const double fmin  = 20.0, fmax = 20000.0;           // 20..20k
        const double logSpan = std::log10 (fmax / fmin);

        // hustota vzorkovania krivky
        int points = juce::jlimit (512, juce::jmax (2 * W, bins), 4096);
        // cap render resolution to avoid heavy work on large windows / high bin counts
        const int maxRenderPoints = 2048; // tweak for perf/quality tradeoff (doubled as requested)
        points = juce::jmin (points, maxRenderPoints);
        std::vector<float> line((size_t) points, 0.0f);

        // Catmull‑Rom vzorka medzi binmi (capture this + bins)
        auto sampleDisp = [&] (int idx) -> float
                {
                    idx = juce::jlimit (0, drawBins - 1, idx);

                    if (! dispDraw.empty())
                        return dispDraw[(size_t) idx];

                    return disp[(size_t) idx];
                };

        for (int i = 0; i < points; ++i)
        {
            const double u = (points == 1 ? 0.0 : (double) i / (double) (points - 1));
            const double f = fmin * std::pow (10.0, u * logSpan);
            double j = f / binHz - 0.5;
            j = juce::jlimit (0.0, (double) drawBins - 1.000001, j);

            const int   j0   = (int) std::floor (j);
            const int   j1   = juce::jmin (drawBins - 1, j0 + 1);
            const float frac = (float) (j - (double) j0);

            const float p0 = sampleDisp (j0 - 1);
            const float p1 = sampleDisp (j0);
            const float p2 = sampleDisp (j1);
            const float p3 = sampleDisp (j1 + 1);

            const float t  = frac;           // 0..1
            const float t2 = t * t;
            const float t3 = t2 * t;

            const float m1 = 0.5f * (p2 - p0);
            const float m2 = 0.5f * (p3 - p1);

            float y = (2*t3 - 3*t2 + 1)*p1 + (t3 - 2*t2 + t)*m1
                    + (-2*t3 + 3*t2)*p2 + (t3 - t2)*m2;

            line[(size_t)i] = juce::jlimit (0.0f, 1.0f, y);
        }

        // ===== log Hz grid (vzdelávacie čiary + popisy) =====
        const double gridHz[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };

        g.setColour (juce::Colours::dimgrey.withAlpha (0.55f));
        g.setFont (12.0f);

        for (double hz : gridHz)
        {
            if (hz < fmin || hz > fmax) continue;

            // log pozícia 0..1
            const double u = std::log10 (hz / fmin) / logSpan;
            const float x = (float) plot.getX() + (float) (u * (double) W);

            // čiara
            g.drawVerticalLine ((int) std::round (x), (float) plot.getY(), (float) plot.getBottom());

            // popis (hore)
            juce::String txt;
            if (hz >= 1000.0)
                txt = juce::String (hz / 1000.0, (hz == 1000 || hz == 2000 || hz == 5000 || hz == 10000 || hz == 20000) ? 0 : 1) + "k";
            else
                txt = juce::String ((int) hz);

            g.drawText (txt, (int) x + 2, plot.getY() + 20, 50, 14, juce::Justification::topLeft);
        }
       

        auto smoothed = line; // TEMP: bez blur

        // fill under curve with a subtle gradient of the line colour
        juce::Path fillPath;
        bool started = false;
        for (int i = 0; i < points; ++i)
        {
            const float u  = (points == 1 ? 0.0f : (float) i / (float) (points - 1));
            const float x  = (float) plot.getX() + u * (float) W;
            float y = (float) plot.getBottom() - smoothed[(size_t)i] * (float) H;
            y = juce::jlimit ((float) plot.getY(), (float) plot.getBottom(), y);
            if (!started) { fillPath.startNewSubPath (x, y); started = true; }
            else          { fillPath.lineTo (x, y); }
        }
        // close and fill
        fillPath.lineTo ((float) plot.getRight(), (float) plot.getBottom());
        fillPath.lineTo ((float) plot.getX(), (float) plot.getBottom());
        fillPath.closeSubPath();

        juce::ColourGradient grad (lineColour.withAlpha (0.25f), (float) plot.getX(), (float) plot.getY(),
                                  lineColour.withAlpha (0.02f), (float) plot.getX(), (float) plot.getBottom(), false);
        g.setGradientFill (grad);
        g.fillPath (fillPath);

        // Color columns: only main frequencies (peaks + harmonics) use the panel's colour;
        // other columns use a subtle background tint derived from the same colour.
        // 1) detect peaks in dispDraw (local maxima)
        float maxMag = 0.0f;
        for (auto v : dispDraw) maxMag = juce::jmax (maxMag, v);

        std::vector<int> peakBins;
        const float peakThreshold = juce::jmax (0.02f, maxMag * 0.28f); // tuneable threshold
        for (int i = 1; i < drawBins - 1; ++i)
        {
            const float v = dispDraw[(size_t) i];
            if (v >= peakThreshold && v >= dispDraw[(size_t) (i-1)] && v >= dispDraw[(size_t) (i+1)])
                peakBins.push_back (i);
        }

        // fallback: use global max if no peaks
        if (peakBins.empty() && maxMag > 0.0001f)
        {
            int imax = 0;
            for (int i = 0; i < drawBins; ++i)
                if (dispDraw[(size_t) i] > dispDraw[(size_t) imax]) imax = i;
            peakBins.push_back (imax);
        }

        // build marked set of resampled points that correspond to peaks and their harmonics
        std::vector<char> mark((size_t) points, 0);
        for (int b : peakBins)
        {
            const double f0 = ((double) b + 0.5) * binHz;
            if (f0 < fmin || f0 > fmax) continue;

            // fundamental and harmonics
            for (int h = 1; h <= 12; ++h)
            {
                const double fh = f0 * (double) h;
                if (fh > fmax) break;
                // map fh to resampled point index
                const double u = (std::log10 (fh / fmin) / logSpan);
                const int pi = (int) std::round (u * (double) (points - 1));
                if (pi < 0 || pi >= points) continue;
                // mark a small neighborhood so columns get some width
                const int spread = juce::jmax (1, (int) (2 - std::log10 ((double) h + 1.0)) );
                for (int s = -spread; s <= spread; ++s)
                {
                    int q = pi + s;
                    if (q >= 0 && q < points) mark[(size_t) q] = 1;
                }
            }
        }

        // draw columns: harmonics (mark) should be more saturated near main peaks; main peaks themselves are thin and subdued
        const float colBaseW = juce::jmax (1.0f, (float) W / (float) points);
        const juce::Colour baseBgCol = lineColour.withAlpha (0.02f); // very subtle base bg tint

        // build small list of main peak resampled indices for distance calculations
        std::vector<int> mainIndices;
        std::vector<char> mainPeak((size_t) points, 0);
        for (int b : peakBins)
        {
            const double f0 = ((double) b + 0.5) * binHz;
            if (f0 < fmin || f0 > fmax) continue;
            const double u = (std::log10 (f0 / fmin) / logSpan);
            const int pi = (int) std::round (u * (double) (points - 1));
            if (pi >= 0 && pi < points)
            {
                mainPeak[(size_t) pi] = 1;
                mainIndices.push_back (pi);
            }
        }

        // cache lineColour HSV for harmonic tinting
        const float lcHue = lineColour.getHue();
        const float lcSat = lineColour.getSaturation();
        const float lcBri = lineColour.getBrightness();

        // fade radius in resampled points around mainPeak where bg columns get emphasized
        const int fadeRadius = juce::jmax (2, (int) std::round ((double) points * 0.008)); // ~0.8% of points, min 2

        for (int i = 0; i < points; ++i)
        {
            const float u = (points == 1 ? 0.0f : (float) i / (float) (points - 1));
            const double f = fmin * std::pow (10.0, u * logSpan);
            if (f < fmin || f > fmax) continue;

            const float x = (float) plot.getX() + u * (float) W;
            const float mag = smoothed[(size_t) i];
            if (mag <= 0.0005f) continue;

            const float hpx = mag * (float) H;
            const float y = (float) plot.getBottom() - hpx;
            const float bottom = (float) plot.getBottom();

            const float alphaTop = juce::jlimit (0.005f, 0.95f, std::pow (mag, 0.6f) * 1.0f);
            const float wcol = colBaseW * 1.1f;

            // compute min distance to any main peak
            int minDist = INT_MAX;
            for (int pi : mainIndices)
            {
                const int d = std::abs (pi - i);
                if (d < minDist) minDist = d;
            }

            float fadeWeight = 0.0f;
            if (minDist <= fadeRadius && ! mainIndices.empty())
            {
                fadeWeight = 1.0f - ((float) minDist / (float) fadeRadius);
                // soften curve
                fadeWeight = fadeWeight * fadeWeight;
            }

            if (mainPeak[(size_t) i])
            {
                // main fundamental: very thin and subdued, same colour as outline
                const float thinW = juce::jmax (1.0f, wcol * 0.18f);
                const float alpha = juce::jlimit (0.005f, 0.50f, alphaTop * 0.45f);
                g.setColour (lineColour.withAlpha (alpha));
                g.fillRect (x - thinW * 0.5f, y, thinW, bottom - y);
            }
            else if (mark[(size_t) i])
            {
                // harmonic/background accent: stronger base saturation and stronger boost near main peaks
                const float baseSat = juce::jmin (1.0f, lcSat * 1.30f);
                const float baseBri = juce::jmin (1.0f, lcBri * 1.03f);
                const float sat = juce::jmin (1.0f, baseSat + fadeWeight * 0.50f);
                const float bri = juce::jmin (1.0f, baseBri + fadeWeight * 0.06f);
                juce::Colour harmCol = juce::Colour::fromHSV (lcHue, sat, bri, 1.0f);
                const float a = juce::jlimit (0.02f, 0.98f, alphaTop * (0.75f + fadeWeight * 0.70f));
                g.setColour (harmCol.withAlpha (a));
                g.fillRect (x - wcol * 0.5f, y, wcol, bottom - y);
            }
            else
            {
                // plain background column (very subtle), but if close to main peak apply a faint emphasis
                const float a = alphaTop * (0.12f + fadeWeight * 0.60f);
                g.setColour (baseBgCol.withAlpha (juce::jlimit (0.005f, 0.8f, a)));
                g.fillRect (x - wcol * 0.5f, y, wcol, bottom - y);
            }
        }

        // finally draw outline path on top
        juce::Path path;
        started = false;
        for (int i = 0; i < points; ++i)
        {
            const float u  = (points == 1 ? 0.0f : (float) i / (float) (points - 1));
            const float x  = (float) plot.getX() + u * (float) W;
            float y = (float) plot.getBottom() - smoothed[(size_t)i] * (float) H;
            y = juce::jlimit ((float) plot.getY(), (float) plot.getBottom(), y);
            if (!started) { path.startNewSubPath (x, y); started = true; }
            else          { path.lineTo (x, y); }
        }

        g.setColour (lineColour);
        g.strokePath (path, juce::PathStrokeType (2.0f));
        g.restoreState(); // koniec clippingu

        // NOTE: Text info is now rendered by PanelHost (always horizontal)
    }

private:
    IAudioSource& audioSource;

    int displayBins = 2048;
    // A‑weighting (dB/bin)
    bool  useAWeight = false;
    std::vector<float> aWeightDb;
    bool  needRebuildAWeight = true;
    double assumedSampleRateHz = 48000.0;
    float  assumedDbFloor      = -90.0f;

    // časové smoothingy
    float inputAlpha    = 0.50f;  // rýchlosť pred-EMA, nechaj cca stredne rýchle
    float attackAlpha   = 0.8f;   // rýchle stúpanie (0.3 – 0.7 odporúčané)
    float releaseAlpha  = 0.1f;  // pomalé klesanie (0.01 – 0.15 odporúčané)

    // buffre
    std::vector<float> prevInput;
    std::vector<float> disp;
    juce::Colour lineColour = juce::Colours::red;

    // A‑weight tabuľka
    void buildAWeightTable (int bins, double sampleRateHz)
    {
        aWeightDb.resize ((size_t) bins, 0.0f);
        const double nyq   = sampleRateHz * 0.5;
        const double binHz = nyq / (double) bins;

        for (int i = 0; i < bins; ++i)
            aWeightDb[(size_t)i] = aWeighting_dB ((i + 0.5) * binHz);
    }

    static float aWeighting_dB (double f)
    {
        if (f <= 0.0) return 0.0f;
        const double f2  = f * f;
        const double num = (12200.0 * 12200.0) * f2 * f2;
        const double den = (f2 + 20.6 * 20.6)
                         * (f2 + 12200.0 * 12200.0)
                         * std::sqrt ((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9));
        return (float) (20.0 * std::log10 (num / den) + 2.0);
    }
};