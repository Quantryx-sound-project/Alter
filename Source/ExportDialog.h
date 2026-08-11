/*
  ==============================================================================

    ExportDialog.h
    The "Export settings" dialog shown after the user has picked a file name for
    a HUD recording: audio source, what to capture, output size.

    A real component rather than a juce::AlertWindow, because the contents have
    to change as the user chooses: the width/height fields only make sense for a
    custom size, the quality picker only for the presets, and the resulting pixel
    size is shown live so nobody has to guess what they are about to get.

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <vector>
#include <functional>
#include "UdpReceiver.h"    // PluginInstanceInfo
#include "HudRecorder.h"

// ── what the dialog collects ───────────────────────────────────────────────────
struct AlterExportSettings
{
    /** HOW MANY PIXELS — never what shape. The shape of an export is the shape of
        the HUD window, chosen with the Shape buttons in the controller while you
        compose. By the time this dialog opens, the framing decision is already
        made and visible on screen, so all that is left is a quality tier. That is
        also why nothing here can produce black bars any more: every mode below
        keeps the source's own proportions. */
    enum SizeMode { DisplaySize = 0, FullHD, HD, Custom };

    int  sizeMode = FullHD;
    int  customW  = 1080, customH = 1920;
    /** WHAT is filmed. Not how big, not what shape — those are separate questions
        and keeping them separate is what stopped this dialog from growing modes. */
    enum RecordMode { HudWindow = 0, WholeScreen, EachModule };
    int  recordMode = HudWindow;

    bool isWholeScreen()  const noexcept { return recordMode == WholeScreen; }
    bool isEachModule()   const noexcept { return recordMode == EachModule; }
    juce::uint32 audioInstanceId = 0;    // 0 = system audio, else plugin instance

    /** Record the modules on a transparent background into a lossless .mov with a
        real alpha channel, instead of filming the screen into an .mp4. Off by
        default: it is a different pipeline with different trade-offs (much larger
        files) and should only happen when it is asked for. */
    bool transparent = false;

    /** The recorder's target, given the source it will be filming.

        Returns a WIDTH-DRIVEN target (h = 0) for the tiers: the recorder then
        makes the frame exactly that wide and takes the height from the source's
        own proportions, so a 9:16 HUD exports 1080x1920 and a 16:9 one exports
        1920x1080 from the very same "Full HD" setting. The tier is applied to the
        LONG side, which is what "Full HD" means to everyone regardless of whether
        the video stands up or lies down. */
    void targetFor (int srcW, int srcH, int& w, int& h) const noexcept
    {
        const bool portrait = srcH > srcW;
        switch (sizeMode)
        {
            case FullHD: w = portrait ? 1080 : 1920; h = 0; break;
            case HD:     w = portrait ?  720 : 1280; h = 0; break;
            case Custom: w = customW;               h = customH; break;
            default:     w = 0;                     h = 0;       break;  // display size: 1:1
        }
    }

    int targetWidth  (int srcW, int srcH) const noexcept { int w = 0, h = 0; targetFor (srcW, srcH, w, h); return w; }
    int targetHeight (int srcW, int srcH) const noexcept { int w = 0, h = 0; targetFor (srcW, srcH, w, h); return h; }

    /** The exact frame the recorder will produce for a source of srcW x srcH. */
    void resolve (int srcW, int srcH, int& outW, int& outH) const noexcept
    {
        if (srcW < 1 || srcH < 1) { outW = outH = 0; return; }

        int w = 0, h = 0;
        targetFor (srcW, srcH, w, h);

        if (w < 2)                          // original: 1:1, up to the capture ceiling
        {
            // The recorder caps the long side (HudRecorder::maxCaptureDim). Applying
            // the same cap here is what keeps this dialog's promise equal to the
            // file: it used to offer 3840x2160 for a 4K source and quietly deliver
            // 2560x1440.
            const int longSide = juce::jmax (srcW, srcH);
            const double s = longSide > HudRecorder::maxCaptureDim
                                 ? (double) HudRecorder::maxCaptureDim / (double) longSide : 1.0;
            outW = juce::jmax (2, juce::roundToInt (srcW * s) & ~1);
            outH = juce::jmax (2, juce::roundToInt (srcH * s) & ~1);
        }
        else if (h < 2)                     // width-driven: height follows the source
        {
            outW = juce::jmax (2, w & ~1);
            outH = juce::jmax (2, (int) std::lround ((double) outW * srcH / srcW) & ~1);
        }
        else                                // custom: exact frame, fit + letterbox
        {
            outW = juce::jmax (2, w & ~1);
            outH = juce::jmax (2, h & ~1);
        }
    }

    /** How much of that frame the source actually fills (1.0 = edge to edge).
        Only a CUSTOM size can be less than 1 now — the tiers follow the source. */
    double coverage (int srcW, int srcH) const noexcept
    {
        if (srcW < 1 || srcH < 1 || sizeMode != Custom) return 1.0;

        const double s = juce::jmin ((double) customW / (double) srcW,
                                     (double) customH / (double) srcH);
        return (srcW * s * srcH * s) / ((double) customW * (double) customH);
    }
};

// ── the dialog body ───────────────────────────────────────────────────────────
class AlterExportDialog : public juce::Component
{
public:
    AlterExportDialog (const AlterExportSettings& initial,
                       const std::vector<PluginInstanceInfo>& instances,
                       int hudWidth, int hudHeight,
                       int screenWidth, int screenHeight,
                       std::function<void (AlterExportSettings)> onStart)
        : hudW (hudWidth), hudH (hudHeight),
          screenW (screenWidth), screenH (screenHeight),
          startCallback (std::move (onStart))
    {
        // ── what to record ──────────────────────────────────────────────────────
        cbSource.addItem ("HUD window",  1);
        cbSource.addItem ("Whole screen", 2);
        cbSource.addItem ("Each module separately", 3);
        addRow (lblSource, "Record", cbSource);

        // ── background ──────────────────────────────────────────────────────────
        cbBg.addItem ("Normal (.mp4)",                1);
        cbBg.addItem ("Transparent - alpha (.mov)",   2);
        addRow (lblBg, "Background", cbBg);

        // ── audio source (first: it is the choice people get wrong most often) ──
        audioIds.push_back (0);
        cbAudio.addItem ("System audio", 1);
        for (const auto& inst : instances)
        {
            const juce::String idStr ((juce::int64) inst.id);
            juce::String label = inst.name.isNotEmpty() ? inst.name : ("Plugin #" + idStr);
            if (! inst.active) label += "  (no signal)";
            cbAudio.addItem ("VST: " + label, (int) audioIds.size() + 1);
            audioIds.push_back (inst.id);
        }
        addRow (lblAudio, "Audio source", cbAudio);

        // ── size ────────────────────────────────────────────────────────────────
        // Pixels only. The shape came from the HUD window and is already decided.
        // Text filled in by refresh(): it names the SOURCE's own size, and the
        // source changes with the Record setting.
        cbSize.addItem ("Original size", 1);
        cbSize.addItem ("Full HD (1920 on the long side)", 2);
        cbSize.addItem ("HD (1280 on the long side)",      3);
        cbSize.addItem ("Custom size",                     4);
        addRow (lblSize, "Size", cbSize);

        addAndMakeVisible (lblCustom);
        lblCustom.setText ("Width x height", juce::dontSendNotification);
        for (auto* ed : { &edW, &edH })
        {
            addAndMakeVisible (*ed);
            ed->setInputRestrictions (5, "0123456789");
            ed->setJustification (juce::Justification::centred);
            ed->onTextChange = [this] { refresh(); };
        }
        addAndMakeVisible (lblTimes);
        lblTimes.setText ("x", juce::dontSendNotification);
        lblTimes.setJustificationType (juce::Justification::centred);

        // ── live result ─────────────────────────────────────────────────────────
        addAndMakeVisible (lblResult);
        lblResult.setJustificationType (juce::Justification::centredLeft);
        lblResult.setFont (juce::Font (juce::FontOptions (15.0f).withStyle ("Bold")));

        // ── help ────────────────────────────────────────────────────────────────
        addAndMakeVisible (help);
        help.setMultiLine (true);
        help.setReadOnly (true);
        help.setScrollbarsShown (false);
        help.setCaretVisible (false);
        help.setPopupMenuEnabled (false);
        help.setFont (juce::Font (juce::FontOptions (13.0f)));
        help.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        help.setColour (juce::TextEditor::outlineColourId,    juce::Colours::transparentBlack);
        help.setText (helpText(), false);

        addAndMakeVisible (btnStart);
        addAndMakeVisible (btnCancel);
        btnStart .onClick = [this] { finish (true);  };
        btnCancel.onClick = [this] { finish (false); };

        // ── restore the previous choices ────────────────────────────────────────
        cbSource   .setSelectedItemIndex (juce::jlimit (0, 2, initial.recordMode), juce::dontSendNotification);
        cbBg       .setSelectedItemIndex (initial.transparent ? 1 : 0,           juce::dontSendNotification);
        cbSize     .setSelectedItemIndex (juce::jlimit (0, 3, initial.sizeMode), juce::dontSendNotification);
        edW.setText (juce::String (initial.customW > 0 ? initial.customW : 1080), false);
        edH.setText (juce::String (initial.customH > 0 ? initial.customH : 1920), false);

        int audioSel = 0;
        for (size_t i = 0; i < audioIds.size(); ++i)
            if (audioIds[i] == initial.audioInstanceId) { audioSel = (int) i; break; }
        cbAudio.setSelectedItemIndex (audioSel, juce::dontSendNotification);

        cbSource .onChange = [this] { refresh(); };
        cbBg     .onChange = [this] { refresh(); };
        cbSize   .onChange = [this] { refresh(); };

        setSize (500, 512);
        refresh();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);

        // buttons pinned to the bottom
        auto buttons = r.removeFromBottom (30);
        btnStart .setBounds (buttons.removeFromRight (140));
        buttons.removeFromRight (8);
        btnCancel.setBounds (buttons.removeFromRight (90));
        r.removeFromBottom (12);

        // help text sits above them
        help.setBounds (r.removeFromBottom (194));   // sized to fit the text WITHOUT scrolling
        r.removeFromBottom (10);

        // the rest flows from the top; hidden rows take no space
        auto row = [&r] (juce::Component& label, juce::Component& control)
        {
            if (! control.isVisible()) return;
            auto line = r.removeFromTop (26);
            label.setBounds (line.removeFromLeft (110));
            control.setBounds (line);
            r.removeFromTop (8);
        };

        row (lblAudio,      cbAudio);
        r.removeFromTop (6);
        row (lblSource,     cbSource);
        row (lblBg,         cbBg);
        row (lblSize,       cbSize);

        if (edW.isVisible())
        {
            auto line = r.removeFromTop (26);
            lblCustom.setBounds (line.removeFromLeft (110));
            edW      .setBounds (line.removeFromLeft (80));
            lblTimes .setBounds (line.removeFromLeft (20));
            edH      .setBounds (line.removeFromLeft (80));
            r.removeFromTop (8);
        }

        r.removeFromTop (4);
        lblResult.setBounds (r.removeFromTop (24));
    }

    /** Everything the dialog currently describes. */
    AlterExportSettings currentSettings() const
    {
        AlterExportSettings s;
        s.recordMode  = juce::jlimit (0, 2, cbSource.getSelectedItemIndex());
        s.transparent = cbBg.getSelectedItemIndex() == 1;
        s.sizeMode    = juce::jlimit (0, 3, cbSize.getSelectedItemIndex());
        s.customW     = juce::jlimit (2, 7680, edW.getText().getIntValue());
        s.customH     = juce::jlimit (2, 7680, edH.getText().getIntValue());

        const int a = cbAudio.getSelectedItemIndex();
        s.audioInstanceId = (a > 0 && a < (int) audioIds.size()) ? audioIds[(size_t) a] : 0;
        return s;
    }

private:
    void addRow (juce::Label& label, const juce::String& text, juce::Component& control)
    {
        addAndMakeVisible (label);
        label.setText (text, juce::dontSendNotification);
        addAndMakeVisible (control);
    }

    /** Show only what applies to the current choice, and say what will come out. */
    void refresh()
    {
        const auto cfg    = currentSettings();
        const bool custom = cfg.sizeMode == AlterExportSettings::Custom;

        // A transparent take does not film the screen at all — it paints the
        // modules onto nothing — so "whole screen" has no meaning, and the .mov
        // it writes carries no audio track. Say so by disabling both rather than
        // letting someone pick a combination that silently does something else.
        cbAudio.setEnabled (! cfg.isEachModule());

        // Whole screen cannot be transparent — there is no component tree behind a
        // screen grab to take an alpha channel from. Every other combination works.
        if (cfg.transparent && cfg.isWholeScreen())
            cbSource.setSelectedItemIndex (0, juce::dontSendNotification);

        for (auto* c : { (juce::Component*) &lblCustom, (juce::Component*) &edW,
                         (juce::Component*) &lblTimes,  (juce::Component*) &edH })
            c->setVisible (custom);

        const int srcW = cfg.isWholeScreen() ? screenW : hudW;
        const int srcH = cfg.isWholeScreen() ? screenH : hudH;

        int w = 0, h = 0;
        cfg.resolve (srcW, srcH, w, h);

        // Name WHAT is being filmed as well as how big it comes out. "Original"
        // alone left it ambiguous whether that meant the window or the display —
        // and that ambiguity is expensive: a 9:16 HUD on a 2560-wide screen is
        // about 810 px wide, so a file that says so is not low quality, it is
        // small, and enlarging it afterwards is a 3x upscale that looks exactly
        // like the codec failed.
        //
        // The size shown is the RESOLVED one, so it also reflects the capture
        // ceiling rather than promising a 4K file the recorder will not produce.
        {
            int ow = 0, oh = 0;
            AlterExportSettings original = cfg;
            original.sizeMode = AlterExportSettings::DisplaySize;
            original.resolve (srcW, srcH, ow, oh);

            cbSize.changeItemText (1, juce::String (cfg.isWholeScreen() ? "Whole screen"
                                                                       : "HUD at display size")
                                      + " (" + juce::String (ow) + " x " + juce::String (oh) + ")");
        }

        juce::String txt = cfg.isEachModule()
                             ? juce::String ("Export: one file per module, each at its own size")
                             : "Export: " + juce::String (w) + " x " + juce::String (h) + " px";
        if (cfg.transparent) txt += "   .mov + alpha";

        // Only a hand-typed custom size can still disagree with the source shape.
        // The tiers follow it, so they can never letterbox.
        if (custom && ! cfg.isEachModule() && srcW > 1 && srcH > 1)
        {
            const int pct = juce::roundToInt (cfg.coverage (srcW, srcH) * 100.0);
            if (pct < 90)
                txt += "   (" + juce::String (pct) + "% filled, rest black)";
        }

        lblResult.setText (txt, juce::dontSendNotification);
        resized();
    }

    void finish (bool accepted)
    {
        auto cb  = startCallback;              // we are about to be deleted
        auto res = currentSettings();

        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState (accepted ? 1 : 0);

        if (accepted && cb != nullptr) cb (res);
    }

    static juce::String helpText()
    {
        // Short on purpose. Anything that only matters once belongs in a tooltip;
        // what stays here is what people get WRONG, which is why the transparent
        // note is the longest of the three.
        return
            "SHAPE comes from the HUD window - set it with Shape at the top right of "
            "the controller. This dialog only sets how many PIXELS.\n\n"
            "Full HD = 1920 on the long side, so a vertical HUD gives 1080x1920. "
            "Exporting smaller than the display also looks sharper than 1:1.\n\n"
            "TRANSPARENT writes a .mov with an alpha channel and uncompressed audio. "
            "Windows Media Player and Photos CANNOT open it - that is normal, not a "
            "broken file. Use After Effects, Premiere Pro, DaVinci Resolve or VLC.\n\n"
            "EACH MODULE SEPARATELY records every module in the HUD to its own file "
            "at the same time, each taking the audio of the plugin assigned to it. A "
            "Fusion counts as one module.";
    }

    int hudW = 0, hudH = 0, screenW = 0, screenH = 0;
    std::vector<juce::uint32> audioIds;
    std::function<void (AlterExportSettings)> startCallback;

    juce::Label lblAudio, lblSource, lblBg, lblSize, lblCustom, lblTimes, lblResult;
    juce::ComboBox cbAudio, cbSource, cbBg, cbSize;
    juce::TextEditor edW, edH, help;
    juce::TextButton btnStart { "Start recording" }, btnCancel { "Cancel" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AlterExportDialog)
};

/** Opens the export dialog; `onStart` runs only if the user confirms. */
inline void alterShowExportDialog (const AlterExportSettings& initial,
                                  const std::vector<PluginInstanceInfo>& instances,
                                  int hudWidth, int hudHeight,
                                  std::function<void (AlterExportSettings)> onStart)
{
    // Physical pixels, to match hudWidth/hudHeight and the file the grab produces —
    // JUCE's totalArea is logical, and the two differ under display scaling.
    const auto& disp   = juce::Desktop::getInstance().getDisplays().getMainDisplay();
    const auto  screen = disp.totalArea;
    const int   scrW   = juce::roundToInt (screen.getWidth()  * disp.scale);
    const int   scrH   = juce::roundToInt (screen.getHeight() * disp.scale);

    auto* body = new AlterExportDialog (initial, instances, hudWidth, hudHeight,
                                        scrW, scrH, std::move (onStart));

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (body);
    o.dialogTitle                  = "Export settings";
    o.dialogBackgroundColour       = juce::LookAndFeel::getDefaultLookAndFeel()
                                        .findColour (juce::ResizableWindow::backgroundColourId);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar            = true;
    o.resizable                    = false;
    o.launchAsync();
}
