#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <vector>
#include "AudioSourceInterface.h"

// Real-time spectrum renderer:
// - EMA smoothing (attack/release, štandardizované)
// - log-X resampling, linear interp (measurement) alebo Catmull-Rom (visual)
// - Psychoacoustic curve: Flat / A-weight / ISO 226 (Fletcher-Munson)
// - Peak Hold krivka (pomalý decay)
// - Measurement mode: power-average downsample, linear interp
// - Harmonics overlay: len zmena farby, NIE hodnôt krivky

class VisualSpectrum : public juce::Component
{
public:
    explicit VisualSpectrum (IAudioSource& r) : audioSource (r) { setOpaque (true); }

    // --- Settery ---
    void setDisplayBins (int b)
    {
        if (b != 512 && b != 1024 && b != 2048) b = 2048;
        displayBins = b; repaint();
    }

    // smooth 0..1
    void setSmoothAmount (float s)
    {
        s = juce::jlimit (0.0f, 1.0f, s);
        // Štandardizovaná EMA: s=0 → okamžitá odozva, s=1 → max vyhladzovanie
        // attack je vždy rýchlejší ako release (SPAN-like)
        const float a = juce::jmap (s, 0.0f, 1.0f, 0.0f, 0.92f);
        inputAlpha   = 1.0f - juce::jlimit (0.02f, 0.85f, a);          // EMA koeficient vstupu
        attackAlpha  = juce::jlimit (0.15f, 0.95f, 1.0f - a * 0.6f);   // rýchle stúpanie
        releaseAlpha = juce::jlimit (0.02f, 0.35f, 1.0f - a * 0.95f);  // pomalé klesanie
    }

    void setAssumedDbFloor    (float dB)  { assumedDbFloor = dB; }
    void setAssumedSampleRate (double sr) { assumedSampleRateHz = (sr > 0.0 ? sr : 48000.0); needRebuildTables = true; }
    void setLineColour        (juce::Colour c) { lineColour = c; repaint(); }

    // Psychoacoustic curve: 0=Flat, 1=A-weight, 2=ISO226
    void setPsychoacousticMode (int mode, int phonLevel = 60)
    {
        psychoMode  = juce::jlimit (0, 2, mode);
        phonLevelDb = juce::jlimit (20, 100, phonLevel);
        needRebuildTables = true;
        repaint();
    }

    // legacy
    void setUseAWeight (bool on) { setPsychoacousticMode (on ? 1 : 0, phonLevelDb); }

    // Peak hold: true = zobrazí druhú krivku nad hlavnou
    void setPeakHoldEnabled (bool on) { peakHoldEnabled = on; if (!on) peakHold.clear(); repaint(); }

    // Measurement mode: true = SPAN-like (linear interp, power-avg downsample)
    void setMeasurementMode (bool on) { measurementMode = on; repaint(); }

    // Getters
    int  getDisplayBins()          const { return displayBins; }
    bool isUsingAWeight()          const { return psychoMode == 1; }
    float getFftPacketsPerSecond() const { return audioSource.getFftPacketsPerSecond(); }

    // =====================================================================
    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        g.fillAll (juce::Colours::black);
        g.setColour (juce::Colours::darkgrey);
        g.drawRect (bounds, 1);

        // --- Responzívna veľkosť fontu ---
        const int shortSide = juce::jmin (bounds.getWidth(), bounds.getHeight());
        const float fSzGrid = juce::jlimit (6.0f, 10.0f, (float) shortSide * 0.05f);
        const float fSzInfo = juce::jlimit (7.0f, 11.0f, (float) shortSide * 0.058f);

        // --- Max value strip navrchu ---
        const int stripH = juce::jlimit (12, 17, (int)(fSzInfo * 1.5f));
        auto stripRect = bounds.reduced(1).removeFromTop (stripH);
        g.setColour (juce::Colour (0xFF1A1A1A));
        g.fillRect (stripRect);

        const int pad = 4;
        auto plot = bounds.reduced (pad);
        plot.setTop (plot.getY() + stripH);
        g.saveState();
        g.reduceClipRegion (plot);

        std::vector<float> in;
        const int bins = audioSource.getLastFft (in);
        if (bins <= 0)
        {
            g.restoreState();
            g.setFont (fSzInfo);
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.drawText ("ALTF/s: " + juce::String (audioSource.getFftPacketsPerSecond()),
                        stripRect, juce::Justification::centredLeft, false);
            return;
        }

        // --- realloc bufferov ---
        if ((int) prevInput.size() != bins)
        {
            prevInput.assign  ((size_t) bins, 0.0f);
            disp.assign       ((size_t) bins, 0.0f);
            aWeightDb.assign  ((size_t) bins, 0.0f);
            iso226Db.assign   ((size_t) bins, 0.0f);
            peakHold.assign   ((size_t) bins, 0.0f);
            needRebuildTables = true;
        }

        if (needRebuildTables)
        {
            buildAWeightTable  (bins, assumedSampleRateHz);
            buildISO226Table   (bins, assumedSampleRateHz, phonLevelDb);
            needRebuildTables  = false;
        }

        // --- Psychoacoustic curve v dB doméne ---
        if (psychoMode != 0)
        {
            const double nyq   = assumedSampleRateHz * 0.5;
            const double binHz = nyq / (double) bins;
            constexpr float aStrength = 1.0f;

            for (int i = 0; i < bins; ++i)
            {
                double dB = (double) assumedDbFloor + (double) in[(size_t) i] * (0.0 - (double) assumedDbFloor);

                if (psychoMode == 1)
                    dB += aStrength * (double) aWeightDb[(size_t) i];
                else if (psychoMode == 2)
                    dB += aStrength * (double) iso226Db[(size_t) i];

                const double n = (dB - (double) assumedDbFloor) / (0.0 - (double) assumedDbFloor);
                in[(size_t) i] = (float) juce::jlimit (0.0, 1.0, n);
            }
        }

        // --- Vstupná EMA ---
        for (int i = 0; i < bins; ++i)
        {
            const float x = in[(size_t) i];
            const float p = prevInput[(size_t) i];
            prevInput[(size_t) i] = p + inputAlpha * (x - p);
        }

        // --- Výstupná EMA (attack/release) ---
        constexpr float kGain = 1.0f;
        for (int i = 0; i < bins; ++i)
        {
            const float x = juce::jmin (prevInput[(size_t) i] * kGain, 1.0f);
            const float p = disp[(size_t) i];
            disp[(size_t) i] = (x > p) ? p + attackAlpha  * (x - p)
                                        : p + releaseAlpha * (x - p);
        }

        // --- Downsample binov ---
        int drawBins = bins;
        std::vector<float> dispDraw;

        if (displayBins > 0 && displayBins < bins && (bins % displayBins) == 0)
        {
            drawBins = displayBins;
            const int group = bins / drawBins;
            dispDraw.assign ((size_t) drawBins, 0.0f);

            if (measurementMode)
            {
                // Measurement: power-average (vernejší energii)
                const double floor_lin = std::pow (10.0, (double) assumedDbFloor / 10.0);
                for (int i = 0; i < drawBins; ++i)
                {
                    double sumP = 0.0;
                    const int start = i * group;
                    for (int k = 0; k < group; ++k)
                    {
                        // 0..1 → dB → power
                        const double v  = (double) disp[(size_t) (start + k)];
                        const double dB = (double) assumedDbFloor + v * (0.0 - (double) assumedDbFloor);
                        sumP += std::pow (10.0, dB / 10.0);
                    }
                    const double avgP  = sumP / (double) group;
                    const double avgDb = 10.0 * std::log10 (juce::jmax (avgP, floor_lin));
                    const double n = (avgDb - (double) assumedDbFloor) / (0.0 - (double) assumedDbFloor);
                    dispDraw[(size_t) i] = (float) juce::jlimit (0.0, 1.0, n);
                }
            }
            else
            {
                // Visual: maximum (pôvodné správanie)
                for (int i = 0; i < drawBins; ++i)
                {
                    float m = 0.0f;
                    const int start = i * group;
                    for (int k = 0; k < group; ++k)
                        m = juce::jmax (m, disp[(size_t) (start + k)]);
                    dispDraw[(size_t) i] = m;
                }
            }
        }
        else
        {
            dispDraw.assign (disp.begin(), disp.begin() + (size_t) drawBins);
        }

        // --- Peak Hold update ---
        if (peakHoldEnabled)
        {
            if ((int) peakHold.size() != drawBins)
                peakHold.assign ((size_t) drawBins, 0.0f);

            constexpr float kDecay = 0.998f; // pomalý decay per frame
            for (int i = 0; i < drawBins; ++i)
            {
                peakHold[(size_t) i] = juce::jmax (dispDraw[(size_t) i],
                                                    peakHold[(size_t) i] * kDecay);
            }
        }

        // --- log-resampling ---
        const int W = plot.getWidth();
        const int H = plot.getHeight();

        const double nyq     = assumedSampleRateHz * 0.5;
        const double binHz   = nyq / (double) drawBins;
        const double fmin    = 20.0, fmax = 20000.0;
        const double logSpan = std::log10 (fmax / fmin);

        int points = juce::jmin (juce::jmax (2 * W, drawBins), 4096);
        points = juce::jmin (points, 2048);
        std::vector<float> line      ((size_t) points, 0.0f);
        std::vector<float> peakLine  ((size_t) points, 0.0f);

        auto sampleBuf = [&] (const std::vector<float>& buf, int idx) -> float
        {
            return buf[(size_t) juce::jlimit (0, (int) buf.size() - 1, idx)];
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

            if (measurementMode)
            {
                // Linear interpolácia (žiadny overshoot – SPAN-like)
                const float p1 = sampleBuf (dispDraw, j0);
                const float p2 = sampleBuf (dispDraw, j1);
                line[(size_t) i] = juce::jlimit (0.0f, 1.0f, p1 + frac * (p2 - p1));

                if (peakHoldEnabled)
                {
                    const float h1 = sampleBuf (peakHold, j0);
                    const float h2 = sampleBuf (peakHold, j1);
                    peakLine[(size_t) i] = juce::jlimit (0.0f, 1.0f, h1 + frac * (h2 - h1));
                }
            }
            else
            {
                // Catmull-Rom (vizuálne plynulé)
                const float p0 = sampleBuf (dispDraw, j0 - 1);
                const float p1 = sampleBuf (dispDraw, j0);
                const float p2 = sampleBuf (dispDraw, j1);
                const float p3 = sampleBuf (dispDraw, j1 + 1);
                const float t  = frac, t2 = t * t, t3 = t2 * t;
                const float m1 = 0.5f * (p2 - p0);
                const float m2 = 0.5f * (p3 - p1);
                float y = (2*t3 - 3*t2 + 1)*p1 + (t3 - 2*t2 + t)*m1
                        + (-2*t3 + 3*t2)*p2 + (t3 - t2)*m2;
                line[(size_t) i] = juce::jlimit (0.0f, 1.0f, y);

                if (peakHoldEnabled)
                {
                    const float h0 = sampleBuf (peakHold, j0 - 1);
                    const float h1 = sampleBuf (peakHold, j0);
                    const float h2 = sampleBuf (peakHold, j1);
                    const float h3 = sampleBuf (peakHold, j1 + 1);
                    const float n1 = 0.5f * (h2 - h0);
                    const float n2 = 0.5f * (h3 - h1);
                    float hy = (2*t3 - 3*t2 + 1)*h1 + (t3 - 2*t2 + t)*n1
                             + (-2*t3 + 3*t2)*h2 + (t3 - t2)*n2;
                    peakLine[(size_t) i] = juce::jlimit (0.0f, 1.0f, hy);
                }
            }
        }

        // --- dB Y-os grid ---
        {
            const float dbFloor = assumedDbFloor;
            const float dbGrid[] = { 0.0f, -6.0f, -12.0f, -18.0f, -24.0f, -36.0f, -48.0f, -60.0f, -72.0f, -84.0f };
            // Minimálna výška aby sme ukazali label (inak len grid čiara)
            const bool showLabels = (H >= 60);
            g.setFont (fSzGrid);
            for (float db : dbGrid)
            {
                if (db < dbFloor) break;
                const float norm = (db - dbFloor) / (0.0f - dbFloor);
                const float yPos = (float) plot.getBottom() - norm * (float) H;
                g.setColour (juce::Colours::grey.withAlpha (db == 0.0f ? 0.5f : 0.2f));
                g.drawHorizontalLine ((int) std::round (yPos), (float) plot.getX(), (float) plot.getRight());
                if (showLabels)
                {
                    g.setColour (juce::Colours::grey.withAlpha (0.7f));
                    juce::String lbl = (db == 0.0f) ? "0" : juce::String ((int) db);
                    g.drawText (lbl, plot.getX() + 2, (int) yPos - (int)fSzGrid - 1,
                                (int)(fSzGrid * 3.5f), (int)(fSzGrid + 2), juce::Justification::centredLeft);
                }
            }
        }

        // --- Hz X-os grid ---
        const double gridHz[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
        g.setColour (juce::Colours::dimgrey.withAlpha (0.55f));
        g.setFont (fSzGrid);
        const bool showHzLabels = (W >= 80);
        for (double hz : gridHz)
        {
            if (hz < fmin || hz > fmax) continue;
            const double u = std::log10 (hz / fmin) / logSpan;
            const float x = (float) plot.getX() + (float) (u * (double) W);
            g.drawVerticalLine ((int) std::round (x), (float) plot.getY(), (float) plot.getBottom());
            if (showHzLabels)
            {
                juce::String txt;
                if (hz >= 1000.0)
                    txt = juce::String (hz / 1000.0, (hz==1000||hz==2000||hz==5000||hz==10000||hz==20000)?0:1) + "k";
                else
                    txt = juce::String ((int) hz);
                g.drawText (txt, (int) x + 2, plot.getY() + (int)(fSzGrid * 2.2f),
                            (int)(fSzGrid * 4.0f), (int)(fSzGrid + 2), juce::Justification::topLeft);
            }
        }

        const auto& smoothed = line;

        // --- Harmonics detection (overlay bez zmeny hodnôt krivky) ---
        float maxMag = 0.0f;
        for (auto v : dispDraw) maxMag = juce::jmax (maxMag, v);

        std::vector<int> peakBins;
        const float peakThreshold = juce::jmax (0.02f, maxMag * 0.28f);
        for (int i = 1; i < drawBins - 1; ++i)
        {
            const float v = dispDraw[(size_t) i];
            if (v >= peakThreshold && v >= dispDraw[(size_t)(i-1)] && v >= dispDraw[(size_t)(i+1)])
                peakBins.push_back (i);
        }
        if (peakBins.empty() && maxMag > 0.0001f)
        {
            int imax = 0;
            for (int i = 0; i < drawBins; ++i)
                if (dispDraw[(size_t)i] > dispDraw[(size_t)imax]) imax = i;
            peakBins.push_back (imax);
        }

        std::vector<char> mark      ((size_t) points, 0);
        std::vector<char> mainPeak  ((size_t) points, 0);
        std::vector<int>  mainIndices;

        for (int b : peakBins)
        {
            const double f0 = ((double) b + 0.5) * binHz;
            if (f0 < fmin || f0 > fmax) continue;
            // main peak index
            const double um = std::log10 (f0 / fmin) / logSpan;
            const int pm = (int) std::round (um * (double)(points-1));
            if (pm >= 0 && pm < points) { mainPeak[(size_t)pm] = 1; mainIndices.push_back(pm); }
            // harmonics
            for (int h = 1; h <= 12; ++h)
            {
                const double fh = f0 * (double) h;
                if (fh > fmax) break;
                const double u = std::log10 (fh / fmin) / logSpan;
                const int pi = (int) std::round (u * (double)(points-1));
                if (pi < 0 || pi >= points) continue;
                const int spread = juce::jmax (1, (int)(2 - std::log10((double)h+1.0)));
                for (int s = -spread; s <= spread; ++s)
                {
                    int q = pi + s;
                    if (q >= 0 && q < points) mark[(size_t)q] = 1;
                }
            }
        }

        // --- Fill pod krivkou ---
        {
            juce::Path fillPath;
            bool started = false;
            for (int i = 0; i < points; ++i)
            {
                const float u = (points == 1 ? 0.0f : (float)i / (float)(points-1));
                const float x = (float)plot.getX() + u * (float)W;
                float y = (float)plot.getBottom() - smoothed[(size_t)i] * (float)H;
                y = juce::jlimit ((float)plot.getY(), (float)plot.getBottom(), y);
                if (!started) { fillPath.startNewSubPath(x,y); started=true; }
                else          fillPath.lineTo(x,y);
            }
            fillPath.lineTo ((float)plot.getRight(),  (float)plot.getBottom());
            fillPath.lineTo ((float)plot.getX(),      (float)plot.getBottom());
            fillPath.closeSubPath();
            juce::ColourGradient grad (lineColour.withAlpha(0.25f), (float)plot.getX(), (float)plot.getY(),
                                       lineColour.withAlpha(0.02f), (float)plot.getX(), (float)plot.getBottom(), false);
            g.setGradientFill (grad);
            g.fillPath (fillPath);
        }

        // --- Stĺpce s harmonics overlay (hodnoty krivky sa NEMENIA) ---
        const float colBaseW  = juce::jmax (1.0f, (float)W / (float)points);
        const juce::Colour baseBgCol = lineColour.withAlpha (0.02f);
        const float lcHue = lineColour.getHue();
        const float lcSat = lineColour.getSaturation();
        const float lcBri = lineColour.getBrightness();
        const int fadeRadius = juce::jmax (2, (int)std::round((double)points * 0.008));

        for (int i = 0; i < points; ++i)
        {
            const float u   = (points==1 ? 0.0f : (float)i / (float)(points-1));
            const float x   = (float)plot.getX() + u * (float)W;
            const float mag = smoothed[(size_t)i];
            if (mag <= 0.0005f) continue;

            const float hpx = mag * (float)H;
            const float y   = (float)plot.getBottom() - hpx;
            const float bot = (float)plot.getBottom();
            const float alphaTop = juce::jlimit (0.005f, 0.95f, std::pow (mag, 0.6f));
            const float wcol = colBaseW * 1.1f;

            int minDist = INT_MAX;
            for (int pi : mainIndices)
                minDist = juce::jmin (minDist, std::abs(pi - i));

            float fadeWeight = 0.0f;
            if (minDist <= fadeRadius && !mainIndices.empty())
                fadeWeight = std::pow (1.0f - (float)minDist / (float)fadeRadius, 2.0f);

            if (mainPeak[(size_t)i])
            {
                const float thinW = juce::jmax (1.0f, wcol * 0.18f);
                g.setColour (lineColour.withAlpha (juce::jlimit (0.005f, 0.50f, alphaTop * 0.45f)));
                g.fillRect (x - thinW*0.5f, y, thinW, bot - y);
            }
            else if (mark[(size_t)i])
            {
                const float sat = juce::jmin (1.0f, lcSat * 1.30f + fadeWeight * 0.50f);
                const float bri = juce::jmin (1.0f, lcBri * 1.03f + fadeWeight * 0.06f);
                g.setColour (juce::Colour::fromHSV (lcHue, sat, bri, 1.0f)
                                          .withAlpha (juce::jlimit (0.02f, 0.98f, alphaTop * (0.75f + fadeWeight * 0.70f))));
                g.fillRect (x - wcol*0.5f, y, wcol, bot - y);
            }
            else
            {
                g.setColour (baseBgCol.withAlpha (juce::jlimit (0.005f, 0.8f, alphaTop * (0.12f + fadeWeight * 0.60f))));
                g.fillRect (x - wcol*0.5f, y, wcol, bot - y);
            }
        }

        // --- Hlavná krivka ---
        {
            juce::Path path;
            bool started = false;
            for (int i = 0; i < points; ++i)
            {
                const float u = (points==1?0.0f:(float)i/(float)(points-1));
                const float x = (float)plot.getX() + u*(float)W;
                float y = (float)plot.getBottom() - smoothed[(size_t)i]*(float)H;
                y = juce::jlimit ((float)plot.getY(), (float)plot.getBottom(), y);
                if (!started) { path.startNewSubPath(x,y); started=true; }
                else          path.lineTo(x,y);
            }
            g.setColour (lineColour);
            g.strokePath (path, juce::PathStrokeType (2.0f));
        }

        // --- Peak Hold krivka (nad hlavnou, tenká, svetlá) ---
        if (peakHoldEnabled)
        {
            juce::Path pkPath;
            bool started = false;
            for (int i = 0; i < points; ++i)
            {
                const float u = (points==1?0.0f:(float)i/(float)(points-1));
                const float x = (float)plot.getX() + u*(float)W;
                float y = (float)plot.getBottom() - peakLine[(size_t)i]*(float)H;
                y = juce::jlimit ((float)plot.getY(), (float)plot.getBottom(), y);
                if (!started) { pkPath.startNewSubPath(x,y); started=true; }
                else          pkPath.lineTo(x,y);
            }
            g.setColour (lineColour.brighter(0.6f).withAlpha(0.75f));
            g.strokePath (pkPath, juce::PathStrokeType (1.0f));
        }

        g.restoreState();

        // --- Max value strip: nájdi peak bin a vykresli info ---
        {
            // Nájdi bin s najvyššou hodnotou v dispDraw
            int peakBinIdx = 0;
            float peakVal  = 0.0f;
            for (int i = 0; i < drawBins; ++i)
            {
                if (dispDraw[(size_t)i] > peakVal)
                {
                    peakVal    = dispDraw[(size_t)i];
                    peakBinIdx = i;
                }
            }

            // Session max sledovanie
            if (peakVal > specSessionMaxVal)
            {
                specSessionMaxVal    = peakVal;
                specSessionMaxBinIdx = peakBinIdx;
            }

            // Freq a dBFS aktuálneho framu
            const double nyqS   = assumedSampleRateHz * 0.5;
            const double binHzS = nyqS / (double) drawBins;
            const double peakHz = ((double) peakBinIdx + 0.5) * binHzS;
            const float  peakDb = assumedDbFloor + peakVal * (0.0f - assumedDbFloor);

            // Session max freq + dBFS
            const double maxHz  = ((double) specSessionMaxBinIdx + 0.5) * binHzS;
            const float  maxDb  = assumedDbFloor + specSessionMaxVal * (0.0f - assumedDbFloor);

            // Formátovanie frekvencií
            auto fmtHz = [] (double hz) -> juce::String {
                if (hz >= 1000.0) return juce::String (hz / 1000.0, 2) + " kHz";
                return juce::String ((int) hz) + " Hz";
            };

            // Vykreslenie do stripu
            juce::String stripText = "PEAK  "
                + fmtHz (peakHz) + "  " + juce::String (peakDb, 1) + " dBFS"
                + "     MAX  "
                + fmtHz (maxHz)  + "  " + juce::String (maxDb,  1) + " dBFS";

            g.setFont (fSzInfo);
            g.setColour (juce::Colours::lightgrey.withAlpha (0.9f));
            auto labelArea = stripRect;
            g.drawText (stripText, labelArea.reduced (3, 0), juce::Justification::centredLeft, false);

            // Reset gombík "R" vpravo
            auto resetBtn = stripRect.removeFromRight (stripH + 4);
            g.setColour (juce::Colour (0xFF333333));
            g.fillRect (resetBtn.reduced (1));
            g.setColour (juce::Colours::grey);
            g.setFont (fSzInfo);
            g.drawText ("R", resetBtn, juce::Justification::centred, false);
            specResetBtnBounds = resetBtn;
        }
    }

private:
    IAudioSource& audioSource;

    // --- Nastavenia ---
    int   displayBins      = 2048;
    int   psychoMode       = 0;       // 0=Flat, 1=A-weight, 2=ISO226
    int   phonLevelDb      = 60;
    bool  peakHoldEnabled  = false;
    bool  measurementMode  = false;
    bool  needRebuildTables = true;
    double assumedSampleRateHz = 48000.0;
    float  assumedDbFloor      = -90.0f;

    // --- EMA koeficienty (štandardizované) ---
    float inputAlpha   = 0.50f;
    float attackAlpha  = 0.70f;
    float releaseAlpha = 0.08f;

    // --- Buffre ---
    std::vector<float> prevInput;
    std::vector<float> disp;
    std::vector<float> aWeightDb;
    std::vector<float> iso226Db;
    std::vector<float> peakHold;
    juce::Colour lineColour = juce::Colours::red;

    // --- Session max pre strip ---
    float specSessionMaxVal    = 0.0f;
    int   specSessionMaxBinIdx = 0;
    juce::Rectangle<int> specResetBtnBounds;

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (specResetBtnBounds.contains (e.getPosition()))
        {
            specSessionMaxVal    = 0.0f;
            specSessionMaxBinIdx = 0;
            repaint();
        }
    }

    // --- A-weighting ---
    void buildAWeightTable (int bins, double sr)
    {
        aWeightDb.resize ((size_t) bins, 0.0f);
        const double binHz = sr * 0.5 / (double) bins;
        for (int i = 0; i < bins; ++i)
            aWeightDb[(size_t)i] = aWeighting_dB ((i + 0.5) * binHz);
    }

    static float aWeighting_dB (double f)
    {
        if (f <= 0.0) return 0.0f;
        const double f2  = f * f;
        const double num = (12200.0*12200.0) * f2 * f2;
        const double den = (f2 + 20.6*20.6) * (f2 + 12200.0*12200.0)
                         * std::sqrt ((f2 + 107.7*107.7) * (f2 + 737.9*737.9));
        return (float) (20.0 * std::log10 (num / den) + 2.0);
    }

    // --- ISO 226:2003 (Fletcher-Munson) equal-loudness ---
    // Vráti korekciu v dB pre dané frekvencie pri zadanom phone leveli.
    // Použijeme štandardné tabuľkové hodnoty ISO 226:2003 pre 11 frekvencií
    // a lineárne interpolujeme v log-f doméne.
    void buildISO226Table (int bins, double sr, int phon)
    {
        iso226Db.resize ((size_t) bins, 0.0f);
        const double binHz = sr * 0.5 / (double) bins;

        // ISO 226:2003 tabuľka: frekvencia [Hz], Lf (SPL pri 0 phon) a Tf (prah)
        // Korekcia = phon - SPL(f) pri 1 kHz normalizovaná
        // Zjednodušená implementácia: equal loudness contour pre dané phon
        // alfa_f a Lu_f z ISO 226:2003 Table 1
        static const double iso_f[]   = { 20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160,
                                           200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
                                           2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500 };
        static const double iso_af[]  = { 0.532,0.506,0.480,0.455,0.432,0.409,0.387,0.367,0.349,0.330,
                                           0.315,0.301,0.288,0.276,0.267,0.259,0.253,0.250,0.246,0.244,
                                           0.243,0.243,0.243,0.242,0.242,0.245,0.254,0.271,0.301 };
        static const double iso_Lu[]  = {-31.6,-27.2,-23.0,-19.1,-15.9,-13.0,-10.3,-8.1,-6.2,-4.5,
                                           -3.1,-2.0,-1.1,-0.4, 0.0, 0.3, 0.5, 0.0,-2.7,-4.1,
                                           -1.0, 1.7, 2.5, 1.2,-2.1,-7.1,-11.2,-10.7,-3.1 };
        static const double iso_Tf[]  = { 78.5,68.7,59.5,51.1,44.0,37.5,31.5,26.5,22.1,17.9,
                                           14.4,11.4, 8.6, 6.2, 4.4, 3.0, 2.2, 2.4, 3.5, 1.7,
                                           -1.3,-4.2,-6.0,-5.4,-1.5, 6.0,12.6,13.9,12.3 };
        const int N = 29;

        // Výpočet SPL pri danom phone pre každú frekvenciu v tabuľke
        // ISO 226: Lp = (10/alpha_f) * log10( (L_N^(alpha_f/10) - 4*10^-10 * Lu_f^(alpha_f/10))^(10/alpha_f) ) + Tf
        // Zjednodušenie: SPL(f,phon) = Tf + ( phon - Lp_1kHz_ref ) / alpha_f * 10 ... pozri normu
        // Priamočiara implementácia podľa ISO 226:2003 eq.4:
        // Af = 4.47e-3 * (10^(0.025*Ln) - 1.15) + (0.4 * 10^( (Tf+Lu)/10 - 9 ))^alpha_f
        // Lp = (10/alpha_f) * log10(Af) - Lu + 94

        auto iso226_spl = [&] (int idx, double ln) -> double
        {
            const double af  = iso_af[idx];
            const double lu  = iso_Lu[idx];
            const double tf  = iso_Tf[idx];
            const double Af  = 4.47e-3 * (std::pow (10.0, 0.025 * ln) - 1.15)
                             + std::pow (0.4 * std::pow (10.0, (tf + lu) / 10.0 - 9.0), af);
            return (10.0 / af) * std::log10 (juce::jmax (Af, 1e-30)) - lu + 94.0;
        };

        // SPL pri 1 kHz (index 17) pre normalizáciu
        const double spl1k = iso226_spl (17, (double) phon);

        // Korekcia na poslednom bode tabuľky (12500 Hz) – potrebná pre fade nad 12.5kHz
        const double corrAtLast = spl1k - iso226_spl (N - 1, (double) phon);
        // Korekcia na prvom bode tabuľky (20 Hz) – pre fade pod 20 Hz
        const double corrAtFirst = spl1k - iso226_spl (0, (double) phon);

        // Fade koniec: nad poslednou hodnotou tabuľky plynule taper do 0 pri 20kHz
        const double fadeEndHz = 20000.0;

        for (int i = 0; i < bins; ++i)
        {
            const double f = (i + 0.5) * binHz;

            if (f <= 0.0)
            {
                iso226Db[(size_t)i] = 0.0f;
                continue;
            }

            // Pod minimálnou frekvenciou tabuľky (20 Hz): taper od 0 na corrAtFirst
            if (f < iso_f[0])
            {
                const double t = f / iso_f[0]; // 0..1
                iso226Db[(size_t)i] = (float) (corrAtFirst * t);
                continue;
            }

            // Nad maximálnou frekvenciou tabuľky (12500 Hz):
            // plynulý cosine fade z corrAtLast na 0 pri 20kHz – žiadny schod
            if (f > iso_f[N - 1])
            {
                if (f >= fadeEndHz)
                {
                    iso226Db[(size_t)i] = 0.0f;
                }
                else
                {
                    // cosine taper: plynší ako lineárny (žiadny "kink" na 12.5kHz)
                    const double tLin = (f - iso_f[N-1]) / (fadeEndHz - iso_f[N-1]); // 0..1
                    const double tCos = 0.5 * (1.0 - std::cos (tLin * juce::MathConstants<double>::pi)); // 0..1 smooth
                    iso226Db[(size_t)i] = (float) (corrAtLast * (1.0 - tCos));
                }
                continue;
            }

            // Normálny prípad: lineárna interpolácia v log-f doméne
            int lo = 0;
            for (int k = 0; k < N - 1; ++k)
                if (iso_f[k] <= f && f <= iso_f[k+1]) { lo = k; break; }

            const double logF  = std::log10 (f);
            const double logF0 = std::log10 (iso_f[lo]);
            const double logF1 = std::log10 (iso_f[lo+1]);
            const double t     = (logF - logF0) / (logF1 - logF0);

            const double spl0 = iso226_spl (lo,   (double) phon);
            const double spl1 = iso226_spl (lo+1, (double) phon);
            const double spl  = spl0 + t * (spl1 - spl0);

            // Korekcia = spl1k - spl:
            // záporná tam kde sluch nie je citlivý (nízke + vysoké f) → potlačenie
            // kladná tam kde sluch je citlivejší (okolo 3-4kHz) → zvýraznenie
            iso226Db[(size_t)i] = (float) (spl1k - spl);
        }
    }
};
