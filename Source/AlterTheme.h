/*
  ==============================================================================

    AlterTheme.h
    ALTER visual identity: "living architecture"
    fluid / organic / architectural / psychoacoustic / futuristic

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <atomic>

namespace AlterTheme
{
    // ── Core spectral palette (fixed – used by heat maps & module accents) ───
    const juce::Colour congressBlue   { 0xFF043E8F };
    const juce::Colour electricViolet { 0xFF6902D6 };
    const juce::Colour pictonBlue     { 0xFF3D96E7 };
    const juce::Colour cerise         { 0xFFE600B8 };
    const juce::Colour caribbeanGreen { 0xFF13D192 };

    // ── Extended tints (fixed) ───────────────────────────────────────────────
    const juce::Colour iceLavender    { 0xFFBCBBFB };
    const juce::Colour softViolet     { 0xFFC28AFD };
    const juce::Colour iceBlue        { 0xFF96C6F2 };
    const juce::Colour pinkGlow       { 0xFFFF89E7 };
    const juce::Colour mintGlow       { 0xFF52EFBB };

    // ── THEME PALETTE (mutable – set by setTheme, used by ALL UI chrome) ─────
    //  Defaults = Cyber.
    inline juce::Colour bgVoid     { 0xFF03040C };  // deepest background
    inline juce::Colour bgDeep     { 0xFF060A18 };  // window background
    inline juce::Colour bgPanel    { 0xFF0A1126 };  // panels / combo boxes
    inline juce::Colour bgRaised   { 0xFF101A38 };  // buttons / raised elements
    inline juce::Colour navyEdge   { 0xFF022D68 };  // strokes / separators
    inline juce::Colour textBright { 0xFFEAF3FF };
    inline juce::Colour textNormal { 0xFF96C6F2 };
    inline juce::Colour textDim    { 0xFF5F7AA8 };
    inline juce::Colour accent     { 0xFF3D96E7 };  // primary theme accent
    inline juce::Colour accent2    { 0xFF6902D6 };  // secondary theme accent

    // ── HUD block shades ─────────────────────────────────────────────────────
    //  White / grey / black tones for block 1/2/3, softened so each stays visible
    //  on BOTH white and black backgrounds (never pure #FFF / #000).
    inline juce::Colour blockShade (int idx)
    {
        switch (idx)
        {
            case 0:  return juce::Colour (0xFFCFCFCF);   // "white"  (soft silver)
            case 1:  return juce::Colour (0xFF8A8A8A);   // grey
            default: return juce::Colour (0xFF474747);   // "black"  (charcoal)
        }
    }
    inline const char* blockShadeName (int idx)
    {
        switch (idx) { case 0: return "White"; case 1: return "Grey"; default: return "Black"; }
    }

    // Default per-module accent palette (cycled by panel id)
    inline juce::Colour accentForIndex (int i)
    {
        const juce::Colour palette[] = { pictonBlue, caribbeanGreen, cerise,
                                         electricViolet, iceBlue };
        return palette[((i % 5) + 5) % 5];
    }

    /** Heat colormap for spectral intensity 0..1 (void → blue → violet → cerise → white).
        Non-static stops: bgVoid/navyEdge are theme-mutable. */
    inline juce::Colour heatColour (float v01)
    {
        struct Stop { float pos; juce::Colour c; };
        const Stop stops[] = {
            { 0.00f, bgVoid },
            { 0.18f, navyEdge },
            { 0.38f, congressBlue },
            { 0.58f, electricViolet },
            { 0.78f, cerise },
            { 0.92f, pinkGlow },
            { 1.00f, juce::Colour (0xFFF4FFFB) }
        };

        constexpr int numStops = (int) (sizeof (stops) / sizeof (stops[0]));
        const float v = juce::jlimit (0.0f, 1.0f, v01);
        for (int i = 1; i < numStops; ++i)
            if (v <= stops[i].pos)
                return stops[i-1].c.interpolatedWith (stops[i].c,
                        (v - stops[i-1].pos) / (stops[i].pos - stops[i-1].pos));
        return stops[numStops - 1].c;
    }

    // ── HUD freeze ("Hold") ─────────────────────────────────────────────────────
    //  When true, every visual module stops generating new frames and holds its
    //  last rendered image. Set from the controller's "Hold" toggle; read by the
    //  module render loops/timers (process-wide).
    inline std::atomic<bool> hudFrozen { false };

    // When true, the per-module info/parameter overlay (top-right of each module) is
    // hidden. Toggled from the controller's "Hide info" checkbox.
    inline std::atomic<bool> hudInfoHidden { false };

    // ── Themes ────────────────────────────────────────────────────────────────
    //  0 = Cyber  (neon blue/violet – the original ALTER look)
    //  1 = Dark   (grey-black, occasional white lines)
    //  2 = Custom (palette auto-derived from two user-picked colours)
    //  3 = White  (light mode, black accents)
    inline int themeIndex = 0;

    // Custom-theme source colours (persisted in AlterState; set before setTheme(2))
    inline juce::Colour customPrimary   { 0xFF3D96E7 };
    inline juce::Colour customSecondary { 0xFF6902D6 };

    /** Bumped by EVERY palette change, including one that leaves themeIndex alone.

        Caches that bake the palette in — a pre-rendered background, a colour LUT —
        used to test `themeIndex` to decide whether they were stale. That silently
        missed the Custom theme: editing its colour pair repaints the whole app but
        keeps the index at 2, so those caches never rebuilt and the HUD kept the old
        background until an unrelated resize happened to invalidate them. Compare
        against this instead and no future palette change can be missed either. */
    inline unsigned themeGeneration = 0;

    inline void setTheme (int i)
    {
        ++themeGeneration;
        themeIndex = juce::jlimit (0, 3, i);
        switch (themeIndex)
        {
            case 1: // DARK – grey/black, white accents
                bgVoid     = juce::Colour (0xFF000000);
                bgDeep     = juce::Colour (0xFF0B0B0B);
                bgPanel    = juce::Colour (0xFF161616);
                bgRaised   = juce::Colour (0xFF222222);
                navyEdge   = juce::Colour (0xFF333333);
                textBright = juce::Colour (0xFFFFFFFF);
                textNormal = juce::Colour (0xFFC9C9C9);
                textDim    = juce::Colour (0xFF6E6E6E);
                accent     = juce::Colour (0xFFEDEDED);   // white lines
                accent2    = juce::Colour (0xFF8C8C8C);
                break;

            case 2: // CUSTOM – primary drives the ACCENT/foreground, secondary drives the BACKGROUND
            {
                // Background family is taken straight from the user's secondary colour, so
                // whatever they pick actually shows up as the HUD + controller background
                // (previously it was forced almost black regardless of the choice).
                const juce::Colour bg = customSecondary;
                const bool darkBg = bg.getPerceivedBrightness() < 0.5f;

                bgVoid   = bg.darker   (0.45f);   // gradient bottom / GL clear
                bgDeep   = bg.darker   (0.12f);   // gradient top / module base
                bgPanel  = darkBg ? bg.brighter (0.14f) : bg.darker (0.06f);
                bgRaised = darkBg ? bg.brighter (0.26f) : bg.darker (0.12f);
                navyEdge = darkBg ? bg.brighter (0.42f) : bg.darker (0.22f);

                // Foreground TAKES THE ACCENT (primary) HUE so changing the accent visibly
                // recolours all text + highlights — brightness is chosen for legibility
                // against the chosen background. (Before, text was fixed white/slate, so the
                // accent only tinted a few thin lines and appeared to "do nothing".)
                const float ah = customPrimary.getHue();
                const float as = juce::jlimit (0.0f, 1.0f, customPrimary.getSaturation());
                textBright = darkBg ? juce::Colour (ah, as * 0.35f, 0.98f, 1.0f)
                                    : juce::Colour (ah, as * 0.80f, 0.12f, 1.0f);
                textNormal = darkBg ? juce::Colour (ah, as * 0.55f, 0.86f, 1.0f)
                                    : juce::Colour (ah, as * 0.85f, 0.26f, 1.0f);
                textDim    = darkBg ? juce::Colour (ah, as * 0.45f, 0.56f, 1.0f)
                                    : juce::Colour (ah, as * 0.60f, 0.50f, 1.0f);

                accent     = customPrimary;                    // accent lines / ticks / slider fills
                accent2    = customPrimary.contrasting (0.25f);
                break;
            }

            case 3: // WHITE – light mode
                bgVoid     = juce::Colour (0xFFFFFFFF);
                bgDeep     = juce::Colour (0xFFF2F5F8);
                bgPanel    = juce::Colour (0xFFE9EEF3);
                bgRaised   = juce::Colour (0xFFDCE4EC);
                navyEdge   = juce::Colour (0xFFB9C6D2);
                textBright = juce::Colour (0xFF0B0F14);
                textNormal = juce::Colour (0xFF2A3440);
                textDim    = juce::Colour (0xFF8895A3);
                accent     = juce::Colour (0xFF14181D);   // black accents (was blue)
                accent2    = juce::Colour (0xFF3A4048);   // dark charcoal
                break;

            default: // CYBER – original neon
                bgVoid     = juce::Colour (0xFF03040C);
                bgDeep     = juce::Colour (0xFF060A18);
                bgPanel    = juce::Colour (0xFF0A1126);
                bgRaised   = juce::Colour (0xFF101A38);
                navyEdge   = juce::Colour (0xFF022D68);
                textBright = juce::Colour (0xFFEAF3FF);
                textNormal = juce::Colour (0xFF96C6F2);
                textDim    = juce::Colour (0xFF5F7AA8);
                accent     = pictonBlue;
                accent2    = electricViolet;
                break;
        }
    }

    /** Per-theme accent used for HUD chrome (controller button, edges). */
    inline juce::Colour themeAccent() { return accent; }

    /** Vertical "living architecture" background gradient (theme palette). */
    /** The background gradient's colour at a normalised vertical position (0 = top
        of the module, 1 = bottom).

        Kept next to paintBackground because the two MUST agree: Fusion composites
        its layers per pixel and has to know, for any given row, exactly what
        background the module underneath it painted — both to subtract a layer's
        own background out of the way and to put the same one back underneath the
        fused result. If one of these is ever changed, change the other. */
    inline juce::Colour backgroundAt (float t) noexcept
    {
        t = juce::jlimit (0.0f, 1.0f, t);

        // Matches the gradient below: bgDeep at 0, the midpoint colour at 0.55,
        // bgVoid at 1, linear in between.
        const juce::Colour mid = bgDeep.interpolatedWith (bgVoid, 0.5f);

        return (t <= 0.55f) ? bgDeep.interpolatedWith (mid, t / 0.55f)
                            : mid.interpolatedWith (bgVoid, (t - 0.55f) / 0.45f);
    }

    inline void paintBackground (juce::Graphics& g, juce::Rectangle<float> area)
    {
        juce::ColourGradient grad (bgDeep, area.getX(), area.getY(),
                                   bgVoid, area.getX(), area.getBottom(), false);
        grad.addColour (0.55, bgDeep.interpolatedWith (bgVoid, 0.5f));
        g.setGradientFill (grad);
        g.fillRect (area);
    }
}

/** A module that can draw its content on TRANSPARENCY instead of the theme
    gradient.

    Every visualiser normally fills its whole rectangle: the gradient first, the
    drawing on top. That is right in the HUD and wrong inside a Fusion, where
    the module is one layer of a stack and the background would simply hide
    whatever is underneath it.

    Fusion used to work around this by SUBTRACTING the known gradient back out of
    the finished frame and treating the remainder as coverage. That is an inverse
    operation on 8-bit data and it can only ever be approximate: it left a grey
    veil where the module was dark, and it ate faint tails and glows wherever they
    happened to sit near background level. The threshold that traded one of those
    off against the other was pure guesswork.

    This is the honest version. A module that is lent to a Fusion is told to skip
    its background, so what lands in the alpha channel is real coverage that the
    module itself decided, and the fusion shader does not have to guess anything.

    Default is OFF, so a module that has not been told otherwise behaves exactly as
    it always did. */
class ThemedBackground
{
public:
    virtual ~ThemedBackground() = default;

    /** MESSAGE THREAD. Read by the render worker, hence atomic. */
    void setTransparentBackground (bool shouldBeTransparent) noexcept
    {
        transparentBg.store (shouldBeTransparent, std::memory_order_relaxed);
    }

    bool isTransparentBackground() const noexcept
    {
        return transparentBg.load (std::memory_order_relaxed);
    }

    /** Does setTransparentBackground actually reach the pixels this module hands
        out?

        For the 2D modules, yes: they render themselves and the flag simply skips
        the gradient fill. The GL modules are different — while they are on the GPU
        path their background is painted by a shader inside AlterGLHost, which this
        flag does not reach, so they keep handing out an opaque frame and Fusion
        must fall back to subtracting the known gradient for them.

        Saying so explicitly is what lets the fusion shader use real coverage where
        it exists and the old approximation only where it must, instead of applying
        one compromise to everything. */
    virtual bool transparencyReachesPixels() const { return true; }

protected:
    /** Drop-in replacement for AlterTheme::paintBackground inside a module's own
        rendering. Paints the theme gradient normally, and nothing at all when the
        module is being used as a Fusion layer. */
    void paintModuleBackground (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        if (isTransparentBackground())
            return;

        AlterTheme::paintBackground (g, area);
    }

private:
    std::atomic<bool> transparentBg { false };
};

namespace AlterTheme
{

    /** Soft outer glow stroke around a rounded rect. */
    inline void glowRect (juce::Graphics& g, juce::Rectangle<float> r,
                          juce::Colour c, float intensity01, float corner = 6.0f)
    {
        const float a = juce::jlimit (0.0f, 1.0f, intensity01);
        if (a < 0.02f) return;
        for (int i = 3; i >= 1; --i)
            g.setColour (c.withAlpha (a * 0.10f * (float)(4 - i))),
            g.drawRoundedRectangle (r.expanded ((float) i), corner + (float) i, 1.5f);
        g.setColour (c.withAlpha (a * 0.75f));
        g.drawRoundedRectangle (r, corner, 1.2f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// AlterLookAndFeel – app-wide organic/futuristic styling
// ─────────────────────────────────────────────────────────────────────────────
class AlterLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AlterLookAndFeel() { refreshFromTheme(); }

    /** Re-applies all colours from the current AlterTheme palette.
        Call after AlterTheme::setTheme(), then sendLookAndFeelChange()
        on the top-level windows. */
    void refreshFromTheme()
    {
        using namespace AlterTheme;

        setColour (juce::ResizableWindow::backgroundColourId, bgDeep);
        setColour (juce::DocumentWindow::textColourId,        textNormal);

        setColour (juce::Label::textColourId,                 textNormal);

        setColour (juce::TextButton::buttonColourId,          bgRaised);
        setColour (juce::TextButton::buttonOnColourId,        accent.withAlpha (0.35f));
        setColour (juce::TextButton::textColourOffId,         textNormal);
        setColour (juce::TextButton::textColourOnId,          textBright);

        setColour (juce::ComboBox::backgroundColourId,        bgPanel);
        setColour (juce::ComboBox::textColourId,              textNormal);
        setColour (juce::ComboBox::outlineColourId,           navyEdge.brighter (0.4f));
        setColour (juce::ComboBox::arrowColourId,             accent);

        setColour (juce::PopupMenu::backgroundColourId,           bgPanel);
        setColour (juce::PopupMenu::textColourId,                 textNormal);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent2.withAlpha (0.55f));
        setColour (juce::PopupMenu::highlightedTextColourId,       textBright);

        setColour (juce::Slider::backgroundColourId,          navyEdge);
        setColour (juce::Slider::trackColourId,               accent.withAlpha (0.6f));
        setColour (juce::Slider::thumbColourId,               accent);
        setColour (juce::Slider::rotarySliderFillColourId,    accent2);
        setColour (juce::Slider::rotarySliderOutlineColourId, navyEdge);
        setColour (juce::Slider::textBoxTextColourId,         textNormal);
        setColour (juce::Slider::textBoxOutlineColourId,      navyEdge.withAlpha (0.6f));
        setColour (juce::Slider::textBoxBackgroundColourId,   bgPanel);

        setColour (juce::ToggleButton::textColourId,          textNormal);
        setColour (juce::ToggleButton::tickColourId,          accent);
        setColour (juce::ToggleButton::tickDisabledColourId,  textDim);

        setColour (juce::ListBox::backgroundColourId,         juce::Colours::transparentBlack);
        setColour (juce::ListBox::outlineColourId,            navyEdge);

        setColour (juce::ScrollBar::thumbColourId,            accent.withAlpha (0.5f));

        setColour (juce::AlertWindow::backgroundColourId,     bgPanel);
        setColour (juce::AlertWindow::textColourId,           textNormal);

        setColour (juce::TooltipWindow::backgroundColourId,   bgPanel);
        setColour (juce::TooltipWindow::textColourId,         textNormal);
        setColour (juce::TooltipWindow::outlineColourId,      accent.withAlpha (0.4f));

        setColour (juce::TextEditor::backgroundColourId,      bgPanel);
        setColour (juce::TextEditor::textColourId,            textBright);
        setColour (juce::TextEditor::outlineColourId,         navyEdge);
    }

    // ── Typography ───────────────────────────────────────────────────────────
    // One knob for every widget the look-and-feel draws: a couple of px above
    // JUCE's defaults and bold, because the small dim captions were hard to read.
    // This deliberately does NOT reach the info window or the HUD modules — those
    // paint their text with their own g.setFont() calls and keep their own sizes.
    static constexpr float kUiFontBump = 0.5f;   // bold does most of the work; more than this and buttons ellipsise

    static juce::Font uiFont (float height)
    {
        return juce::Font (juce::FontOptions (height + kUiFontBump).withStyle ("Bold"));
    }

    juce::Font getLabelFont (juce::Label& l) override
    {
        return uiFont (l.getFont().getHeight());
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return uiFont (juce::jmin (16.0f, (float) buttonHeight * 0.6f));
    }

    juce::Font getComboBoxFont (juce::ComboBox& box) override
    {
        return uiFont (juce::jmin (16.0f, (float) box.getHeight() * 0.85f));
    }

    juce::Font getPopupMenuFont() override
    {
        return uiFont (16.0f);   // JUCE's default is 17 — same ladder, bolder
    }

    // ── Buttons: rounded, liquid gradient, glow on hover ─────────────────────
    void drawButtonBackground (juce::Graphics& g, juce::Button& b,
                               const juce::Colour& backgroundColour,
                               bool over, bool down) override
    {
        using namespace AlterTheme;
        auto r = b.getLocalBounds().toFloat().reduced (1.0f);
        const float corner = juce::jmin (8.0f, r.getHeight() * 0.3f);

        auto top    = backgroundColour.brighter (down ? 0.00f : over ? 0.25f : 0.12f);
        auto bottom = backgroundColour.darker   (down ? 0.10f : 0.35f);

        g.setGradientFill ({ top, r.getX(), r.getY(), bottom, r.getX(), r.getBottom(), false });
        g.fillRoundedRectangle (r, corner);

        // thin inner top highlight — "lit edge" tech look
        g.setColour (top.brighter (0.8f).withAlpha (0.22f));
        g.fillRoundedRectangle (r.withHeight (2.0f).reduced (corner * 0.6f, 0.0f), 1.0f);

        // side tech ticks (subtle port markers, brighten on hover)
        g.setColour (accent.withAlpha (over || down ? 0.85f : 0.35f));
        g.fillRect (r.getX() + 3.0f,      r.getCentreY() - 3.0f, 1.5f, 6.0f);
        g.fillRect (r.getRight() - 4.5f,  r.getCentreY() - 3.0f, 1.5f, 6.0f);

        g.setColour ((over || down ? accent : navyEdge.brighter (0.5f))
                         .withAlpha (over ? 0.85f : 0.55f));
        g.drawRoundedRectangle (r, corner, 1.0f);

        if (over || down)
            glowRect (g, r, b.getToggleState() ? accent2 : accent,
                      down ? 0.8f : 0.45f, corner);
    }

    // ── ComboBox: rounded panel + chevron ────────────────────────────────────
    void drawComboBox (juce::Graphics& g, int width, int height, bool isDown,
                       int, int, int, int, juce::ComboBox& box) override
    {
        using namespace AlterTheme;
        auto r = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
        const float corner = 6.0f;

        const auto bgc = findColour (juce::ComboBox::backgroundColourId);
        g.setGradientFill ({ bgc.brighter (0.10f), r.getX(), r.getY(),
                             bgc.darker (0.25f),   r.getX(), r.getBottom(), false });
        g.fillRoundedRectangle (r, corner);

        // hairline divider separating the chevron "port"
        g.setColour (navyEdge.brighter (0.5f).withAlpha (0.55f));
        g.fillRect ((float) width - 24.0f, r.getY() + 4.0f, 1.0f, r.getHeight() - 8.0f);

        g.setColour ((box.hasKeyboardFocus (true) || isDown ? accent : navyEdge.brighter (0.5f))
                         .withAlpha (0.8f));
        g.drawRoundedRectangle (r, corner, 1.0f);

        juce::Path chevron;
        const float cx = (float) width - 13.0f, cy = (float) height * 0.5f;
        chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
        chevron.lineTo (cx, cy + 2.5f);
        chevron.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (findColour (juce::ComboBox::arrowColourId));
        g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }

    // ── ToggleButton: organic pill switch ────────────────────────────────────
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b,
                           bool over, bool down) override
    {
        using namespace AlterTheme;
        juce::ignoreUnused (down);

        const float h = juce::jmin (18.0f, (float) b.getHeight() - 4.0f);
        const float w = h * 1.9f;
        auto track = juce::Rectangle<float> (2.0f, ((float) b.getHeight() - h) * 0.5f, w, h);
        const bool on = b.getToggleState();

        if (on)
            g.setGradientFill ({ accent.darker (0.5f), track.getX(), track.getY(),
                                 accent, track.getRight(), track.getBottom(), false });
        else
            g.setColour (bgRaised);
        g.fillRoundedRectangle (track, h * 0.5f);

        g.setColour ((on ? accent.brighter (0.4f) : navyEdge.brighter (0.6f))
                         .withAlpha (over ? 0.95f : 0.6f));
        g.drawRoundedRectangle (track, h * 0.5f, 1.0f);

        const float knobR = h - 5.0f;
        const float kx = on ? track.getRight() - knobR - 2.5f : track.getX() + 2.5f;

        if (on)   // energised: soft halo + accent ring around the knob
        {
            g.setColour (accent.withAlpha (0.20f));
            g.fillEllipse (kx - 3.5f, track.getY() - 1.0f, knobR + 7.0f, knobR + 7.0f);
        }
        g.setColour (on ? textBright : textDim.brighter (0.3f));
        g.fillEllipse (kx, track.getY() + 2.5f, knobR, knobR);
        if (on)
        {
            g.setColour (accent.brighter (0.4f).withAlpha (0.9f));
            g.drawEllipse (kx - 1.0f, track.getY() + 1.5f, knobR + 2.0f, knobR + 2.0f, 1.1f);
        }

        g.setColour (b.findColour (juce::ToggleButton::textColourId)
                       .withAlpha (b.isEnabled() ? 1.0f : 0.4f));
        g.setFont (uiFont (13.0f));
        g.drawFittedText (b.getButtonText(),
                          b.getLocalBounds().withTrimmedLeft ((int) w + 8),
                          juce::Justification::centredLeft, 2);
    }

    // ── Linear sliders: capsule track with flowing gradient ──────────────────
    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minPos, float maxPos,
                           juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        using namespace AlterTheme;

        if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearVertical)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height,
                                              sliderPos, minPos, maxPos, style, s);
            return;
        }

        const bool horizontal = (style == juce::Slider::LinearHorizontal);
        const float trackT = 5.0f;

        juce::Rectangle<float> track = horizontal
            ? juce::Rectangle<float> ((float) x, (float) y + (float) height * 0.5f - trackT * 0.5f,
                                      (float) width, trackT)
            : juce::Rectangle<float> ((float) x + (float) width * 0.5f - trackT * 0.5f, (float) y,
                                      trackT, (float) height);

        g.setColour (bgRaised);
        g.fillRoundedRectangle (track, trackT * 0.5f);

        auto filled = track;
        if (horizontal) filled = filled.withWidth (juce::jmax (trackT, sliderPos - (float) x));
        else            filled = filled.withTop (sliderPos);

        g.setGradientFill ({ accent.darker (0.55f), track.getX(), track.getY(),
                             accent,
                             horizontal ? track.getRight() : track.getX(),
                             horizontal ? track.getY() : track.getBottom(), false });
        g.fillRoundedRectangle (filled, trackT * 0.5f);

        // neon core: a thin bright line inside the filled capsule (energy conduit)
        if (horizontal && filled.getWidth() > 8.0f)
        {
            auto core = filled.reduced (3.0f, 1.8f);
            g.setColour (textBright.withAlpha (0.30f));
            g.fillRoundedRectangle (core, core.getHeight() * 0.5f);
        }

        const float r = 7.0f;
        const float cx = horizontal ? sliderPos : track.getCentreX();
        const float cy = horizontal ? track.getCentreY() : sliderPos;

        g.setColour (accent.withAlpha (0.30f));
        g.fillEllipse (cx - r - 3, cy - r - 3, (r + 3) * 2, (r + 3) * 2);
        g.setGradientFill ({ textBright, cx, cy - r, accent, cx, cy + r, false });
        g.fillEllipse (cx - r, cy - r, r * 2, r * 2);
        g.setColour (accent.brighter (0.35f).withAlpha (0.9f));   // crisp tech ring
        g.drawEllipse (cx - r, cy - r, r * 2, r * 2, 1.1f);
    }

    // ── Rotary: violet→cerise energy arc ─────────────────────────────────────
    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float startAngle, float endAngle,
                           juce::Slider&) override
    {
        using namespace AlterTheme;
        const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const float cx = bounds.getCentreX(), cy = bounds.getCentreY();
        const float angle  = startAngle + sliderPos * (endAngle - startAngle);
        const float trackW = juce::jmax (2.0f, radius * 0.16f);
        const float arcR   = radius - trackW * 0.5f;

        juce::Path bg;
        bg.addCentredArc (cx, cy, arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (bgRaised);
        g.strokePath (bg, juce::PathStrokeType (trackW, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));

        if (sliderPos > 0.001f)
        {
            juce::Path arc;
            arc.addCentredArc (cx, cy, arcR, arcR, 0.0f, startAngle, angle, true);
            g.setGradientFill ({ accent2, cx - radius, cy,
                                 accent, cx + radius, cy, false });
            g.strokePath (arc, juce::PathStrokeType (trackW, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        }

        const float knobR = (radius - trackW) * 0.72f;
        g.setGradientFill ({ bgRaised.brighter (0.4f), cx, cy - knobR,
                             bgPanel, cx, cy + knobR, false });
        g.fillEllipse (cx - knobR, cy - knobR, knobR * 2, knobR * 2);
        g.setColour (accent.withAlpha (0.5f));
        g.drawEllipse (cx - knobR, cy - knobR, knobR * 2, knobR * 2, 1.0f);

        juce::Path pointer;
        const float pW = juce::jmax (1.5f, radius * 0.09f);
        pointer.addRoundedRectangle (-pW * 0.5f, -knobR * 0.85f, pW, knobR * 0.55f, pW * 0.5f);
        pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (cx, cy));
        g.setColour (textBright);
        g.fillPath (pointer);
    }

    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
        using namespace AlterTheme;
        auto r = juce::Rectangle<float> (0, 0, (float) width, (float) height);
        g.setGradientFill ({ bgPanel, 0, 0, bgDeep, 0, (float) height, false });
        g.fillAll();
        g.setColour (accent2.withAlpha (0.35f));
        g.drawRect (r, 1.0f);
    }
};
