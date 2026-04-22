/*
  ==============================================================================

    ChladniPaterns.h
    Created: 21 Apr 2026 10:55:46am
    Author:  Martin Peroncik

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include "AudioSourceInterface.h"
#include <cmath>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  ChladniPatternMeter
//
//  Simulates Chladni figures on a rectangular plate with FREE edge boundary
//  conditions. Sand accumulates at nodal lines (where Z ≈ 0).
//
//  Chladni function (free-edge, cos-cos antisymmetric form):
//      Z(x,y) = cos(n·π·x)·cos(m·π·y/r) − cos(m·π·x)·cos(n·π·y/r)
//  Source: Leissa "Vibration of Plates" (1969); matches p5.js reference impl.
//
//  Physical resonance frequency (rectangular plate, approx. centre excitation):
//      f(m,n) = k · (m²/r² + n²)
//      k = π/(2·L²) · sqrt(D/(ρ·h)),   D = Eh³ / (12·(1−ν²))
//      r = aspect ratio Lx/Ly
//
//  NOTE: When m == n the antisymmetric form Z ≡ 0 → blank pattern. Use m ≠ n.
// ─────────────────────────────────────────────────────────────────────────────
class ChladniPatternMeter : public juce::Component, private juce::Timer
{
public:
    // ── Construction ─────────────────────────────────────────────────────────
    explicit ChladniPatternMeter (IAudioSource& src) : audioSource (src)
    {
        setOpaque (true);
        startTimerHz (30);
    }

    ~ChladniPatternMeter() override { stopTimer(); }

    // ── Control API ──────────────────────────────────────────────────────────

    void setM (int v)
    {
        if (m != v) { m = juce::jlimit (1, 8, v); triggerSettle(); }
    }

    void setN (int v)
    {
        if (n != v) { n = juce::jlimit (1, 8, v); triggerSettle(); }
    }

    /** Aspect ratio r = Lx/Ly  (0.25–4.0, default 1.0 = square plate). */
    void setAspectRatio (float r)
    {
        ar = juce::jlimit (0.25f, 4.0f, r);
        markDirty();
    }

    /** Sand sharpness 0–1:  0 = soft diffuse,  1 = thin crisp nodal lines. */
    void setSandSharpness (float s01)
    {
        userSharpness = juce::jmap (juce::jlimit (0.0f, 1.0f, s01),
                                    0.0f, 1.0f, 4.0f, 120.0f);
        markDirty();
    }

    /** Sand colour (default: warm cream). */
    void setSandColour (juce::Colour c) { sandColour = c; markDirty(); }

    /** Plate material preset: 0=Aluminium, 1=Steel, 2=Glass, 3=Acrylic. */
    void setMaterial (int idx) { applyMaterial (idx); markDirty(); }

    int   getM()               const noexcept { return m; }
    int   getN()               const noexcept { return n; }
    float getAspectRatio()     const noexcept { return ar; }
    float getTheoreticalFreq() const noexcept { return computeFreq (m, n); }
    bool  isDegenerate()       const noexcept { return m == n; }

    // ── Component overrides ──────────────────────────────────────────────────
    void resized() override { markDirty(); }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds();
        g.fillAll (juce::Colours::black);

        const int W = b.getWidth();
        const int H = b.getHeight();
        if (W < 4 || H < 4) return;

        const float fs     = juce::jlimit (9.0f, 13.0f, (float) juce::jmin (W, H) * 0.05f);
        const int   stripH = juce::jlimit (14, 20, (int) (fs * 1.6f));
        const int   imgH   = H - stripH;

        if (imageDirty
            || cache.isNull()
            || cache.getWidth()  != W
            || cache.getHeight() != imgH)
        {
            rebuildImage (W, imgH);
            imageDirty = false;
        }

        g.drawImageAt (cache, b.getX(), b.getY());

        // ── Live brightness pulse (cheap – no pixel rebuild) ────────────────
        // Additive brighten over the pattern proportional to instantaneous RMS.
        if (liveBrightness > 0.01f)
        {
            g.setColour (sandColour.withAlpha (liveBrightness * 0.40f));
            g.fillRect (juce::Rectangle<int> (b.getX(), b.getY(), b.getWidth(), imgH));
        }

        drawOverlay (g, b, stripH, imgH, fs);
    }

private:
    // ── Physics ──────────────────────────────────────────────────────────────

    // Default: aluminium 30 cm × 1 mm
    float E   { 70.0e9f };
    float rho { 2700.0f };
    float nu  { 0.33f   };
    float h   { 0.001f  };
    float L   { 0.30f   };

    void applyMaterial (int idx) noexcept
    {
        switch (idx)
        {
            case 0: E = 70.0e9f;  rho = 2700.0f; nu = 0.33f; break;  // Aluminium
            case 1: E = 200.0e9f; rho = 7800.0f; nu = 0.28f; break;  // Steel
            case 2: E = 70.0e9f;  rho = 2500.0f; nu = 0.22f; break;  // Glass
            case 3: E = 3.2e9f;   rho = 1180.0f; nu = 0.37f; break;  // Acrylic
            default: break;
        }
    }

    float computeD() const noexcept
    {
        return (E * h * h * h) / (12.0f * (1.0f - nu * nu));
    }

    float computeK() const noexcept
    {
        const float D = computeD();
        const float c = std::sqrt (D / (rho * h));
        return (juce::MathConstants<float>::pi / (2.0f * L * L)) * c;
    }

    float computeFreq (int mi, int ni) const noexcept
    {
        return computeK() * ((float) (mi * mi) / (ar * ar) + (float) (ni * ni));
    }

    // ── Chladni function (free-edge, cos-cos antisymmetric) ──────────────────
    //  x, y ∈ [0,1] normalised plate coords.  r = aspect ratio.
    inline float chladni (float x, float y) const noexcept
    {
        constexpr float pi = juce::MathConstants<float>::pi;
        const float fm = (float) m;
        const float fn = (float) n;
        return   std::cos (fn * pi * x) * std::cos (fm * pi * y / ar)
               - std::cos (fm * pi * x) * std::cos (fn * pi * y / ar);
    }

    // ── Image renderer ────────────────────────────────────────────────────────
    void rebuildImage (int W, int H)
    {
        cache = juce::Image (juce::Image::ARGB, W, H, false);
        juce::Image::BitmapData bd (cache, juce::Image::BitmapData::writeOnly);

        const float sharp = userSharpness * animFactor;
        const float sR    = sandColour.getFloatRed();
        const float sG    = sandColour.getFloatGreen();
        const float sB    = sandColour.getFloatBlue();

        for (int py = 0; py < H; ++py)
        {
            const float y = (float) py / (float) juce::jmax (H - 1, 1);
            for (int px = 0; px < W; ++px)
            {
                const float x    = (float) px / (float) juce::jmax (W - 1, 1);
                const float Z    = chladni (x, y);
                // Gaussian: sand piles on nodal lines (Z ≈ 0)
                const float sand = std::exp (-Z * Z * sharp);
                // Very dark background; brighten only at nodal lines
                constexpr float kBg = 0.03f;
                const float v = kBg + (1.0f - kBg) * sand;

                bd.setPixelColour (px, py,
                    juce::Colour::fromFloatRGBA (v * sR, v * sG, v * sB, 1.0f));
            }
        }
    }

    // ── Overlay (strip + warnings + glow) ────────────────────────────────────
    void drawOverlay (juce::Graphics& g, juce::Rectangle<int> b,
                      int stripH, int imgH, float fs)
    {
        // ── Bottom info strip ───────────────────────────────────────────────
        auto strip = b.withY (b.getY() + imgH).withHeight (stripH);
        g.setColour (juce::Colour (0xFF111111));
        g.fillRect (strip);
        g.setColour (juce::Colour (0xFF333333));
        g.drawLine ((float) strip.getX(), (float) strip.getY(),
                    (float) strip.getRight(), (float) strip.getY(), 1.0f);

        g.setFont (juce::Font (juce::FontOptions (fs).withStyle ("Bold")));
        g.setColour (juce::Colours::white);

        char buf[80];
        const float fTheo = getTheoreticalFreq();
        if (fTheo < 1000.0f)
            std::snprintf (buf, sizeof (buf), "(%d,%d)  %.1f Hz  AR:%.2f", m, n, fTheo, ar);
        else
            std::snprintf (buf, sizeof (buf), "(%d,%d)  %.2f kHz  AR:%.2f",
                           m, n, fTheo / 1000.0f, ar);
        g.drawText (juce::String (buf), strip.reduced (4, 0),
                    juce::Justification::centred, false);

        // ── Degenerate warning ──────────────────────────────────────────────
        if (m == n)
        {
            const auto imgArea = b.withHeight (imgH);
            g.setColour (juce::Colours::black.withAlpha (0.65f));
            g.fillRect (imgArea);
            g.setFont (juce::Font (juce::FontOptions (fs)));
            g.setColour (juce::Colours::orange);
            g.drawText ("m = n : blank pattern", imgArea,
                        juce::Justification::centred, false);
        }

        // ── RMS activity border glow ────────────────────────────────────────
        const float rmsDb    = linearToDb (audioSource.getLastRms());
        const float activity = juce::jmap (juce::jlimit (-60.0f, 0.0f, rmsDb),
                                            -60.0f, 0.0f, 0.0f, 1.0f);
        if (activity > 0.01f)
        {
            g.setColour (sandColour.withAlpha (activity * 0.55f));
            g.drawRect (b.withHeight (imgH).toFloat().reduced (1.5f), 2.0f);
        }
    }

    // ── Timer: settle animation + live RMS pulse ─────────────────────────────
    //  animFactor: 0 = "sand just shaken" (blurry), 1 = fully settled (sharp).
    //  liveBrightness: pulses with instantaneous RMS for a "vibrating" look.
    void timerCallback() override
    {
        const float rms    = audioSource.getLastRms();
        const float rmsDb  = linearToDb (rms);

        // animFactor target: louder = sharper nodal lines
        const float target = juce::jmap (juce::jlimit (-60.0f, 0.0f, rmsDb),
                                          -60.0f, 0.0f, 0.15f, 1.0f);

        // Faster convergence (0.75 = ~0.5 s settle at 30 Hz)
        const float prevAnim = animFactor;
        animFactor = animFactor * 0.75f + target * 0.25f;

        // Live brightness: punchy per-frame pulse (no image rebuild needed)
        const float prevBright = liveBrightness;
        liveBrightness = liveBrightness * 0.55f + rms * 1.4f * 0.45f;
        liveBrightness = juce::jlimit (0.0f, 1.0f, liveBrightness);

        // Rebuild pixel cache if sharpness changed meaningfully
        if (std::abs (animFactor - prevAnim) > 0.002f)
            imageDirty = true;

        // Always repaint – liveBrightness overlay + RMS glow needs every frame
        if (imageDirty || std::abs (liveBrightness - prevBright) > 0.005f)
            repaint();
    }

    void triggerSettle() noexcept
    {
        animFactor    = 0.05f;   // "shake" – sand scatters and re-settles
        liveBrightness = 0.8f;   // brief flash on mode change
        markDirty();
    }

    void markDirty() noexcept { imageDirty = true; repaint(); }

    static float linearToDb (float v) noexcept
    {
        return (v > 1e-5f) ? 20.0f * std::log10 (v) : -60.0f;
    }

    // ── Members ───────────────────────────────────────────────────────────────
    IAudioSource& audioSource;

    int   m { 2 };
    int   n { 3 };
    float ar { 1.0f };

    float userSharpness { 20.0f };
    float animFactor    { 1.0f  };
    float liveBrightness { 0.0f }; // per-frame RMS pulse, no cache rebuild

    juce::Colour sandColour { juce::Colour (0xFFE8D5A0) };   // warm cream

    bool        imageDirty { true };
    juce::Image cache;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChladniPatternMeter)
};
