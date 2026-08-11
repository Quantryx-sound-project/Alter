#include "ControllerWindow.h"
#include "LogoData.h"
#include "QspLogoTextData.h"
#include <cstring>
#include <cmath>
#include <memory>
#include <vector>
#include "InfoWindowOscilator.h"
#include "InfoWindowSpectrum.h"
#include "InfoWindowSynesthesia.h"
#include "InfoWindowChladniPatterns.h"
#include "InfoWindowToneAnalyzer.h"
#include "InfoWindowSpectrogram.h"
#include "InfoWindowStereoscope.h"
#include "InfoWindowGeometry.h"
#include "InfoWindowFusion.h"
#include "GeometryVisual.h"        // beat-division table for the BPM-sync controls
#include "Oscilator.h"             // kMinWindowSec — the window slider's floor is the
                                   // module's own limit, not a number copied here
#include "ExportDialog.h"          // recording export settings dialog

// App data folders (AppData, not Documents – avoids Windows "Controlled folder
// access" blocking writes by Defender).
static juce::File alterDataDir()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("ALTER");
}
static juce::File alterPresetsDir()    { return alterDataDir().getChildFile ("Presets"); }
static juce::File alterRecordingsDir() { return alterDataDir().getChildFile ("Recordings"); }

// ── HUD shape presets ────────────────────────────────────────────────────────
// Ratios, not pixel sizes. How many pixels the export gets is a separate and
// much later question; this only decides the SHAPE of the frame you compose in.
const ControllerContent::AspectPreset ControllerContent::kAspectPresets[] =
{
    // Labels stay SHORT (the stepper sits in the narrow bottom strip); the platform
    // each ratio is for lives in the tooltip, which is where the explanation belongs.
    { "Default", 0.0,          "The shape the app starts with: full screen width, short banner" },
    { "9:16",    9.0  / 16.0,  "Reels, TikTok, Shorts - vertical full screen" },
    { "4:5",     4.0  /  5.0,  "Instagram feed post" },
    { "1:1",     1.0,          "Square - feed posts, album art" },
    { "16:9",    16.0 /  9.0,  "YouTube and any landscape video" },
};

// ─────────────────────────────────────────────────────────────────────────────
// Shared colour-picker popup (graphic-app style: 2D space + hue + hex code)
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    struct ColorPopup : public juce::Component
    {
        ColorPopup (juce::Colour initial, std::function<void(juce::Colour)> cb,
                    int w = 280, int h = 320)
            : onOk (std::move (cb))
        {
            addAndMakeVisible (cs);
            cs.setCurrentColour (initial);
            cs.setColour (juce::ColourSelector::backgroundColourId, AlterTheme::bgDeep);

            addAndMakeVisible (ok);
            addAndMakeVisible (cancel);
            ok.setButtonText ("OK");
            cancel.setButtonText ("Cancel");

            ok.onClick = [this]() {
                if (onOk) onOk (cs.getCurrentColour());
                if (auto* co = findParentComponentOfClass<juce::CallOutBox>()) co->dismiss();
            };
            cancel.onClick = [this]() {
                if (auto* co = findParentComponentOfClass<juce::CallOutBox>()) co->dismiss();
            };

            setSize (w, h);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (5);
            auto b = r.removeFromBottom (26);
            cs.setBounds (r);
            ok.setBounds (b.removeFromLeft (74));
            b.removeFromLeft (6);
            cancel.setBounds (b.removeFromLeft (74));
        }

        juce::ColourSelector cs { juce::ColourSelector::showColourAtTop
                                | juce::ColourSelector::editableColour
                                | juce::ColourSelector::showColourspace
                                | juce::ColourSelector::showSliders };
        juce::TextButton ok, cancel;
        std::function<void(juce::Colour)> onOk;
    };
}

// ===== ControllerContent =====

ControllerContent::ControllerContent (AlterState& s) : settings (s)
{
    setWantsKeyboardFocus (true);   // Ctrl+Z / Ctrl+Shift+Z (undo/redo)

    // After undo/redo the editor widgets must re-read the restored values —
    // without this they showed the OLD values until the module was re-selected.
    settings.onHistoryRestored = [this]
    {
        panelList.updateContent();
        refreshRightEditorFromSelection();
        resized();
        repaint();
    };

    // Keep the editor widgets in sync with values changed from OUTSIDE the
    // controller (a Control-mode VST automating this module, preset loads, ...).
    settings.getTree().addListener (this);

    // 20 Hz, not 2. This timer is what applies pendingEditorRefresh, and at 2 Hz
    // it was the controller's own perceived lag: the flag is set by ANY property
    // change on the selected panel — INCLUDING the ones the controller itself
    // just wrote — so every edit was followed by a full right-hand editor rebuild
    // and re-layout arriving up to half a second later. The visible rebuild that
    // long after the click is what "the controller takes ages to react" is.
    //
    // The tick itself is nearly free: it returns immediately unless
    // pendingEditorRefresh is set, so raising the rate costs a flag test, and
    // what it buys is that the rebuild lands inside one frame of the edit.
    startTimerHz (20);

    // ── shared helpers ──────────────────────────────────────────────────────
    auto setProp = [this](const juce::Identifier& prop, const juce::var& v)
    {
        if (selectedPanelId < 0) return;
        auto panel = settings.getPanelById (selectedPanelId);
        if (! panel.isValid()) return;

        // FLAGGED AS OURS while the write goes through.
        //
        // Our own valueTreePropertyChanged raises pendingEditorRefresh for every
        // property change on the selected panel, which is right for a change that
        // came from somewhere else (automation, a preset load, undo) — the widgets
        // genuinely have to be re-read. It is exactly wrong for a change the user
        // just made HERE: the widget already holds the value, and all the refresh
        // does is rebuild and re-lay-out the whole right-hand editor underneath
        // the control being used. Dragging a slider queued one of those per
        // frame, and the one that survived to mouse-up landed as a visible jump.
        const juce::ScopedValueSetter<bool> guard (writingOwnProperty, true);
        panel.setProperty (prop, v, nullptr);
    };

    // slider → float property
    auto bindSlider = [this, setProp](juce::Slider& sl, const juce::Identifier& prop)
    {
        editorPanel.addAndMakeVisible (sl);
        sl.setSliderStyle (juce::Slider::LinearHorizontal);
        sl.setRange (0.0, 1.0, 0.001);
        sl.onValueChange = [&sl, prop, setProp] { setProp (prop, (float) sl.getValue()); };
    };

    auto addLabel = [this](juce::Label& l, const juce::String& text)
    {
        editorPanel.addAndMakeVisible (l);
        l.setText (text, juce::dontSendNotification);
    };

    // LEFT: Add button + list
    addAndMakeVisible (btnAdd);
    btnAdd.onClick = [this]
    {
        juce::PopupMenu m;
        // The menu has three sections, coarsest thing last: the ordinary modules
        // you add all day, then Fusion — which is not a module you read but a
        // container that fuses other modules — then the block (row) operations.
        // Keeping Fusion out of the alphabetical run stops it being picked by
        // accident when someone is really after "Audio Meter".
        m.addSectionHeader ("Modules");
        // module types — alphabetical
        m.addItem (2, "Audio Meter");
        m.addItem (3, "Chladni Pattern");
        m.addItem (4, "Geometry");
        m.addItem (5, "Oscilloscope");
        m.addItem (6, "Spectrogram");
        m.addItem (7, "Spectrum");
        m.addItem (8, "Stereoscope");
        m.addItem (9, "Synesthesia");
        m.addItem (10, "Tone Analyzer");

        m.addSeparator();
        m.addSectionHeader ("Fusion");
        m.addItem (1, "Fusion");

        // HUD blocks (layers): up to 3 horizontal rows of modules
        const int blocks = settings.hudLayers();
        m.addSeparator();
        m.addSectionHeader ("Blocks");
        m.addItem (20, "New block (" + juce::String (blocks) + "/3)", blocks < 3);
        {
            // remove a SPECIFIC block, listed by number + its shade (white/grey/black)
            juce::PopupMenu rm;
            for (int b = 0; b < blocks; ++b)
                rm.addColouredItem (30 + b,
                                    "Block " + juce::String (b + 1)
                                      + "  (" + AlterTheme::blockShadeName (b) + ")",
                                    AlterTheme::blockShade (b),
                                    blocks > 1);
            m.addSubMenu ("Remove block", rm, blocks > 1);
        }
        {
            // Hide a WHOLE block — the row and every module in it leave the HUD,
            // and the blocks that stay take over its height. Ticked = currently
            // hidden, so the same entry is also the way back. An empty block is
            // greyed out: there is nothing in it to hide.
            juce::PopupMenu hb;
            for (int b = 0; b < blocks; ++b)
            {
                const int  count  = settings.blockModuleCount (b);
                const bool hidden = settings.blockIsHidden (b);

                // Spell out which way the click goes. The tick alone says "this
                // block is hidden", but the entry is also the ONLY way back, and
                // an entry called "Hide" is a bad place to look for "show".
                juce::String txt = "Block " + juce::String (b + 1)
                                     + "  (" + AlterTheme::blockShadeName (b) + ")";
                if      (count == 0) txt += "  - empty";
                else if (hidden)     txt += "  - show";
                else                 txt += "  - hide";

                hb.addColouredItem (40 + b, txt, AlterTheme::blockShade (b),
                                    count > 0, hidden);
            }
            m.addSubMenu ("Hide / show block", hb, blocks > 0);
        }

        m.showMenuAsync (juce::PopupMenu::Options(),
                         [this](int res)
                         {
                             // alphabetical — must match the menu order above
                             static const char* types[] = { "fusion", "audiometer", "chladni", "geometry",
                                                            "oscillator", "spectrogram", "spectrum",
                                                            "stereoscope", "synesthesia", "toneanalyzer" };
                             if (res >= 1 && res <= (int) juce::numElementsInArray (types))
                             {
                                 settings.addPanel (types[res - 1]);
                                 settings.sortPanelsByLayer();   // new module joins its block's bubble
                             }
                             else if (res == 20)
                                 settings.setHudLayers (settings.hudLayers() + 1);
                             else if (res >= 30 && res <= 32)
                                 settings.removeBlock (res - 30);
                             else if (res >= 40 && res <= 42)
                             {
                                 const int b = res - 40;
                                 settings.setBlockHidden (b, ! settings.blockIsHidden (b));
                             }
                             else
                                 return;

                             panelList.updateContent();
                             panelList.repaint();
                             refreshRightEditorFromSelection();
                             resized();
                         });
    };

    // ensure the auto-managed folders exist (AppData – not blocked by Windows
    // "Controlled folder access" the way Documents is)
    alterPresetsDir().createDirectory();
    alterRecordingsDir().createDirectory();

    addAndMakeVisible (btnSavePreset);
    btnSavePreset.setTooltip ("Save the whole controller setup (modules + settings) to a preset file");
    btnSavePreset.onClick = [this] { savePreset(); };

    addAndMakeVisible (btnLoadPreset);
    btnLoadPreset.setTooltip ("Load a saved controller preset");
    btnLoadPreset.onClick = [this] { loadPreset(); };

    // ── HUD shape presets ────────────────────────────────────────────────────
    // Aspect belongs to the WINDOW, not to the export: you shape the HUD to the
    // frame you are shooting for, see it that way while you work, and the export
    // then only has to decide how many pixels — never how to fit one shape into
    // another. That is why there are no black bars anywhere in this design.
    // No "Shape" caption: the ratio itself ("16:9", "9:16") already says what the
    // control is, and the caption only pushed the stepper away from the right edge.
    // A stepper rather than a row of buttons: five shapes would eat the whole top
    // bar, and this is a "pick one, occasionally" control, not something anyone
    // needs one click away.
    addAndMakeVisible (btnAspectPrev);
    addAndMakeVisible (btnAspectNext);
    btnAspectPrev.setConnectedEdges (juce::Button::ConnectedOnRight);
    btnAspectNext.setConnectedEdges (juce::Button::ConnectedOnLeft);

    addAndMakeVisible (lblAspectValue);
    lblAspectValue.setJustificationType (juce::Justification::centred);
    lblAspectValue.setColour (juce::Label::textColourId, AlterTheme::textBright);

    // Wraps around, so you can reach 16:9 from Default in one click either way.
    btnAspectPrev.onClick = [this] { applyAspect ((currentAspect + kNumAspects - 1) % kNumAspects); };
    btnAspectNext.onClick = [this] { applyAspect ((currentAspect + 1) % kNumAspects); };

    // Remember the last shape, but do NOT re-apply it on launch: resizing the
    // user's window out from under them at startup would be rude, and the app's
    // own startup geometry is the documented default.
    currentAspect = juce::jlimit (0, kNumAspects - 1,
                                  (int) settings.getTree().getProperty ("hudAspect", 0));
    refreshAspectButtons();

    addAndMakeVisible (btnRecord);
    btnRecord.setTooltip ("Record the HUD window - or the whole screen - to a video. You'll be "
                          "asked for the file name, then for what to capture, how many PIXELS "
                          "(display size / Full HD / HD / custom) and the AUDIO SOURCE - the "
                          "system mix, or the track of one connected ALTER plugin instance. The "
                          "SHAPE of the export comes from the HUD window itself: set it with the "
                          "Shape buttons at the top right.");
    btnRecord.onClick = [this]
    {
        if (recorder.isRecording() || ! moduleRecorders.empty())
        {
            recorder.stop();
            stopAllRecordings();
            // Release the recorder's claim on the plugin's waveform stream, so the
            // instance goes back to sending only what the visible modules need.
            if (setRecordingAudioInstance) setRecordingAudioInstance (0);
            // ...and put the HUD back on its normal background.
            if (setAlphaCaptureMode) setAlphaCaptureMode (false);
            btnRecord.setButtonText ("Record");
            btnRecord.removeColour (juce::TextButton::buttonColourId);
            return;
        }

        auto* hud = getHudComponent ? getHudComponent() : nullptr;
        if (hud == nullptr) return;

        auto base = alterRecordingsDir();
        base.createDirectory();

        const juce::String ext    = HudRecorder::outputExtension();
        const juce::String filter = "*" + ext;
        const auto defName = "ALTER " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S") + ext;

        presetChooser = std::make_unique<juce::FileChooser> (
            "Save recording as...", base.getChildFile (defName), filter);
        const auto chooserFlags = juce::FileBrowserComponent::saveMode
                                | juce::FileBrowserComponent::canSelectFiles
                                | juce::FileBrowserComponent::warnAboutOverwriting;

        presetChooser->launchAsync (chooserFlags, [this, hud] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File()) return;            // cancelled

            // STEP 2 of the record flow (after the file dialog): the export
            // settings dialog (audio source, size, background). Every choice is
            // remembered in the settings tree.
            AlterExportSettings initial;
            {
                auto& tree = settings.getTree();
                initial.sizeMode        = juce::jlimit (0, 3, (int) tree.getProperty ("recSize", 1));
                initial.customW         = (int) tree.getProperty ("recCustomW", 1080);
                initial.customH         = (int) tree.getProperty ("recCustomH", 1920);
                initial.recordMode      = juce::jlimit (0, 2, (int) tree.getProperty ("recMode", 0));
                initial.transparent     = (bool) tree.getProperty ("recTransparent", false);
                initial.audioInstanceId = (juce::uint32) (int) tree.getProperty ("recAudioSrcId", 0);
            }

            juce::Component::SafePointer<ControllerContent> safeThis (this);
            juce::Component::SafePointer<juce::Component>    safeHud  (hud);

            // PHYSICAL pixels, not JUCE's logical ones.
            //
            // The recorder BitBlts the window's client rect, which Windows gives in
            // real pixels, while Component::getWidth() is logical — the two agree
            // only at 100% display scaling. At 150% the dialog was quoting 1707x960
            // for a capture that actually came out 2560x1440, so "Original size"
            // named a number that appeared nowhere in the finished file. Aspect is
            // unaffected (both axes scale together), so this only makes the numbers
            // honest.
            auto* top = hud->getTopLevelComponent() != nullptr ? hud->getTopLevelComponent() : hud;

            double scale = 1.0;
            if (auto* d = juce::Desktop::getInstance().getDisplays()
                              .getDisplayForRect (top->getScreenBounds()))
                scale = d->scale;

            const int hudW = juce::roundToInt (top->getWidth()  * scale);
            const int hudH = juce::roundToInt (top->getHeight() * scale);

            alterShowExportDialog (initial,
                                   getPluginInstances ? getPluginInstances()
                                                      : std::vector<PluginInstanceInfo>{},
                                   hudW, hudH,
                                   [safeThis, safeHud, file, hudW, hudH] (AlterExportSettings cfg)
            {
                if (safeThis == nullptr || safeHud == nullptr) return;

                auto& tree = safeThis->settings.getTree();
                tree.setProperty ("recSize",          cfg.sizeMode,          nullptr);
                tree.setProperty ("recCustomW",       cfg.customW,           nullptr);
                tree.setProperty ("recCustomH",       cfg.customH,           nullptr);
                tree.setProperty ("recMode",          cfg.recordMode,        nullptr);
                tree.setProperty ("recTransparent",   cfg.transparent,       nullptr);
                tree.setProperty ("recAudioSrcId",    (int) cfg.audioInstanceId, nullptr);

                // The target is resolved against the SOURCE, so a size tier keeps
                // whatever shape the HUD window currently has and the export can
                // never letterbox.
                const auto& mainDisp = juce::Desktop::getInstance().getDisplays().getMainDisplay();
                const int scrW = juce::roundToInt (mainDisp.totalArea.getWidth()  * mainDisp.scale);
                const int scrH = juce::roundToInt (mainDisp.totalArea.getHeight() * mainDisp.scale);
                const int srcW = cfg.isWholeScreen() ? scrW : hudW;
                const int srcH = cfg.isWholeScreen() ? scrH : hudH;
                safeThis->recorder.setTargetResolution (cfg.targetWidth  (srcW, srcH),
                                                        cfg.targetHeight (srcW, srcH));
                safeThis->recorder.setCaptureSource (cfg.isWholeScreen()
                                                         ? HudRecorder::CaptureSource::WholeScreen
                                                         : HudRecorder::CaptureSource::HudWindow);

                // ── transparent capture ────────────────────────────────────────
                // Armed BEFORE the audio decision, because a transparent take has
                // no audio track at all and the .mov writer would drop it anyway.
                // Per-module with plugin audio: claim every instance NOW so the
                // 'W' streams are running by the time the warm-up expires.
                if (cfg.isEachModule() && safeThis->setRecordingAudioInstances
                    && safeThis->getExportModules)
                {
                    const auto eff = safeThis->getEffectiveAudioInstance
                                       ? safeThis->getEffectiveAudioInstance() : 0;
                    std::vector<juce::uint32> ids;
                    for (const auto& m : safeThis->getExportModules())
                        if (const auto id = m.audioInstance != 0 ? m.audioInstance : eff; id != 0)
                            ids.push_back (id);
                    safeThis->setRecordingAudioInstances (ids);
                }

                if (cfg.transparent && safeThis->grabAlphaFrame && safeThis->setAlphaCaptureMode)
                {
                    safeThis->setAlphaCaptureMode (true);
                    auto grab = safeThis->grabAlphaFrame;
                    safeThis->recorder.setAlphaSource ([grab] (int w, int h) { return grab (w, h); });
                }
                else
                {
                    safeThis->recorder.setAlphaSource (nullptr);
                    if (safeThis->setAlphaCaptureMode) safeThis->setAlphaCaptureMode (false);
                }

                // ── audio source ───────────────────────────────────────────────
                // Transparent takes carry audio too now, so the choice applies
                // to both pipelines.
                const juce::uint32 srcId = cfg.audioInstanceId;

                // Configuring a PLUGIN source is deferred to just before start():
                // its sample rate is measured from the 'W' stream, and that stream
                // only starts flowing once the request below has reached the plugin
                // (the needs mask goes out every 500 ms). Measuring it here would
                // read 0 Hz and fall back to 48 kHz, so a 44.1 kHz session would
                // export a track that plays ~9% sharp.
                std::function<void()> configureAudio;

                if (srcId == 0 || ! safeThis->pullPluginStereo)
                {
                    safeThis->recorder.setSystemAudioSource();
                    if (safeThis->setRecordingAudioInstance) safeThis->setRecordingAudioInstance (0);
                }
                else
                {
                    // Ask the instance for its waveform stream FIRST. Without this the
                    // plugin only sends what the visible modules need, 'W' is not on
                    // that list unless a scope happens to be open, and the recorder
                    // pulls an empty stream for the whole take.
                    if (safeThis->setRecordingAudioInstance) safeThis->setRecordingAudioInstance (srcId);

                    configureAudio = [safeThis, srcId]()
                    {
                        if (safeThis == nullptr) return;

                        // The plugin's own stream: measured rate, snapped to the nearest
                        // standard one so the exported track plays back at the right speed.
                        const double measured = safeThis->getPluginStreamRate
                                                    ? safeThis->getPluginStreamRate (srcId) : 0.0;
                        static const int kStdRates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
                        int rate = 48000;
                        if (measured > 1000.0)
                        {
                            double best = 1.0e9;
                            for (int r : kStdRates)
                                if (const double d = std::abs (measured - (double) r); d < best)
                                    { best = d; rate = r; }
                        }

                        // The AAC encoders (Media Foundation / AVFoundation) only take
                        // 44.1 or 48 kHz, so a high-rate session is decimated by an
                        // integer factor (88.2→44.1, 96→48, 176.4→44.1, 192→48) with a
                        // box average, instead of ending up with no audio track at all.
                        int decim = 1;
                        while (rate / decim > 48000 && decim < 8) decim *= 2;
                        const int outRate = rate / decim;

                        auto pull   = safeThis->pullPluginStereo;
                        auto cursor = std::make_shared<std::uint64_t> (0);
                        auto pendL  = std::make_shared<std::vector<float>>();
                        auto pendR  = std::make_shared<std::vector<float>>();
                        auto dbgAt  = std::make_shared<double> (0.0);   // last log time, seconds

                        DBG ("ALTER REC: plugin audio source id " << (int) srcId
                             << ", measured " << juce::String (measured, 1) << " Hz -> snapped "
                             << rate << " Hz, decim " << decim << ", out " << outRate << " Hz");

                        safeThis->recorder.setExternalAudioSource (
                            [pull, srcId, cursor, pendL, pendR, decim, dbgAt] (std::vector<float>& out) -> int
                            {
                                std::vector<float> l, r;
                                const bool live = pull (srcId, *cursor, l, r);
                                const size_t n  = live ? juce::jmin (l.size(), r.size()) : 0;

                                // Once a second, say WHICH of the two silent failures we are
                                // looking at: no stream for this instance at all (`live` false
                                // — the id is unknown to the receiver, or no 'W' packet has
                                // ever arrived), or a stream that simply is not advancing.
                                if (const double nowSec = juce::Time::getMillisecondCounterHiRes() * 0.001;
                                    nowSec - *dbgAt > 1.0)
                                {
                                    *dbgAt = nowSec;
                                    DBG ("ALTER REC: pull id " << (int) srcId
                                         << " -> live " << (live ? "YES" : "NO")
                                         << ", frames " << (int) n
                                         << ", cursor " << (juce::int64) *cursor);
                                }

                                if (! live || n == 0) return 0;

                                if (decim <= 1)   // common case: no rate conversion
                                {
                                    out.reserve (out.size() + n * 2);
                                    for (size_t i = 0; i < n; ++i) { out.push_back (l[i]); out.push_back (r[i]); }
                                    return (int) n;
                                }

                                pendL->insert (pendL->end(), l.begin(), l.begin() + (long) n);
                                pendR->insert (pendR->end(), r.begin(), r.begin() + (long) n);

                                const size_t d = (size_t) decim;
                                const size_t frames = pendL->size() / d;
                                out.reserve (out.size() + frames * 2);
                                for (size_t f = 0; f < frames; ++f)
                                {
                                    float sl = 0.0f, sr = 0.0f;
                                    for (size_t k = 0; k < d; ++k)
                                    {
                                        sl += (*pendL)[f * d + k];
                                        sr += (*pendR)[f * d + k];
                                    }
                                    out.push_back (sl / (float) d);
                                    out.push_back (sr / (float) d);
                                }
                                pendL->erase (pendL->begin(), pendL->begin() + (long) (frames * d));
                                pendR->erase (pendR->begin(), pendR->begin() + (long) (frames * d));
                                return (int) frames;
                            },
                            outRate);
                    };
                }

                // Re-dock any detached/popped-out modules so the recording captures
                // the whole HUD. The re-attach + relayout is async, so we start
                // recording only after it has settled.
                bool anyDetached = false;
                auto panels = safeThis->settings.getPanelsRoot();
                for (int i = 0; i < panels.getNumChildren(); ++i)
                {
                    auto p = panels.getChild (i);
                    if ((bool) p.getProperty (AlterState::kDetached, false))
                    {
                        p.setProperty (AlterState::kDetached, false, nullptr);
                        anyDetached = true;
                    }
                }

                auto begin = [safeThis, safeHud, file, configureAudio, cfg]()
                {
                    if (safeThis == nullptr || safeHud == nullptr) return;

                    if (cfg.isEachModule())
                    {
                        if (safeThis->startPerModuleRecording (cfg, file))
                        {
                            safeThis->btnRecord.setButtonText ("Stop");
                            safeThis->btnRecord.setColour (juce::TextButton::buttonColourId,
                                                           juce::Colours::red.darker (0.2f));
                        }
                        else if (safeThis->setAlphaCaptureMode)
                        {
                            safeThis->setAlphaCaptureMode (false);
                        }
                        return;
                    }

                    if (configureAudio) configureAudio();   // plugin source: rate is known by now
                    if (safeThis->recorder.start (safeHud.getComponent(), file))
                    {
                        safeThis->btnRecord.setButtonText ("Stop");
                        safeThis->btnRecord.setColour (juce::TextButton::buttonColourId,
                                                       juce::Colours::red.darker (0.2f));
                    }
                    else if (safeThis->setRecordingAudioInstance)
                    {
                        safeThis->setRecordingAudioInstance (0);   // start failed: drop the claim
                    }
                };

                // A plugin source needs a moment before its rate can be known, and
                // the budget is additive: up to 500 ms for the needs request to go
                // out, then a FULL 1 s window before the first honest measurement —
                // full, because the stream having just been switched on invalidates
                // the previous window rather than continuing it. Asking too early
                // reads 0 and silently falls back to 48 kHz, which is exactly how a
                // 44.1 kHz session ends up exported ~9% sharp.
                // Re-docked modules need 300 ms to settle; wait for whichever is more.
                const bool needsPluginWarmup = (srcId != 0) || cfg.isEachModule();
                const int delayMs = juce::jmax (anyDetached ? 300 : 0, needsPluginWarmup ? 2200 : 0);

                if (delayMs > 0) juce::Timer::callAfterDelay (delayMs, begin);
                else             begin();
            });
        });
    };

    settings.sortPanelsByLayer();   // group modules per block → bubbles render contiguous

    addAndMakeVisible (panelList);
    panelList.setModel (this);
    panelList.setRowHeight (26);

    // Enable drag & drop reordering
    dragOverlay.setAlwaysOnTop (true);
    dragOverlay.setVisible (false);
    dragOverlay.setInterceptsMouseClicks (false, false);

    // RIGHT: Delete button
    editorPanel.addAndMakeVisible (btnDelete);
    btnDelete.onClick = [this]
    {
        if (selectedPanelId < 0) return;

        settings.removePanelById (selectedPanelId);

        selectedPanelId = -1;
        panelList.deselectAllRows();
        panelList.updateContent();

        resized();
        repaint();
    };

    // Per-module audio source: Auto (use the global instance at the bottom) or a
    // specific plugin instance, which takes priority for THIS module only.
    editorPanel.addAndMakeVisible (lblModuleInstance);
    lblModuleInstance.setText ("Source", juce::dontSendNotification);
    lblModuleInstance.setColour (juce::Label::textColourId, AlterTheme::textDim);
    lblModuleInstance.setJustificationType (juce::Justification::centredRight);
    editorPanel.addAndMakeVisible (cbModuleInstance);
    cbModuleInstance.setTooltip ("Audio source for THIS module.\n"
                                 "Auto = the global instance picked at the bottom.");
    cbModuleInstance.addItem ("Auto", 1);
    cbModuleInstance.setSelectedId (1, juce::dontSendNotification);
    cbModuleInstance.onChange = [this]
    {
        if (selectedPanelId < 0) return;
        const int sel = cbModuleInstance.getSelectedId();
        const juce::uint32 instId = (sel >= 2 && sel - 2 < (int) cachedInstances.size())
                                  ? cachedInstances[(size_t)(sel - 2)].id : 0;
        auto panel = settings.getPanelById (selectedPanelId);
        if (panel.isValid())
            panel.setProperty (AlterState::kAudioInstance, (juce::int64) instId, nullptr);
    };

    // Get Info button (educational guide)
    editorPanel.addAndMakeVisible (btnGetInfo);
    btnGetInfo.onClick = [this]
    {
        if (selectedPanelId < 0) return;
        auto panel = settings.getPanelById (selectedPanelId);
        if (! panel.isValid()) return;

        const auto type = panel.getProperty (AlterState::kType).toString();

        ModuleInfoWindow* w = nullptr;
        if (type == "audiometer" || type == "rms")            w = new AudioMeterInfoWindow();
        else if (type == "spectrum")                          w = new SpectrumInfoWindow();
        else if (type == "oscillator" || type == "oscilator") w = new OscillatorInfoWindow();
        else if (type == "synesthesia")                       w = new SynesthesiaInfoWindow();
        else if (type == "chladni")                           w = new ChladniInfoWindow();
        else if (type == "toneanalyzer")                      w = new ToneAnalyzerInfoWindow();
        else if (type == "spectrogram")                       w = new SpectrogramInfoWindow();
        else if (type == "stereoscope")                       w = new StereoscopeInfoWindow();
        else if (type == "geometry")                          w = new GeometryInfoWindow();
        else if (type == "fusion")                           w = new FusionInfoWindow();

        if (w != nullptr)
        {
            currentInfoWindow.reset (w);   // owns it; deletes any previous window
            currentInfoWindow->setVisible (true);
            currentInfoWindow->toFront (true);
            if (alwaysOnTop.getToggleState())
                currentInfoWindow->setAlwaysOnTop (true);
        }
    };

    // ── RMS editor ──────────────────────────────────────────────────────────
    addLabel (lblRmsMode, "Mode");

    editorPanel.addAndMakeVisible (cbRmsPeakMode);
    cbRmsPeakMode.addItem ("RMS (Average)", 1);
    cbRmsPeakMode.addItem ("True Peak", 2);
    cbRmsPeakMode.addItem ("LUFS (Loudness)", 3);
    cbRmsPeakMode.addItem ("Level history", 4);
    cbRmsPeakMode.setSelectedId (1, juce::dontSendNotification);
    cbRmsPeakMode.onChange = [this, setProp]
    {
        setProp (AlterState::kMeterMode, cbRmsPeakMode.getSelectedId() - 1);
        resized();   // Level history swaps the meter controls for its own
    };

    addLabel (lblMeterView, "View");

    editorPanel.addAndMakeVisible (cbMeterView);
    cbMeterView.addItem ("Momentary", 1);
    cbMeterView.addItem ("Trend", 2);
    cbMeterView.setSelectedId (1, juce::dontSendNotification);
    cbMeterView.onChange = [this, setProp]
    {
        const int view = cbMeterView.getSelectedId() - 1; // 0=Momentary, 1=Trend
        setProp (AlterState::kMeterView, view);
        if (view == 0)
            setProp (AlterState::kMeasureState, 0);
        resized(); // show/hide Trend controls
    };

    // Trend measurement controls
    editorPanel.addAndMakeVisible (btnMeasureStart);
    btnMeasureStart.setColour (juce::TextButton::buttonColourId,
                               AlterTheme::caribbeanGreen.darker (0.6f));
    btnMeasureStart.onClick = [setProp] { setProp (AlterState::kMeasureState, 1); };

    editorPanel.addAndMakeVisible (btnMeasureStop);
    btnMeasureStop.setColour (juce::TextButton::buttonColourId,
                              AlterTheme::cerise.darker (0.7f));
    btnMeasureStop.onClick = [setProp] { setProp (AlterState::kMeasureState, 0); };

    editorPanel.addAndMakeVisible (lblMeasureStatus);
    lblMeasureStatus.setText ("Idle", juce::dontSendNotification);
    lblMeasureStatus.setJustificationType (juce::Justification::centred);

    addLabel (lblRmsSmooth, "Smooth");
    bindSlider (sRmsSmooth, AlterState::kSmooth);
    // 0.500, matching kSmooth's default in AlterState, and stated HERE as well as
    // there for a reason: bindSlider leaves a slider sitting at 0, and
    // finaliseSliders then records "wherever it sits right now" as its
    // double-click-to-default. So this control's reset value was 0 — not the
    // module's default but the absence of one — and a meter that had been reset,
    // or whose editor wrote the widget's value back before the state was read into
    // it, ended up genuinely smoothing by nothing at all.
    sRmsSmooth.setValue (0.5, juce::dontSendNotification);
    sRmsSmooth.setDoubleClickReturnValue (true, 0.5);

    // ── Spectrum editor ─────────────────────────────────────────────────────
    addLabel (lblSpecSmooth, "Smooth");
    bindSlider (sSpecSmooth, AlterState::kSmooth);

    // ── Oscillator editor ───────────────────────────────────────────────────
    addLabel (lblOscSmooth, "Smooth");
    bindSlider (sOscSmooth, AlterState::kSmooth);

    editorPanel.addAndMakeVisible (tbFill);
    tbFill.setButtonText ("Fill");
    tbFill.setClickingTogglesState (true);
    tbFill.onClick = [this, setProp] { setProp (AlterState::kNeon, tbFill.getToggleState()); };

    addLabel (lblDisplayMode, "Display");

    editorPanel.addAndMakeVisible (cbDisplayMode);
    cbDisplayMode.addItem ("Mono", 1);
    cbDisplayMode.addItem ("Stereo", 2);
    cbDisplayMode.addItem ("Mirror", 3);
    cbDisplayMode.addItem ("Mirror Stereo", 4);
    cbDisplayMode.onChange = [this, setProp]
    {
        setProp (AlterState::kDisplayMode, juce::jlimit (0, 3, cbDisplayMode.getSelectedId() - 1));
        resized();   // the L/R colour row exists only in the overlaid Stereo layout
    };

    // Oscillator zoom (time window)
    addLabel (lblOscZoom, "Zoom");
    bindSlider (sOscZoom, AlterState::kZoom);
    sOscZoom.setValue (0.420);
    sOscZoom.setDoubleClickReturnValue (true, 0.420);
    sOscZoom.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    sOscZoom.textFromValueFunction = [](double v)
    {
        const double twMs = 85.0 * std::pow (0.001176, v);
        if (twMs >= 1.0)
            return juce::String (twMs, 1) + " ms";
        return juce::String (twMs * 1000.0, 0) + juce::String::charToString (0x00B5) + "s";
    };
    sOscZoom.valueFromTextFunction = [](const juce::String& text)
    {
        double ms = text.getDoubleValue();
        if (text.containsIgnoreCase ("us") || text.containsChar (0x00B5))
            ms *= 0.001;
        if (ms <= 0.0) return 0.420;
        return juce::jlimit (0.0, 1.0, std::log (ms / 85.0) / std::log (0.001176));
    };

    // Oscillator view mode: short waveform / level envelope / long raw wave
    addLabel (lblOscTerm, "Term");
    editorPanel.addAndMakeVisible (cbOscTerm);
    cbOscTerm.addItem ("Short-Term Scope", 1);
    cbOscTerm.addItem ("Long Waveform",    2);   // raw waveform over a long window
    cbOscTerm.setSelectedId (1, juce::dontSendNotification);   // (Level history moved to the Audio Meter)
    cbOscTerm.onChange = [this, setProp]
    {
        setProp (AlterState::kOscLongTerm, cbOscTerm.getSelectedId() - 1);
        resized();   // show/hide window slider vs zoom
    };

    addLabel (lblOscLtWin, "Window");
    bindSlider (sOscLtWin, AlterState::kOscLtWindow);
    // The RANGE is set per view in resized(): this one slider serves both the
    // Oscilloscope's Long Waveform and the meter's Level history, and they do not
    // bottom out in the same place. Long Wave stores raw samples and stays readable
    // down to 20 ms; Level history stores one envelope point per 30 Hz tick, so a
    // 20 ms window would hold less than one point. Only the step and the shape are
    // fixed here.
    sOscLtWin.setRange (0.02, 30.0, 0.01);
    sOscLtWin.setSkewFactorFromMidPoint (5.0);   // fine control in the short range
    sOscLtWin.setValue (10.0, juce::dontSendNotification);
    sOscLtWin.setDoubleClickReturnValue (true, 10.0);
    sOscLtWin.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    // Milliseconds below a second: with a 0.01 step the seconds reading spends the
    // whole bottom of the travel saying "0.02 s", where "20 ms" is the number the
    // user is actually dialling in.
    sOscLtWin.textFromValueFunction = [] (double v)
    {
        return v < 1.0 ? juce::String (v * 1000.0, 0) + " ms"
                       : juce::String (v, v < 10.0 ? 2 : 1) + " s";
    };
    sOscLtWin.valueFromTextFunction = [] (const juce::String& t)
    {
        const double v = t.retainCharacters ("0123456789.-").getDoubleValue();
        return t.containsIgnoreCase ("ms") ? v / 1000.0 : v;
    };

    editorPanel.addAndMakeVisible (tbOscSymmetry);
    tbOscSymmetry.setButtonText ("Symmetry");
    tbOscSymmetry.setClickingTogglesState (true);
    tbOscSymmetry.onClick = [this, setProp]
    { setProp (AlterState::kOscSymmetry, tbOscSymmetry.getToggleState()); };

    // ── Synesthesia editor ──────────────────────────────────────────────────
    addLabel (lblSynSmooth, "Tone smooth");
    bindSlider (sSynSmooth, AlterState::kSmooth);

    addLabel (lblSynGhost, "Curve smooth");
    bindSlider (sSynGhost, AlterState::kCurveSmooth);   // 0..1: ghost/lag of curve motion

    addLabel (lblZoom, "Zoom");
    bindSlider (sZoom, AlterState::kZoom);
    sZoom.setRange (0.5, 2.0, 0.01);
    sZoom.setValue (1.0);

    addLabel (lblRotation, "Rotation");
    editorPanel.addAndMakeVisible (sRotation);
    sRotation.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    sRotation.setRange (-360.0, 360.0, 1.0);   // bipolar: 0 = still, +/- = spin each way
    sRotation.setValue (0.0);
    sRotation.onValueChange = [this, setProp] { setProp (AlterState::kRotation, (float) sRotation.getValue()); };

    addLabel (lblSymmetry, "Symmetry");
    editorPanel.addAndMakeVisible (sSymmetry);
    sSymmetry.setSliderStyle (juce::Slider::IncDecButtons);
    sSymmetry.setRange (1, 8, 1);
    sSymmetry.setValue (1);
    sSymmetry.onValueChange = [this, setProp] { setProp (AlterState::kSymmetry, (int) sSymmetry.getValue()); };

    editorPanel.addAndMakeVisible (tbSynMirror);
    tbSynMirror.setButtonText ("Mirror");
    tbSynMirror.setClickingTogglesState (true);
    tbSynMirror.onClick = [this, setProp]
    { setProp (AlterState::kMirror, tbSynMirror.getToggleState()); };

    addLabel (lblSaturation, "Saturation");
    bindSlider (sSaturation, AlterState::kSaturation);
    sSaturation.setRange (0.0, 2.0, 0.01);
    sSaturation.setValue (1.0);

    addLabel (lblSynBright, "Brightness");
    bindSlider (sSynBright, AlterState::kSynBrightness);
    sSynBright.setRange (0.0, 2.0, 0.01);
    sSynBright.setValue (1.0);

    addLabel (lblBloom, "Bloom");
    bindSlider (sBloom, AlterState::kBloom);
    sBloom.setRange (-1.0, 1.0, 0.01);   // bipolar: negative dims below neutral, positive boosts
    sBloom.setValue (0.0);

    addLabel (lblSpeed, "Speed");
    bindSlider (sSpeed, AlterState::kSpeed);
    sSpeed.setRange (-2.0, 2.0, 0.01);   // 0 = still, negative = reverse evolution
    sSpeed.setValue (1.0);

    addLabel (lblSynReact, "React");
    bindSlider (sSynReact, AlterState::kSynReact);

    addLabel (lblSynChange, "Fragment");
    bindSlider (sSynChange, AlterState::kFragment);   // 0..1: morphs the fractal variation (formerly "Change")

    addLabel (lblSynTransmute, "Transmute");
    bindSlider (sSynTransmute, AlterState::kTransmute);   // 0..1: second, symmetric morph

    addLabel (lblSynClear, "Clear");
    bindSlider (sSynClear, AlterState::kClear);   // 0..1: final cleanup/merge top layer

    addLabel (lblSynDenoise, "Denoise");
    bindSlider (sSynDenoise, AlterState::kDenoise);   // 0..1: fuse dashed secondary curves into continuous, dimmer lines

    addLabel (lblSynTunnel, "Tunnel");
    bindSlider (sSynTunnel, AlterState::kSynTunnel);  // 0..1: symmetric fly-through tunnel morph (0 = flat shader)

    addLabel (lblSynVortex, "Vortex");
    bindSlider (sSynVortex, AlterState::kSynVortex);  // bipolar: swirl the image into a spiral, either rotational direction
    sSynVortex.setRange (-1.0, 1.0, 0.01);   // bipolar: negative = swirl the other way

    // ── Fusion editor ──────────────────────────────────────────────────────
    {
        // The fold headers. Everything under one of these belongs to it, and
        // clicking it takes the whole block away — which is the only way three
        // layers and a global chain fit in a controller you can read.
        for (int i = 0; i < AlterState::kMaxFusionLayers; ++i)
        {
            editorPanel.addAndMakeVisible (btnFusionLayHead[i]);
            btnFusionLayHead[i].setConnectedEdges (juce::Button::ConnectedOnLeft
                                                 | juce::Button::ConnectedOnRight);
            btnFusionLayHead[i].onClick = [this, i] { toggleFusionBlock (i); };
        }

        editorPanel.addAndMakeVisible (btnFusionGlobalHead);
        btnFusionGlobalHead.setConnectedEdges (juce::Button::ConnectedOnLeft
                                             | juce::Button::ConnectedOnRight);
        btnFusionGlobalHead.onClick = [this] { toggleFusionBlock (-1); };

        for (int i = 0; i < AlterState::kMaxFusionLayers; ++i)
        {
            addLabel (lblFusionLayer[i], "Module");
            editorPanel.addAndMakeVisible (cbFusionLayer[i]);
            cbFusionLayer[i].onChange = [this, i]
            {
                // Item id 1 is "(none)"; anything else is panelId + 1.
                const int sel = cbFusionLayer[i].getSelectedId();
                applyFusionLayerPick (i, sel <= 1 ? 0 : sel - 1);
            };
        }

        // A plain 0..1 slider bound to an indexed per-layer property.
        auto laySlider = [this, setProp] (juce::Slider& s, const char* name, int i,
                                          double lo, double hi, double step, double def)
        {
            editorPanel.addAndMakeVisible (s);
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setRange (lo, hi, step);
            s.setValue (def);
            s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s.setPopupDisplayEnabled (true, false, this);
            const auto prop = AlterState::fusionLayerProp (name, i);
            s.onValueChange = [&s, prop, setProp] { setProp (prop, (float) s.getValue()); };
        };

        // A COUNT — axes, wedges — bound to an indexed per-layer property. Stored
        // as an int, because that is what it is: a stepper that writes 3.0 leaves
        // a float in the tree and the preset then reads back something that only
        // looks like the number the user chose.
        auto layStepper = [this, setProp] (juce::Slider& s, const char* name, int i,
                                           int lo, int hi, int def)
        {
            editorPanel.addAndMakeVisible (s);
            s.setSliderStyle (juce::Slider::IncDecButtons);
            s.setRange (lo, hi, 1);
            s.setValue (def);
            const auto prop = AlterState::fusionLayerProp (name, i);
            s.onValueChange = [&s, prop, setProp] { setProp (prop, (int) s.getValue()); };
        };

        for (int i = 0; i < AlterState::kMaxFusionLayers; ++i)
        {
            // BLEND: how this layer meets everything below it. Slot A is the base —
            // there is nothing under it, so it has no blend and the box stays hidden.
            editorPanel.addAndMakeVisible (cbFusionBlend[i]);
            cbFusionBlend[i].addItem ("Screen", 1);
            cbFusionBlend[i].addItem ("Merge",  2);
            cbFusionBlend[i].addItem ("Weave",  3);
            cbFusionBlend[i].onChange = [this, i, setProp]
            {
                setProp (AlterState::fusionLayerProp (AlterState::kFusionLayBlend, i),
                         juce::jmax (0, cbFusionBlend[i].getSelectedId() - 1));
                refreshRightEditorFromSelection();   // Merge and Weave show different settings
            };

            addLabel (lblFusionOpacity[i], "Opacity");
            laySlider (sFusionOpacity[i], AlterState::kFusionLayOpacity, i, 0.0, 1.0, 0.01, 1.0);

            addLabel (lblFusionAmount[i], "Amount");
            laySlider (sFusionAmount[i], AlterState::kFusionLayAmount, i, 0.0, 1.0, 0.01, 0.5);

            addLabel (lblFusionScale[i], "Scale");
            laySlider (sFusionScale[i], AlterState::kFusionLayScale, i, 0.1, 4.0, 0.01, 1.0);

            addLabel (lblFusionBands[i], "Bands");
            laySlider (sFusionBands[i], AlterState::kFusionLayBands, i, 2.0, 32.0, 1.0, 8.0);

            addLabel (lblFusionAngle[i], "Angle");
            laySlider (sFusionAngle[i], AlterState::kFusionLayAngle, i, -180.0, 180.0, 1.0, 0.0);

            // ── this layer's own post chain ─────────────────────────────────
            // The same four stages the global block offers, asked of this layer
            // alone. Mirror leaves the radius alone and Symmetry folds it, which
            // is why both are here rather than one being the other turned up.
            addLabel (lblFusionLayMirror[i], "Mirror");
            layStepper (sFusionLayMirror[i], AlterState::kFusionLayMirror, i, 0, 8, 0);
            // 0 axes means no fold, so the Axis row below has nothing to aim —
            // relaying out is what makes it come and go with the count.
            sFusionLayMirror[i].onValueChange = [this, i, setProp]
            {
                setProp (AlterState::fusionLayerProp (AlterState::kFusionLayMirror, i),
                         (int) sFusionLayMirror[i].getValue());
                refreshRightEditorFromSelection();
            };

            addLabel (lblFusionLayMirrorAng[i], "Axis");
            laySlider (sFusionLayMirrorAng[i], AlterState::kFusionLayMirrorAng, i,
                       -180.0, 180.0, 1.0, 0.0);

            addLabel (lblFusionLaySymmetry[i], "Symmetry");
            layStepper (sFusionLaySymmetry[i], AlterState::kFusionLaySymmetry, i, 1, 11, 1);

            addLabel (lblFusionLaySpin[i], "Spin");
            laySlider (sFusionLaySpin[i], AlterState::kFusionLaySpin, i, -180.0, 180.0, 1.0, 0.0);

            addLabel (lblFusionLaySpeed[i], "Speed");
            laySlider (sFusionLaySpeed[i], AlterState::kFusionLaySpeed, i, 0.0, 1.0, 0.01, 0.0);

            addLabel (lblFusionLayZoom[i], "Zoom");
            laySlider (sFusionLayZoom[i], AlterState::kFusionLayZoom, i, 0.25, 4.0, 0.01, 1.0);
        }

        // ── post chain ──────────────────────────────────────────────────────
        auto setupStage = [this, setProp] (juce::ToggleButton& tb, const juce::String& text,
                                           const juce::Identifier& prop)
        {
            editorPanel.addAndMakeVisible (tb);
            tb.setButtonText (text);
            tb.setClickingTogglesState (true);
            tb.onClick = [this, &tb, prop, setProp]
            {
                setProp (prop, tb.getToggleState());
                refreshRightEditorFromSelection();   // show / hide that stage's settings
            };
        };

        setupStage (tbFusionWarp, "Warp", AlterState::kFusionWarp);
        addLabel (lblFusionWarpAmt, "Amount");   bindSlider (sFusionWarpAmt,   AlterState::kFusionWarpAmt);
        addLabel (lblFusionWarpSwirl, "Swirl");  bindSlider (sFusionWarpSwirl, AlterState::kFusionWarpSwirl);
        addLabel (lblFusionWarpSmooth, "Smooth"); bindSlider (sFusionWarpSmooth, AlterState::kFusionWarpSmooth);
        addLabel (lblFusionWarpDenoise, "Denoise"); bindSlider (sFusionWarpDenoise, AlterState::kFusionWarpDenoise);
        addLabel (lblFusionWarpSrc, "Source");
        editorPanel.addAndMakeVisible (cbFusionWarpSrc);
        for (int i = 0; i < AlterState::kMaxFusionLayers; ++i)
            cbFusionWarpSrc.addItem (juce::String ("Layer ") + (char) ('A' + i), i + 1);
        cbFusionWarpSrc.onChange = [this, setProp]
        { setProp (AlterState::kFusionWarpSrc, juce::jmax (0, cbFusionWarpSrc.getSelectedId() - 1)); };

        // MIRROR, as one number. 1 is no fold at all, so the control needs no
        // separate on/off — the off state is a value it already has.
        addLabel (lblFusionSymmetry, "Symmetry");
        editorPanel.addAndMakeVisible (sFusionSymmetry);
        sFusionSymmetry.setSliderStyle (juce::Slider::IncDecButtons);
        sFusionSymmetry.setRange (1, 11, 1);
        sFusionSymmetry.setValue (1);
        sFusionSymmetry.onValueChange = [this, setProp]
        { setProp (AlterState::kFusionSymmetry, (int) sFusionSymmetry.getValue()); };

        // MIRROR is the other half of the same idea and deliberately separate:
        // Symmetry makes the picture radial, Mirror folds halves onto each other.
        // They compose — a mirrored picture cut into wedges is not the same thing
        // as either on its own.
        //
        // A COUNT, like Symmetry beside it, and for the same reason: 0 is the off
        // state, so the toggle this used to need is a value the control already
        // has. One axis is what it always did; N puts an axis every 180/N degrees.
        addLabel (lblFusionMirror, "Mirror");
        editorPanel.addAndMakeVisible (sFusionMirror);
        sFusionMirror.setSliderStyle (juce::Slider::IncDecButtons);
        sFusionMirror.setRange (0, 8, 1);
        sFusionMirror.setValue (0);
        sFusionMirror.onValueChange = [this, setProp]
        {
            setProp (AlterState::kFusionMirror, (int) sFusionMirror.getValue());
            refreshRightEditorFromSelection();   // the Axis slider only matters above 0
        };

        addLabel (lblFusionMirrorAngle, "Axis");
        editorPanel.addAndMakeVisible (sFusionMirrorAngle);
        sFusionMirrorAngle.setSliderStyle (juce::Slider::LinearHorizontal);
        sFusionMirrorAngle.setRange (-180.0, 180.0, 1.0);
        sFusionMirrorAngle.setValue (0.0);
        sFusionMirrorAngle.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        sFusionMirrorAngle.setPopupDisplayEnabled (true, false, this);
        sFusionMirrorAngle.onValueChange = [this, setProp]
        { setProp (AlterState::kFusionMirrorAngle, (float) sFusionMirrorAngle.getValue()); };

        addLabel (lblFusionSpin, "Spin");
        editorPanel.addAndMakeVisible (sFusionSpin);
        sFusionSpin.setSliderStyle (juce::Slider::LinearHorizontal);
        sFusionSpin.setRange (-180.0, 180.0, 1.0);
        sFusionSpin.setValue (0.0);
        sFusionSpin.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        sFusionSpin.setPopupDisplayEnabled (true, false, this);
        sFusionSpin.onValueChange = [this, setProp]
        { setProp (AlterState::kFusionSpin, (float) sFusionSpin.getValue()); };

        addLabel (lblFusionZoom, "Zoom");
        editorPanel.addAndMakeVisible (sFusionZoom);
        sFusionZoom.setSliderStyle (juce::Slider::LinearHorizontal);
        sFusionZoom.setRange (0.25, 4.0, 0.01);
        sFusionZoom.setValue (1.0);
        sFusionZoom.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        sFusionZoom.setPopupDisplayEnabled (true, false, this);
        sFusionZoom.onValueChange = [this, setProp]
        { setProp (AlterState::kFusionZoom, (float) sFusionZoom.getValue()); };

        // VORTEX: a turn that grows with the radius. Bipolar, because swirling the
        // other way is a different picture and not a smaller one.
        addLabel (lblFusionVortex, "Vortex");
        editorPanel.addAndMakeVisible (sFusionVortex);
        sFusionVortex.setSliderStyle (juce::Slider::LinearHorizontal);
        sFusionVortex.setRange (-1.0, 1.0, 0.01);
        sFusionVortex.setValue (0.0);
        sFusionVortex.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        sFusionVortex.setPopupDisplayEnabled (true, false, this);
        sFusionVortex.onValueChange = [this, setProp]
        { setProp (AlterState::kFusionVortex, (float) sFusionVortex.getValue()); };

        addLabel (lblFusionReact, "React");
        bindSlider (sFusionReact, AlterState::kFusionReact);
        sFusionReact.setValue (0.4);

        // WHICH layers the whole global chain touches. All on = fold the result, as
        // before; untick one to leave it standing still through the fold/tunnel/liquid.
        addLabel (lblFusionGlobLayers, "Global on");
        {
            const juce::Identifier globProps[AlterState::kMaxFusionLayers] =
                { AlterState::kFusionGlobL0, AlterState::kFusionGlobL1, AlterState::kFusionGlobL2 };
            for (int i = 0; i < AlterState::kMaxFusionLayers; ++i)
            {
                auto& tb = tbFusionGlobLayer[i];
                editorPanel.addAndMakeVisible (tb);
                tb.setButtonText (juce::String::charToString ((juce::juce_wchar) ('A' + i)));
                tb.setClickingTogglesState (true);
                const auto prop = globProps[i];
                tb.onClick = [this, &tb, prop, setProp] { setProp (prop, tb.getToggleState()); };
            }
        }

        // LIQUID: a sibling of Warp that melts the targeted layers together.
        setupStage (tbFusionLiquid, "Liquid", AlterState::kFusionLiquid);
        addLabel (lblFusionLiquidAmt, "Amount");    bindSlider (sFusionLiquidAmt,    AlterState::kFusionLiquidAmt);
        addLabel (lblFusionLiquidSmooth, "Smooth"); bindSlider (sFusionLiquidSmooth, AlterState::kFusionLiquidSmooth);
        addLabel (lblFusionLiquidDenoise, "Denoise"); bindSlider (sFusionLiquidDenoise, AlterState::kFusionLiquidDenoise);

        // TUNNEL: radially symmetric receding depth. No sub-settings — it either is
        // a tunnel or it is not.
        editorPanel.addAndMakeVisible (tbFusionTunnel);
        tbFusionTunnel.setButtonText ("Tunnel");
        tbFusionTunnel.setClickingTogglesState (true);
        tbFusionTunnel.onClick = [this, setProp]
        { setProp (AlterState::kFusionTunnel, tbFusionTunnel.getToggleState()); };

        // DETAIL: the resolution budget every layer renders at. It is a ladder and
        // not a slider because the useful answers are four, and because the cost is
        // area — halving the number halves nothing, it quarters it.
        addLabel (lblFusionDetail, "Detail");
        editorPanel.addAndMakeVisible (cbFusionDetail);
        cbFusionDetail.addItem ("Low",    1);
        cbFusionDetail.addItem ("Normal", 2);
        cbFusionDetail.addItem ("High",   3);
        cbFusionDetail.addItem ("Native", 4);
        cbFusionDetail.onChange = [this, setProp]
        { setProp (AlterState::kFusionDetail, juce::jmax (0, cbFusionDetail.getSelectedId() - 1)); };
    }

    editorPanel.addAndMakeVisible (tbSynBpmSync);
    tbSynBpmSync.setButtonText ("BPM sync");
    tbSynBpmSync.setClickingTogglesState (true);
    tbSynBpmSync.onClick = [this, setProp]
    {
        setProp (AlterState::kSynBpmSync, tbSynBpmSync.getToggleState());
        resized();   // show/hide the tempo + beat-division controls
    };

    addLabel (lblSynBpm, "BPM");
    editorPanel.addAndMakeVisible (sSynBpm);
    sSynBpm.setSliderStyle (juce::Slider::LinearHorizontal);
    sSynBpm.setRange (20.0, 400.0, 0.1);
    sSynBpm.setValue (120.0);
    sSynBpm.onValueChange = [this, setProp] { setProp (AlterState::kSynBpm, (float) sSynBpm.getValue()); };

    addLabel (lblSynBeatDiv, "Beat div");
    editorPanel.addAndMakeVisible (sSynBeatDiv);
    sSynBeatDiv.setSliderStyle (juce::Slider::IncDecButtons);
    sSynBeatDiv.setRange (0, GeometryVisual::numBeatDivisions() - 1, 1);
    sSynBeatDiv.setValue (5);
    sSynBeatDiv.textFromValueFunction = [] (double v)
    { return juce::String (GeometryVisual::labelForDivision ((int) v)); };
    sSynBeatDiv.valueFromTextFunction = [] (const juce::String& t)
    {
        for (int i = 0; i < GeometryVisual::numBeatDivisions(); ++i)
            if (t == GeometryVisual::labelForDivision (i)) return (double) i;
        return 5.0;
    };
    sSynBeatDiv.onValueChange = [this, setProp] { setProp (AlterState::kSynBeatDiv, (int) sSynBeatDiv.getValue()); };
    sSynBeatDiv.updateText();

    editorPanel.addAndMakeVisible (tbSynTone);
    tbSynTone.setButtonText ("Tone colour");
    tbSynTone.setClickingTogglesState (true);
    tbSynTone.onClick = [this, setProp]
    {
        setProp (AlterState::kColorMode, tbSynTone.getToggleState() ? 1 : 0);
        resized();   // show/hide the manual colour picker + the Mirror tone color box
    };

    editorPanel.addAndMakeVisible (tbSynTwist);
    tbSynTwist.setButtonText ("Mirror tone color");
    tbSynTwist.setClickingTogglesState (true);
    tbSynTwist.onClick = [this, setProp]
    { setProp (AlterState::kToneTwist, tbSynTwist.getToggleState()); };

    // ── Chladni Pattern editor ──────────────────────────────────────────────
    editorPanel.addAndMakeVisible (tbChladniReactive);
    tbChladniReactive.setButtonText ("Audio reactive (m,n)");
    tbChladniReactive.setClickingTogglesState (true);
    tbChladniReactive.setToggleState (true, juce::dontSendNotification);
    tbChladniReactive.onClick = [this, setProp]
    {
        const bool on = tbChladniReactive.getToggleState();
        setProp (AlterState::kChladniReactive, on);
        sChladniM.setEnabled (! on);
        sChladniN.setEnabled (! on);
        cbChladniPreset.setEnabled (! on);
    };

    // Mode presets: classic figures, selecting one switches to manual (the base mode)
    addLabel (lblChladniPreset, "Preset");
    editorPanel.addAndMakeVisible (cbChladniPreset);
    {
        static const int presets[][2] = { {1,2},{2,3},{1,4},{2,5},{3,4},{3,5},
                                          {4,5},{2,7},{3,7},{5,6},{4,9},{6,7} };
        cbChladniPreset.addItem ("Custom", 1);
        for (int i = 0; i < (int) (sizeof (presets) / sizeof (presets[0])); ++i)
            cbChladniPreset.addItem ("(" + juce::String (presets[i][0]) + ","
                                         + juce::String (presets[i][1]) + ")", i + 2);
        cbChladniPreset.setSelectedId (1, juce::dontSendNotification);
        cbChladniPreset.onChange = [this, setProp]
        {
            const int sel = cbChladniPreset.getSelectedId();
            if (sel < 2) return;
            const auto& pr = presets[sel - 2];
            setProp (AlterState::kChladniReactive, false);     // presets = manual base
            setProp (AlterState::kChladniM, pr[0]);
            setProp (AlterState::kChladniN, pr[1]);
            refreshRightEditorFromSelection();
        };
    }

    auto bindIncDec = [this, setProp](juce::Slider& sl, const juce::Identifier& prop, int def)
    {
        editorPanel.addAndMakeVisible (sl);
        sl.setSliderStyle (juce::Slider::IncDecButtons);
        sl.setRange (1, 12, 1);
        sl.setValue (def, juce::dontSendNotification);
        sl.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 30, 20);
        sl.onValueChange = [&sl, prop, setProp] { setProp (prop, (int) sl.getValue()); };
    };

    addLabel (lblChladniM, "m");
    bindIncDec (sChladniM, AlterState::kChladniM, 2);

    addLabel (lblChladniN, "n");
    bindIncDec (sChladniN, AlterState::kChladniN, 3);

    // reactive mode shift: matched (m,n) + shift, reactivity preserved
    addLabel (lblChladniShift, "Shift");
    editorPanel.addAndMakeVisible (sChladniShift);
    sChladniShift.setSliderStyle (juce::Slider::IncDecButtons);
    sChladniShift.setRange (0, 6, 1);
    sChladniShift.setValue (0, juce::dontSendNotification);
    sChladniShift.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 30, 20);
    sChladniShift.onValueChange = [this, setProp]
    { setProp (AlterState::kChladniShift, (int) sChladniShift.getValue()); };

    addLabel (lblChladniAR, "Aspect Ratio");
    bindSlider (sChladniAR, AlterState::kChladniAR);
    sChladniAR.setRange (0.25, 4.0, 0.01);
    sChladniAR.setValue (1.0, juce::dontSendNotification);
    sChladniAR.setDoubleClickReturnValue (true, 1.0);
    sChladniAR.setTextBoxStyle (juce::Slider::TextBoxRight, false, 45, 20);

    addLabel (lblChladniSharp, "Sand smooth");
    bindSlider (sChladniSharp, AlterState::kChladniSharp);
    sChladniSharp.setValue (0.5, juce::dontSendNotification);
    sChladniSharp.setDoubleClickReturnValue (true, 0.5);

    addLabel (lblChladniParticles, "Particles");
    bindSlider (sChladniParticles, AlterState::kChladniParticles);
    sChladniParticles.setRange (1000.0, 15000.0, 250.0);
    sChladniParticles.setValue (5000.0, juce::dontSendNotification);
    sChladniParticles.setDoubleClickReturnValue (true, 5000.0);
    sChladniParticles.setTextBoxStyle (juce::Slider::TextBoxRight, false, 55, 20);
    sChladniParticles.onValueChange = [this, setProp]
    { setProp (AlterState::kChladniParticles, (int) sChladniParticles.getValue()); };

    // colour follows the dominant tone (12-hue wheel, C=red ... B=rose)
    editorPanel.addAndMakeVisible (tbChladniToneColor);
    tbChladniToneColor.setButtonText ("Color by tone");
    tbChladniToneColor.setClickingTogglesState (true);
    tbChladniToneColor.onClick = [this, setProp]
    {
        setProp (AlterState::kColorMode, tbChladniToneColor.getToggleState() ? 1 : 0);
        resized();   // show/hide the sand colour picker + the Mirror tone color box
    };

    editorPanel.addAndMakeVisible (tbChladniTwist);
    tbChladniTwist.setButtonText ("Mirror tone color");
    tbChladniTwist.setClickingTogglesState (true);
    tbChladniTwist.onClick = [this, setProp]
    { setProp (AlterState::kToneTwist, tbChladniTwist.getToggleState()); };

    addLabel (lblChladniMaterial, "Material");

    editorPanel.addAndMakeVisible (cbChladniMaterial);
    cbChladniMaterial.addItem ("Aluminium  (70 GPa)",   1);
    cbChladniMaterial.addItem ("Steel      (200 GPa)",  2);
    cbChladniMaterial.addItem ("Glass      (70 GPa)",   3);
    cbChladniMaterial.addItem ("Acrylic    (3.2 GPa)",  4);
    cbChladniMaterial.setSelectedId (1, juce::dontSendNotification);
    cbChladniMaterial.onChange = [this, setProp]
    { setProp (AlterState::kChladniMaterial, cbChladniMaterial.getSelectedId() - 1); };

    // ── ToneAnalyzer editor ─────────────────────────────────────────────────
    addLabel (lblToneSens, "Sensitivity");
    bindSlider (sToneSens, AlterState::kToneSens);
    sToneSens.setValue (0.5, juce::dontSendNotification);
    sToneSens.setDoubleClickReturnValue (true, 0.5);

    // tuner mode: accurate monophonic pitch (MPM) + needle for tuning an instrument
    editorPanel.addAndMakeVisible (tbToneTuner);
    tbToneTuner.setButtonText ("Tuner mode");
    tbToneTuner.setClickingTogglesState (true);
    tbToneTuner.onClick = [this, setProp]
    { setProp (AlterState::kToneTuner, tbToneTuner.getToggleState()); };

    // ── Spectrogram editor ──────────────────────────────────────────────────
    addLabel (lblSpectroSmooth, "Smooth");
    bindSlider (sSpectroSmooth, AlterState::kSmooth);
    sSpectroSmooth.setValue (0.35, juce::dontSendNotification);
    sSpectroSmooth.setDoubleClickReturnValue (true, 0.35);

    addLabel (lblSpectroWin, "Window");
    bindSlider (sSpectroWin, AlterState::kSpectroWindow);
    sSpectroWin.setRange (1.0, 120.0, 1.0);
    sSpectroWin.setValue (30.0, juce::dontSendNotification);
    sSpectroWin.setDoubleClickReturnValue (true, 30.0);
    sSpectroWin.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    sSpectroWin.setTextValueSuffix (" s");

    addLabel (lblSpectroFill, "Row fill");
    bindSlider (sSpectroFill, AlterState::kSpectroLineFill);   // 0 = thin line .. 1 = full row
    sSpectroFill.setDoubleClickReturnValue (true, 0.0);


    editorPanel.addAndMakeVisible (tbSpectroMirror);
    tbSpectroMirror.setButtonText ("Mirror axis");
    tbSpectroMirror.setClickingTogglesState (true);
    tbSpectroMirror.onClick = [this, setProp]
    { setProp (AlterState::kSpectroMirror, tbSpectroMirror.getToggleState()); };

    editorPanel.addAndMakeVisible (tbSpectroConstantQ);
    tbSpectroConstantQ.setButtonText ("Constant-Q");
    tbSpectroConstantQ.setClickingTogglesState (true);
    tbSpectroConstantQ.setTooltip ("Aggregate FFT bins per log-frequency row (fills low bands with "
                                   "true per-band energy instead of a thin line).");
    tbSpectroConstantQ.onClick = [this, setProp]
    {
        const bool on = tbSpectroConstantQ.getToggleState();
        setProp (AlterState::kConstantQ, on);
        if (on && tbSpectroReassign.getToggleState())   // mutually exclusive with Enhanced freq
        {
            tbSpectroReassign.setToggleState (false, juce::dontSendNotification);
            setProp (AlterState::kSpectroReassign, false);
            resized();
        }
    };

    editorPanel.addAndMakeVisible (tbSpectroReassign);
    tbSpectroReassign.setButtonText ("Enhanced freq");
    tbSpectroReassign.setClickingTogglesState (true);
    tbSpectroReassign.setTooltip ("Spectral reassignment: paints each bin at its true instantaneous "
                                  "frequency, collapsing tones into razor-thin lines (Wave Candy style).");
    tbSpectroReassign.onClick = [this, setProp]
    {
        const bool on = tbSpectroReassign.getToggleState();
        setProp (AlterState::kSpectroReassign, on);
        if (on && tbSpectroConstantQ.getToggleState())   // mutually exclusive with Constant-Q
        {
            tbSpectroConstantQ.setToggleState (false, juce::dontSendNotification);
            setProp (AlterState::kConstantQ, false);
        }
        resized();   // show/hide the Continuity slider
    };

    // Spectrogram colour: a THREE-way choice, so a combo rather than the old
    // "Custom color" checkbox. The ids are the stored kColorMode values + 1, which
    // keeps every existing preset valid: 0 (theme) and 1 (custom) mean exactly what
    // they did before and 2 is the new one.
    addLabel (lblSpectroColorMode, "Color");
    editorPanel.addAndMakeVisible (cbSpectroColorMode);
    cbSpectroColorMode.addItem ("Theme heat map",  1);
    cbSpectroColorMode.addItem ("Custom gradient", 2);
    cbSpectroColorMode.addItem ("By tone",         3);
    cbSpectroColorMode.setSelectedId (1, juce::dontSendNotification);
    cbSpectroColorMode.onChange = [this, setProp]
    {
        setProp (AlterState::kColorMode, juce::jlimit (0, 2, cbSpectroColorMode.getSelectedId() - 1));
        resized();   // show/hide the colour pickers and the tone rows
    };

    // ── Shared colour-by-tone controls (Spectrum / Oscilloscope / Stereoscope /
    //    Spectrogram) ──────────────────────────────────────────────────────────
    editorPanel.addAndMakeVisible (tbToneColor);
    tbToneColor.setButtonText ("Color by tone");
    tbToneColor.setClickingTogglesState (true);
    tbToneColor.onClick = [this, setProp]
    {
        setProp (AlterState::kColorMode, tbToneColor.getToggleState() ? 1 : 0);
        resized();   // swaps the colour picker for the twist row
    };

    editorPanel.addAndMakeVisible (tbToneTwist);
    tbToneTwist.setButtonText ("Mirror tone color");
    tbToneTwist.setClickingTogglesState (true);
    tbToneTwist.onClick = [this, setProp]
    { setProp (AlterState::kToneTwist, tbToneTwist.getToggleState()); };

    addLabel (lblToneSmooth, "Tone smooth");
    bindSlider (sToneSmooth, AlterState::kToneSmooth);

    // ── Stereoscope editor ──────────────────────────────────────────────────
    addLabel (lblStereoMode, "Mode");
    editorPanel.addAndMakeVisible (cbStereoMode);
    cbStereoMode.addItem ("Particles",   1);
    cbStereoMode.addItem ("Goniometer",  2);
    cbStereoMode.addItem ("Polar",       3);
    cbStereoMode.addItem ("Correlation", 4);
    cbStereoMode.addItem ("Correlometer", 5);
    cbStereoMode.onChange = [this, setProp]
    {
        setProp (AlterState::kStereoMode, juce::jlimit (0, 4, cbStereoMode.getSelectedId() - 1));
        resized();   // sensitivity row visibility depends on the mode
    };

    addLabel (lblStereoSmooth, "Smooth");
    bindSlider (sStereoSmooth, AlterState::kSmooth);

    editorPanel.addAndMakeVisible (tbStereoParticles);
    tbStereoParticles.setButtonText ("Particles");
    tbStereoParticles.setClickingTogglesState (true);
    tbStereoParticles.onClick = [this, setProp]
    {
        setProp (AlterState::kStereoParticles, tbStereoParticles.getToggleState());
        resized();   // show/hide density depending on the toggle
    };

    addLabel (lblStereoDensity, "Density");
    bindSlider (sStereoDensity, AlterState::kStereoDensity);

    addLabel (lblStereoBright, "Brightness");
    bindSlider (sStereoBright, AlterState::kStereoBright);

    // Two separate controls and not one, because they are not one thing: the beam
    // is a stroked path and the cloud is a field of dots, and a mode can show
    // either. Both are NEUTRAL AT 0.5 — dead centre is exactly what the module
    // drew before they existed, so the sliders only ever mean "finer than stock"
    // or "heavier than stock".
    addLabel (lblStereoLineW, "Line width");
    bindSlider (sStereoLineW, AlterState::kStereoLineW);
    sStereoLineW.setTooltip ("Thickness of the Goniometer's line trace. "
                             "Centre = the standard 1.6 px; left = finer.");

    addLabel (lblStereoPointSize, "Point size");
    bindSlider (sStereoPointSize, AlterState::kStereoPointSize);
    sStereoPointSize.setTooltip ("Size of the dots in Particles, the Goniometer's "
                                 "particle cloud and Polar. Centre = standard; left = finer.");

    editorPanel.addAndMakeVisible (tbStereoCtrlBins);
    tbStereoCtrlBins.setButtonText ("Use controller bins");
    tbStereoCtrlBins.setClickingTogglesState (true);
    tbStereoCtrlBins.setTooltip ("Correlometer resolution: follow the global Max-bins setting "
                                 "(top right) instead of the default 512 bins.");
    tbStereoCtrlBins.onClick = [this, setProp]
    {
        setProp (AlterState::kStereoCtrlBins, tbStereoCtrlBins.getToggleState());
    };

    // ── Geometry editor (shared params reuse the Synesthesia sliders) ─────────
    addLabel (lblGeoTri, "Triangle");
    bindSlider (sGeoTri, AlterState::kGeoTri);

    addLabel (lblGeoSquare, "Square");
    bindSlider (sGeoSquare, AlterState::kGeoSquare);

    addLabel (lblGeoCircle, "Circle");
    bindSlider (sGeoCircle, AlterState::kGeoCircle);

    addLabel (lblGeoComplexity, "Complexity");
    editorPanel.addAndMakeVisible (sGeoComplexity);
    sGeoComplexity.setSliderStyle (juce::Slider::LinearHorizontal);
    sGeoComplexity.setRange (1, GeometryVisual::kMaxComplexity, 1);
    sGeoComplexity.setValue (12);
    sGeoComplexity.onValueChange = [this, setProp] { setProp (AlterState::kGeoComplexity, (int) sGeoComplexity.getValue()); };

    addLabel (lblGeoRandom, "Random");
    bindSlider (sGeoRandom, AlterState::kGeoRandom);

    addLabel (lblGeoReact, "React");
    bindSlider (sGeoReact, AlterState::kGeoReact);

    addLabel (lblGeoDepth, "Depth");
    bindSlider (sGeoDepth, AlterState::kGeoDepth);

    addLabel (lblGeoTunnel, "Tunnel");
    bindSlider (sGeoTunnel, AlterState::kGeoTunnel);

    addLabel (lblGeoAperture, "Aperture");
    bindSlider (sGeoAperture, AlterState::kGeoAperture);

    addLabel (lblGeoGlobalRot, "Module rot");
    editorPanel.addAndMakeVisible (sGeoGlobalRot);
    sGeoGlobalRot.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    sGeoGlobalRot.setRange (-360.0, 360.0, 1.0);   // bipolar: 0 = still, +/- = spin each way
    sGeoGlobalRot.setValue (0.0);
    sGeoGlobalRot.setDoubleClickReturnValue (true, 0.0);
    sGeoGlobalRot.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    sGeoGlobalRot.setPopupDisplayEnabled (true, false, this);
    sGeoGlobalRot.onValueChange = [this, setProp] { setProp (AlterState::kGeoGlobalRot, (float) sGeoGlobalRot.getValue()); };

    editorPanel.addAndMakeVisible (tbGeoBpmSync);
    tbGeoBpmSync.setButtonText ("BPM sync");
    tbGeoBpmSync.setClickingTogglesState (true);
    tbGeoBpmSync.onClick = [this, setProp]
    {
        setProp (AlterState::kGeoBpmSync, tbGeoBpmSync.getToggleState());
        resized();   // show/hide the tempo + beat-division controls
    };

    addLabel (lblGeoBpm, "BPM");
    editorPanel.addAndMakeVisible (sGeoBpm);
    sGeoBpm.setSliderStyle (juce::Slider::LinearHorizontal);
    sGeoBpm.setRange (20.0, 400.0, 0.1);
    sGeoBpm.setValue (100.0);
    sGeoBpm.onValueChange = [this, setProp] { setProp (AlterState::kBpm, (float) sGeoBpm.getValue()); };

    addLabel (lblGeoBeatDiv, "Beat div");
    editorPanel.addAndMakeVisible (sGeoBeatDiv);
    sGeoBeatDiv.setSliderStyle (juce::Slider::IncDecButtons);
    sGeoBeatDiv.setRange (0, GeometryVisual::numBeatDivisions() - 1, 1);
    sGeoBeatDiv.setValue (5);
    sGeoBeatDiv.textFromValueFunction = [] (double v)
    { return juce::String (GeometryVisual::labelForDivision ((int) v)); };
    sGeoBeatDiv.valueFromTextFunction = [] (const juce::String& t)
    {
        for (int i = 0; i < GeometryVisual::numBeatDivisions(); ++i)
            if (t == GeometryVisual::labelForDivision (i)) return (double) i;
        return 5.0;
    };
    sGeoBeatDiv.onValueChange = [this, setProp] { setProp (AlterState::kGeoBeatDiv, (int) sGeoBeatDiv.getValue()); };
    sGeoBeatDiv.updateText();

    editorPanel.addAndMakeVisible (tbGeoTone);
    tbGeoTone.setButtonText ("Tone colour");
    tbGeoTone.setClickingTogglesState (true);
    tbGeoTone.onClick = [this, setProp]
    {
        setProp (AlterState::kColorMode, tbGeoTone.getToggleState() ? 1 : 0);
        resized();   // show/hide the base colour picker + the Mirror tone color box
    };

    editorPanel.addAndMakeVisible (tbGeoTwist);
    tbGeoTwist.setButtonText ("Mirror tone color");
    tbGeoTwist.setClickingTogglesState (true);
    tbGeoTwist.onClick = [this, setProp]
    { setProp (AlterState::kToneTwist, tbGeoTwist.getToggleState()); };

    // ── Spectrum extended ───────────────────────────────────────────────────
    editorPanel.addAndMakeVisible (tbSpecMeasurement);
    tbSpecMeasurement.setButtonText ("Measurement mode");
    tbSpecMeasurement.setClickingTogglesState (true);
    tbSpecMeasurement.onClick = [this, setProp]
    { setProp (AlterState::kMeasurementMode, tbSpecMeasurement.getToggleState() ? 1 : 0); };

    editorPanel.addAndMakeVisible (tbSpecPeakHold);
    tbSpecPeakHold.setButtonText ("Peak hold");
    tbSpecPeakHold.setClickingTogglesState (true);
    tbSpecPeakHold.onClick = [this, setProp]
    { setProp (AlterState::kPeakHold, tbSpecPeakHold.getToggleState()); };

    editorPanel.addAndMakeVisible (tbSpecConstantQ);
    tbSpecConstantQ.setButtonText ("Constant-Q");
    tbSpecConstantQ.setClickingTogglesState (true);
    tbSpecConstantQ.setTooltip ("Aggregate FFT bins per log-frequency band so narrow peaks "
                                "are never sampled-over (clean musical bands). Best with high Max bins.");
    tbSpecConstantQ.onClick = [this, setProp]
    {
        setProp (AlterState::kConstantQ, tbSpecConstantQ.getToggleState());
        resized();   // hide/show Measurement / Stereo / Peak-hold (they don't apply in Constant-Q)
    };

    addLabel (lblSpecPsycho, "Curve");

    editorPanel.addAndMakeVisible (cbSpecPsycho);
    cbSpecPsycho.addItem ("Flat",     1);
    cbSpecPsycho.addItem ("A-weight", 2);
    cbSpecPsycho.addItem ("Fletcher-Munson", 3);   // ISO 226 equal-loudness
    cbSpecPsycho.setSelectedId (1, juce::dontSendNotification);
    cbSpecPsycho.onChange = [this, setProp]
    {
        const int curve = cbSpecPsycho.getSelectedId() - 1; // 0=Flat,1=A,2=ISO226
        setProp (AlterState::kPsychoCurve, curve);
        setProp (AlterState::kAWeight, curve == 1);   // legacy sync
        lblSpecPhon.setVisible (curve == 2);
        sSpecPhon.setVisible (curve == 2);
        resized();   // reflow rows: phon slider appears/disappears (ISO226 only)
    };

    addLabel (lblSpecPhon, "Phon");
    lblSpecPhon.setVisible (false);

    bindSlider (sSpecPhon, AlterState::kPhon);
    sSpecPhon.setRange (20.0, 100.0, 1.0);
    sSpecPhon.setValue (60.0);
    sSpecPhon.setVisible (false);
    sSpecPhon.onValueChange = [this, setProp] { setProp (AlterState::kPhon, (int) sSpecPhon.getValue()); };

    // Reference tonal-balance curve
    addLabel (lblSpecRef, "Reference");

    editorPanel.addAndMakeVisible (cbSpecRef);
    cbSpecRef.addItem ("Off",          1);
    cbSpecRef.addItem ("EDM",          2);
    cbSpecRef.addItem ("Bass music",   3);
    cbSpecRef.addItem ("House/Techno", 4);
    cbSpecRef.addItem ("Hip-Hop",      5);
    cbSpecRef.addItem ("Pop",          6);
    cbSpecRef.addItem ("Rock",         7);
    cbSpecRef.setSelectedId (1, juce::dontSendNotification);
    cbSpecRef.onChange = [this, setProp]
    { setProp (AlterState::kSpecReference, cbSpecRef.getSelectedId() - 1); };

    // Stereo: overlaid L/R spectra
    editorPanel.addAndMakeVisible (tbSpecStereo);
    tbSpecStereo.setButtonText ("Stereo (L+R)");
    tbSpecStereo.setClickingTogglesState (true);
    tbSpecStereo.onClick = [this, setProp]
    {
        setProp (AlterState::kSpecStereo, tbSpecStereo.getToggleState());
        resized();   // show/hide L/R colour selector
    };

    // R-curve colour relationship
    addLabel (lblSpecLrColor, "R colour");
    editorPanel.addAndMakeVisible (cbSpecLrColor);
    cbSpecLrColor.addItem ("Complementary", 1);
    cbSpecLrColor.addItem ("Analogous",     2);
    cbSpecLrColor.setSelectedId (1, juce::dontSendNotification);
    cbSpecLrColor.onChange = [this, setProp]
    { setProp (AlterState::kSpecLrColor, cbSpecLrColor.getSelectedId() - 1); };

    // (Spectrum 'Bins' selector removed — the display always uses the full
    //  resolution the source provides; the GLOBAL FFT setting controls it.)

    // ── FOOTER ──────────────────────────────────────────────────────────────
    // Global visual theme (backgrounds of HUD + all modules)
    addAndMakeVisible (lblTheme);
    lblTheme.setText ("Theme", juce::dontSendNotification);
    lblTheme.setJustificationType (juce::Justification::centred);   // caption sits ABOVE the button again
    lblTheme.setColour (juce::Label::textColourId, AlterTheme::textDim);

    addAndMakeVisible (btnTheme);
    btnTheme.setTooltip ("Visual theme. In Custom, two colour swatches appear right next to it: "
                         "the 1st is the accent, the 2nd is the background.");
    btnTheme.onClick = [this] { showThemeMenu(); };
    updateThemeButtonText();

    // Custom-theme colour swatches — small colour squares that live in the TOP bar and
    // are only shown while the Custom theme is active (so they never crowd the footer).
    auto setupSwatch = [this] (juce::TextButton& b, const juce::Identifier& prop,
                               const char* tip, juce::Colour fallback)
    {
        addAndMakeVisible (b);
        b.setButtonText ("");
        b.setTooltip (tip);
        b.onClick = [this, prop, fallback] { openThemeColour (prop, fallback); };
    };
    setupSwatch (btnThemeC1, AlterState::kThemeColor1,
                 "Custom theme: accent colour (text + highlights)", AlterTheme::pictonBlue);
    setupSwatch (btnThemeC2, AlterState::kThemeColor2,
                 "Custom theme: background colour", AlterTheme::electricViolet);
    refreshThemeSwatches();

    addAndMakeVisible (alwaysOnTop);
    alwaysOnTop.setButtonText ("Always on top");
    alwaysOnTop.setClickingTogglesState (true);
    alwaysOnTop.setToggleState (settings.controllerAlwaysOnTop(), juce::dontSendNotification);
    alwaysOnTop.onClick = [this]
    {
        settings.setControllerAlwaysOnTop (alwaysOnTop.getToggleState());
        if (onAlwaysOnTopChanged) onAlwaysOnTopChanged (alwaysOnTop.getToggleState());
    };

    // Hold: freeze the whole HUD (all modules hold their last frame)
    addAndMakeVisible (btnHold);
    btnHold.setButtonText ("Hold");
    btnHold.setClickingTogglesState (true);
    btnHold.setTooltip ("Freeze the HUD: every module holds its last frame until released.");
    btnHold.onClick = [this] { AlterTheme::hudFrozen.store (btnHold.getToggleState()); };

    // Hide info: globally hide the per-module top-right info/parameter overlays
    addAndMakeVisible (btnHideInfo);
    btnHideInfo.setButtonText ("Hide info");
    btnHideInfo.setClickingTogglesState (true);
    btnHideInfo.setTooltip ("Hide the parameter readout shown top-right on every module.");
    btnHideInfo.onClick = [this] { AlterTheme::hudInfoHidden.store (btnHideInfo.getToggleState()); };

    // Global FFT resolution: max bins the plugins compute/send (caps the Spectrum
    // module's own bins choice). Higher = finer low-frequency detail, slower response.
    addAndMakeVisible (cbMaxBins);
    cbMaxBins.addItem ("512 bins",   512);
    cbMaxBins.addItem ("1024 bins",  1024);
    cbMaxBins.addItem ("2048 bins",  2048);
    cbMaxBins.addItem ("4096 bins",  4096);
    cbMaxBins.addItem ("8192 bins",  8192);
    cbMaxBins.addItem ("Constant-Q", 1);   // sentinel: global multi-resolution mode
    cbMaxBins.setSelectedId (settings.globalCqt() ? 1 : settings.maxFftBins(), juce::dontSendNotification);
    cbMaxBins.setTooltip ("Global FFT resolution (512 = lightest for weak machines, 8192 = sharpest "
                          "low-end). 'Constant-Q' = true multi-resolution for Spectrum/Spectrogram.");
    cbMaxBins.onChange = [this]
    {
        const int id = cbMaxBins.getSelectedId();
        if (id == 1)
            settings.setGlobalCqt (true);
        else
        {
            settings.setGlobalCqt (false);
            settings.setMaxFftBins (id);
        }
    };
    // (Sub-bin interpolation is now always-on inside the tone modules — no toggle.)

    addAndMakeVisible (lblMaxBins);
    lblMaxBins.setText ("FFT bins", juce::dontSendNotification);
    lblMaxBins.setJustificationType (juce::Justification::centred);

    // Brand logotype (footer, centred bottom strip) — created + recoloured per theme.
    updateLogoColour();

    addAndMakeVisible (lblAudioMode);
    lblAudioMode.setText ("Audio Input", juce::dontSendNotification);
    lblAudioMode.setJustificationType (juce::Justification::centred);

    addAndMakeVisible (cbAudioMode);
    cbAudioMode.addItem ("VST Plugin", 1);

    #if JUCE_WINDOWS
        cbAudioMode.addItem ("System Audio", 2);
        cbAudioMode.setTooltip ("Choose audio input source:\n"
                               "- VST Plugin: Receive from DAW (Ableton, FL Studio, etc.)\n"
                               "- System Audio: Capture from Windows (Spotify, YouTube, etc.)");
    #elif JUCE_MAC
        cbAudioMode.addItem ("System Audio", 2);
        cbAudioMode.setTooltip ("Choose audio input source:\n"
                               "- VST Plugin: Receive from DAW (Logic, Ableton, etc.)\n"
                               "- System Audio: Capture system sound via ScreenCaptureKit "
                               "(needs macOS 13+ and Screen Recording permission)");
    #else
        cbAudioMode.addItem ("System Audio (Windows/macOS only)", 2);
        cbAudioMode.setTooltip ("Choose audio input source:\n"
                               "- VST Plugin: Receive from DAW (works on all platforms)\n"
                               "- System Audio: Not available on Linux");
    #endif

    cbAudioMode.setSelectedId (1, juce::dontSendNotification); // Default: VST Plugin

    cbAudioMode.onChange = [this]
    {
        const int mode = cbAudioMode.getSelectedId();

        #if ! JUCE_WINDOWS && ! JUCE_MAC
            if (mode == 2)
            {
                juce::NativeMessageBox::showMessageBoxAsync (
                    juce::MessageBoxIconType::WarningIcon,
                    "Feature Not Available",
                    "System Audio Capture is only available on Windows and macOS.\n\n"
                    "On Linux, please use VST Plugin mode instead.",
                    nullptr);
                cbAudioMode.setSelectedId (1, juce::dontSendNotification);
                return;
            }
        #endif

        if (onAudioModeChangedInternal)
            onAudioModeChangedInternal (mode);

        resized();  // re-layout footer (gain knob / instance picker)
    };

    // VST plugin instance picker (next to the Audio Input combo)
    addAndMakeVisible (lblInstance);
    lblInstance.setText ("Instance", juce::dontSendNotification);
    lblInstance.setJustificationType (juce::Justification::centred);

    addAndMakeVisible (cbPluginInstance);
    cbPluginInstance.setTooltip ("Choose which ALTER Listener plugin instance to visualise.\n"
                                 "Auto = first active instance.");
    cbPluginInstance.addItem ("Auto", 1);
    cbPluginInstance.setSelectedId (1, juce::dontSendNotification);
    cbPluginInstance.onChange = [this]
    {
        const int sel = cbPluginInstance.getSelectedId();
        const juce::uint32 instId = (sel >= 2 && sel - 2 < (int) cachedInstances.size())
                                  ? cachedInstances[(size_t)(sel - 2)].id : 0;
        settings.getTree().setProperty (AlterState::kPluginInstance, (juce::int64) instId, nullptr);
        if (onPluginInstanceSelected)
            onPluginInstanceSelected (instId);
    };

    addAndMakeVisible (btnQuit);
    btnQuit.onClick = [this]
    {
        currentInfoWindow.reset();
        juce::JUCEApplicationBase::quit();
    };

    addAndMakeVisible (btnClose);
    btnClose.onClick = [this] { if (onCloseRequested) onCloseRequested(); };

    // ── Color UI ────────────────────────────────────────────────────────────
    auto setupColourButton = [this](juce::Label& lbl, const juce::String& text,
                                    juce::Label& preview, juce::TextButton& btn,
                                    const juce::Identifier& prop)
    {
        editorPanel.addAndMakeVisible (lbl);
        lbl.setText (text, juce::dontSendNotification);
        editorPanel.addAndMakeVisible (preview);
        preview.setOpaque (true);
        preview.setColour (juce::Label::backgroundColourId, AlterTheme::pictonBlue);
        editorPanel.addAndMakeVisible (btn);
        btn.onClick = [this, &preview, &btn, prop]
        { launchColourPicker (prop, preview, btn); };
    };

    setupColourButton (lblColor,  "Color",   lblColorPreview,  btnColor,  AlterState::kColor);
    setupColourButton (lblColor2, "Color 2", lblColor2Preview, btnColor2, AlterState::kColor2);

    // Color mode selector (Standard / Custom)
    addLabel (lblColorMode, "Color Mode");

    editorPanel.addAndMakeVisible (cbColorMode);
    cbColorMode.addItem ("Standard", 1);
    cbColorMode.addItem ("Custom gradient", 2);
    cbColorMode.addItem ("Custom complementary", 3);
    cbColorMode.setSelectedId (1, juce::dontSendNotification);
    cbColorMode.onChange = [this, setProp]
    {
        const int mode = cbColorMode.getSelectedId() - 1;
        setProp (AlterState::kColorMode, mode);
        setProp (AlterState::kCustomColorMode, mode != 0);  // legacy support
        resized();
    };

    // ── Audio Meter: colour by tone + the shade its loudness zones step through ──
    editorPanel.addAndMakeVisible (tbMeterToneColor);
    tbMeterToneColor.setButtonText ("Color by tone");
    tbMeterToneColor.setClickingTogglesState (true);
    tbMeterToneColor.onClick = [this, setProp]
    {
        setProp (AlterState::kMeterToneColor, tbMeterToneColor.getToggleState());
        resized();   // swaps the Color Mode combo for the tone rows
    };

    addLabel (lblMeterToneShade, "Zones");
    editorPanel.addAndMakeVisible (cbMeterToneShade);
    // A meter is a LADDER of colours, and this is the only choice tone mode has to
    // make that the other modules do not: where the hot zones sit relative to the
    // note. Gradient keeps the note exactly the note and only dims; the other two
    // are the same two hue distances the Spectrum's L/R channels use, so the words
    // mean the same thing in both places.
    cbMeterToneShade.addItem ("Gradient",      1);
    cbMeterToneShade.addItem ("Complementary", 2);
    cbMeterToneShade.setSelectedId (1, juce::dontSendNotification);
    cbMeterToneShade.onChange = [this, setProp]
    { setProp (AlterState::kMeterToneShade, juce::jlimit (0, 1, cbMeterToneShade.getSelectedId() - 1)); };

    editorPanel.addAndMakeVisible (tbMeterTwoBars);
    tbMeterTwoBars.setButtonText ("Peak hold bar");
    tbMeterTwoBars.setClickingTogglesState (true);
    tbMeterTwoBars.onClick = [this, setProp]
    { setProp (AlterState::kMeterTwoBars, tbMeterTwoBars.getToggleState()); };

    // ── Shared by Level history and the Oscilloscope ────────────────────────
    editorPanel.addAndMakeVisible (tbClipZone);
    tbClipZone.setButtonText ("Clipping zone");
    tbClipZone.setClickingTogglesState (true);
    tbClipZone.onClick = [this, setProp]
    { setProp (AlterState::kClipZone, tbClipZone.getToggleState()); };

    addLabel (lblLrColor, "L/R color");
    editorPanel.addAndMakeVisible (cbLrColor);
    cbLrColor.addItem ("Complementary", 1);
    cbLrColor.addItem ("Analogous",     2);
    cbLrColor.setSelectedId (1, juce::dontSendNotification);
    cbLrColor.onChange = [this, setProp]
    { setProp (AlterState::kOscLrColor, juce::jlimit (0, 1, cbLrColor.getSelectedId() - 1)); };

    // HUD block (layer) selector
    addLabel (lblModuleLayer, "Block");
    editorPanel.addAndMakeVisible (cbModuleLayer);
    cbModuleLayer.onChange = [this, setProp]
    {
        if (cbModuleLayer.getSelectedId() > 0)
        {
            setProp (AlterState::kLayer, cbModuleLayer.getSelectedId() - 1);
            settings.sortPanelsByLayer();     // keep the block bubbles contiguous
            panelList.updateContent();
            panelList.repaint();
        }
    };

    // Module rotation button (cycles through 0°, 90°, 180°, 270°)
    addLabel (lblModuleRotation, "Rotate");

    editorPanel.addAndMakeVisible (btnRotate);
    btnRotate.onClick = [this]
    {
        if (selectedPanelId < 0) return;
        auto panel = settings.getPanelById (selectedPanelId);
        if (! panel.isValid()) return;

        // OpenGL components don't support rotation in JUCE — unless the module is
        // a Fusion LAYER, in which case it is never drawn as a component at all
        // and the fusion shader rotates its finished frame instead.
        if (panel.getProperty (AlterState::kType).toString() == "synesthesia"
            && (int) panel.getProperty (AlterState::kFusionHost, 0) <= 0)
        {
            juce::AlertWindow::showMessageBoxAsync (
                juce::AlertWindow::InfoIcon,
                "Rotation Not Supported",
                "Rotation is not supported for Synesthesia module (OpenGL rendering).",
                "OK");
            return;
        }

        const int next = (((int) panel.getProperty (AlterState::kRotationAngle, 0)) + 1) % 4;
        panel.setProperty (AlterState::kRotationAngle, next, nullptr);

        static const char* rotationText[] = { "0 deg", "90 deg", "180 deg", "270 deg" };
        btnRotate.setButtonText (rotationText[next]);
    };

    // MIRROR, beside Rotate because it answers the same question: which way round
    // is this module. Spectrum flips the frequency MAPPING rather than the drawn
    // picture, so the Hz labels move with their grid lines and still read
    // forwards — a spectrum with backwards numbers would be worse than one that
    // simply runs the wrong way.
    editorPanel.addAndMakeVisible (tbSpecMirror);
    tbSpecMirror.setClickingTogglesState (true);
    tbSpecMirror.onClick = [this, setProp]
    { setProp (AlterState::kSpecMirror, tbSpecMirror.getToggleState()); };

    // Double-click to reset sliders to default value (VST plugin standard)
    sRmsSmooth.setDoubleClickReturnValue (true, 0.5);
    sSpecSmooth.setDoubleClickReturnValue (true, 0.5);
    sOscSmooth.setDoubleClickReturnValue (true, 0.5);
    sSynSmooth.setDoubleClickReturnValue (true, 0.15);
    sToneSmooth.setValue (0.5, juce::dontSendNotification);   // matches the AlterState default
    sToneSmooth.setDoubleClickReturnValue (true, 0.5);
    sSynGhost.setDoubleClickReturnValue (true, 0.0);
    sZoom.setDoubleClickReturnValue (true, 1.0);
    sRotation.setDoubleClickReturnValue (true, 0.0);
    sSymmetry.setDoubleClickReturnValue (true, 1.0);
    sSaturation.setDoubleClickReturnValue (true, 1.0);
    sBloom.setDoubleClickReturnValue (true, 0.0);
    sSpeed.setDoubleClickReturnValue (true, 1.0);

    sRotation.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    sRotation.setPopupDisplayEnabled (true, false, this);

    // Gain knob for System Audio mode
    addAndMakeVisible (lblGain);
    lblGain.setText ("Gain", juce::dontSendNotification);
    lblGain.setJustificationType (juce::Justification::centred);
    lblGain.setVisible (false);

    addAndMakeVisible (sGain);
    sGain.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    sGain.setRange (0.0, 20.0, 0.5);
    sGain.setValue (0.0, juce::dontSendNotification);
    sGain.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    sGain.setPopupDisplayEnabled (true, false, this);
    sGain.setTextValueSuffix (" dB");
    sGain.setDoubleClickReturnValue (true, 0.0);
    sGain.onValueChange = [this]
    {
        settings.getTree().setProperty (AlterState::kSystemGain, (float) sGain.getValue(), nullptr);
    };
    sGain.setVisible (false);

    // Setup scrollable editor viewport for right panel
    editorViewport.setViewedComponent (&editorPanel, false);
    editorViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (editorViewport);

    // One rule for every knob in the module editor, applied in one place instead
    // of 60 constructor lines: double-click the track to get the default back,
    // click the number to type an exact value. Run LAST, while every slider still
    // holds the value it was constructed with — that value IS its default, and
    // nothing has selected a module yet to overwrite it.
    finaliseSliders (editorPanel, true);

    // The chrome sliders (system gain and friends) live in tight bars where a
    // text box would not fit, so they only get the double-click default.
    finaliseSliders (*this, false);

    // initial state
    refreshRightEditorFromSelection();
    resized();
}

void ControllerContent::finaliseSliders (juce::Component& parent, bool addMissingTextBoxes)
{
    for (auto* child : parent.getChildren())
    {
        if (auto* s = dynamic_cast<juce::Slider*> (child))
        {
            // Sliders that already named their default (Osc Zoom 0.42, Chladni
            // particles 5000, ...) keep it; the rest fall back to where they sit
            // right now, which is what their setValue() in the constructor put there.
            if (! s->isDoubleClickReturnEnabled())
                s->setDoubleClickReturnValue (true, s->getValue());

            // A slider with no readout cannot be typed into. The compact rows
            // (Fusion layers, module rotation) were drawn that way and leaned on
            // the drag popup instead — now they carry a real, editable number.
            if (addMissingTextBoxes && s->getTextBoxPosition() == juce::Slider::NoTextBox)
                s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 46, 18);

            continue;   // do NOT descend: a Slider's own value box is a child of it
        }

        finaliseSliders (*child, addMissingTextBoxes);
    }
}

ControllerContent::~ControllerContent()
{
    settings.getTree().removeListener (this);
    settings.onHistoryRestored = nullptr;      // we're going away — unhook the undo refresh
    AlterTheme::hudFrozen.store (false);       // never leave the HUD stuck frozen
    AlterTheme::hudInfoHidden.store (false);   // restore the module info overlays
}

void ControllerContent::valueTreePropertyChanged (juce::ValueTree& tree,
                                                  const juce::Identifier&)
{
    // Our own edit: the widget that caused it is already showing the value, so
    // re-reading the panel would only rebuild the editor under the user's mouse.
    // See the guard in setProp.
    if (writingOwnProperty)
        return;

    if (selectedPanelId >= 0 && (int) tree.getProperty (AlterState::kId, -1) == selectedPanelId)
        pendingEditorRefresh = true;
}

void ControllerContent::timerCallback()
{
    // ── FAST part: every tick (20 Hz) ────────────────────────────────────────
    //
    // Pull in values changed from outside the controller. Skipped while the mouse
    // is down so a refresh can never fight a slider the user is dragging.
    if (pendingEditorRefresh
        && ! juce::Desktop::getInstance().getMainMouseSource().isDragging()
        && ! juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
    {
        pendingEditorRefresh = false;
        refreshRightEditorFromSelection();
        resized();
    }

    // ── SLOW part: still ~2 Hz ───────────────────────────────────────────────
    //
    // Everything below is polling, not response: it asks the host for its
    // instance list and reads a measurement flag. Neither becomes more useful at
    // 20 Hz, and refreshInstanceCombo() builds a fresh vector on every call, so
    // they stay on the rate the whole timer used to run at. Raising the tick rate
    // was about latency on the editor refresh alone.
    if (++slowTickCounter < 10)
        return;
    slowTickCounter = 0;

    // Refresh the plugin instance picker (only relevant in VST mode)
    if (cbAudioMode.getSelectedId() == 1)
        refreshInstanceCombo();

    if (selectedPanelId < 0) return;

    auto panel = settings.getPanelById (selectedPanelId);
    if (! panel.isValid()) return;

    const auto type = panel.getProperty (AlterState::kType).toString();

    // Update Trend measurement status label (only when Trend view is active)
    if ((type == "audiometer" || type == "rms") && lblMeasureStatus.isVisible())
    {
        const int measureState = (int) panel.getProperty (AlterState::kMeasureState, 0);
        if (measureState == 1)
        {
            lblMeasureStatus.setColour (juce::Label::textColourId, AlterTheme::mintGlow);
            lblMeasureStatus.setText ("* Measuring...", juce::dontSendNotification);
        }
        else
        {
            lblMeasureStatus.setColour (juce::Label::textColourId, AlterTheme::textDim);
            lblMeasureStatus.setText ("[ Stopped ]", juce::dontSendNotification);
        }
    }
}

void ControllerContent::refreshInstanceCombo()
{
    if (! getPluginInstances) return;

    auto fresh = getPluginInstances();

    // changed?
    bool changed = fresh.size() != cachedInstances.size();
    if (! changed)
        for (size_t i = 0; i < fresh.size(); ++i)
            if (fresh[i].id != cachedInstances[i].id
                || fresh[i].active != cachedInstances[i].active
                || fresh[i].name != cachedInstances[i].name)
            { changed = true; break; }

    if (! changed) return;

    cachedInstances = std::move (fresh);

    const juce::uint32 currentSel = (juce::uint32) (juce::int64)
        settings.getTree().getProperty (AlterState::kPluginInstance, 0);

    cbPluginInstance.clear (juce::dontSendNotification);
    cbPluginInstance.addItem ("Auto", 1);

    int selectId = 1;
    for (size_t i = 0; i < cachedInstances.size(); ++i)
    {
        const auto& inst = cachedInstances[i];
        cbPluginInstance.addItem (inst.name + (inst.active ? "" : "  (offline)"),
                                  (int) i + 2);
        if (inst.id == currentSel && currentSel != 0)
            selectId = (int) i + 2;
    }

    cbPluginInstance.setSelectedId (selectId, juce::dontSendNotification);

    // rebuild the per-module picker with the same instance list
    cbModuleInstance.clear (juce::dontSendNotification);
    cbModuleInstance.addItem ("Auto", 1);
    for (size_t i = 0; i < cachedInstances.size(); ++i)
    {
        const auto& inst = cachedInstances[i];
        cbModuleInstance.addItem (inst.name + (inst.active ? "" : "  (offline)"), (int) i + 2);
    }
    updateModuleInstanceSelection();
}

void ControllerContent::updateModuleInstanceSelection()
{
    int selId = 1;   // Auto
    if (selectedPanelId >= 0)
    {
        auto panel = settings.getPanelById (selectedPanelId);
        if (panel.isValid())
        {
            const auto inst = (juce::uint32) (juce::int64) panel.getProperty (AlterState::kAudioInstance, 0);
            if (inst != 0)
                for (size_t i = 0; i < cachedInstances.size(); ++i)
                    if (cachedInstances[i].id == inst) { selId = (int) i + 2; break; }
        }
    }
    cbModuleInstance.setSelectedId (selId, juce::dontSendNotification);
}

void ControllerContent::applyAspect (int index)
{
    if (index < 0 || index >= kNumAspects) return;

    currentAspect = index;
    refreshAspectButtons();
    settings.getTree().setProperty ("hudAspect", index, nullptr);

    if (setHudAspect) setHudAspect (kAspectPresets[index].ratio);
}

void ControllerContent::refreshAspectButtons()
{
    // The highlight says which shape was last APPLIED, not what the window is:
    // the user is free to drag it afterwards, and second-guessing that with a
    // measured-ratio check would make the highlight flicker while resizing.
    const auto& p = kAspectPresets[juce::jlimit (0, kNumAspects - 1, currentAspect)];
    lblAspectValue.setText (p.label, juce::dontSendNotification);
    lblAspectValue.setTooltip (p.tip);
    btnAspectPrev.setTooltip (p.tip);
    btnAspectNext.setTooltip (p.tip);
}

void ControllerContent::resized()
{
    auto r = getLocalBounds().reduced (10);
    const int row = 28;
    const int gap = 6;

    // ── One layout rule for every horizontal band ────────────────────────────
    // Items keep their widths, the FIRST sits on the left edge, the LAST on the
    // right edge, and every leftover pixel is split EQUALLY between them. Widening
    // the window therefore moves every control by the same amount instead of
    // dumping the whole gain into one hole next to the right-anchored button — and
    // because each band ends on the same right edge, the right-hand column (Hold /
    // shape stepper / Close) stays a column at any width.
    struct Cell { juce::Component* c; int w; juce::Label* lbl; };
    const int minGap = 8;

    auto spread = [&] (juce::Rectangle<int> band,
                       juce::Rectangle<int> captions,          // empty = this band has no caption row
                       const std::vector<Cell>& cells) -> std::vector<juce::Rectangle<int>>
    {
        std::vector<juce::Rectangle<int>> out;
        const int n = (int) cells.size();
        if (n == 0) return out;

        int totalW = 0;
        for (const auto& cell : cells) totalW += cell.w;

        // Never let the band collapse past minGap — below that the row visibly
        // overflows, which is exactly what setResizeLimits() is there to prevent.
        const int slack = juce::jmax ((n - 1) * minGap, band.getWidth() - totalW);

        int usedW = 0;
        for (int i = 0; i < n; ++i)
        {
            // Position from the CUMULATIVE share of the slack, not from a per-gap
            // width: integer rounding then cannot accumulate and leave the last
            // item a few px short of the right edge.
            const int x = band.getX() + usedW
                        + (n > 1 ? (int) ((juce::int64) slack * i / (n - 1)) : 0);
            const juce::Rectangle<int> cell (x, band.getY(), cells[i].w, band.getHeight());

            if (cells[i].c != nullptr)
                cells[i].c->setBounds (cell);
            if (cells[i].lbl != nullptr && ! captions.isEmpty())
                cells[i].lbl->setBounds (cell.getX(), captions.getY(), cell.getWidth(), captions.getHeight());

            out.push_back (cell);
            usedW += cells[i].w;
        }
        return out;
    };

    // ── TOP BAR (presets + transport + the window-wide toggles) ──────────────
    {
        auto topBar = r.removeFromTop (row);
        // (FFT resolution / Constant-Q selector lives in the footer, bottom-left.)
        // btnHold is 74, not 62: JUCE eats ~9px of indent on EACH side of a button's
        // text, so "Hold" in bold had ~44px to live in and got ellipsised.
        spread (topBar, {}, { { &btnSavePreset, 80,  nullptr },
                              { &btnLoadPreset, 80,  nullptr },
                              { &btnRecord,     80,  nullptr },
                              { &alwaysOnTop,  130,  nullptr },
                              { &btnHideInfo,   92,  nullptr },
                              { &btnHold,       74,  nullptr } });
        r.removeFromTop (gap);
    }

    // footer = 2 control rows (label + controls) PLUS a dedicated brand-logotype
    // strip pinned to the very bottom of the window (centred).
    const int logoStripH = row;
    const int footerHeight = row * 2 + gap + 4 + gap + logoStripH;
    auto footer = r.removeFromBottom (footerHeight);

    // Bottom strip: the two WINDOW actions live here, one at each edge — Quit left,
    // Close right — with the brand logotype centred between them. Settings (theme,
    // HUD shape) belong with the other settings in the control row above; this strip
    // is only for "I am done with this window".
    auto logoStrip = footer.removeFromBottom (logoStripH);
    footer.removeFromBottom (gap);
    {
        // Quit and Close are the same width, so spreading the three cells evenly
        // puts the logotype dead centre for free — no special-casing needed.
        const int lh = logoStrip.getHeight();
        const int lw = juce::jmin (juce::roundToInt ((double) lh * kQspLogoTextAspect),
                                   juce::jmax (0, logoStrip.getWidth() - 2 * (80 + minGap)));

        auto strip = spread (logoStrip, {}, { { &btnQuit,  80, nullptr },
                                              { nullptr,   lw, nullptr },
                                              { &btnClose, 80, nullptr } });
        logoBounds = strip[1];
    }

    auto labelRow = footer.removeFromTop (row);
    footer.removeFromTop (gap);

    // Bottom row of controls — same rule as every other band: fixed widths, equal
    // gaps, both ends flush. The theme picker and the shape stepper each go in as
    // ONE cell holding a small cluster, so the swatches stay glued to the button
    // they belong to instead of drifting off with the shared gap.
    auto controlRow = footer;

    const int audioMode  = cbAudioMode.getSelectedId();
    const bool showGain     = (audioMode == 2);   // System Audio → gain knob
    const bool showInstance = (audioMode == 1);   // VST Plugin  → instance picker

    sGain.setVisible (showGain);            lblGain.setVisible (showGain);
    cbPluginInstance.setVisible (showInstance); lblInstance.setVisible (showInstance);

    const bool swatches = btnThemeC1.isVisible();
    const int  themeW   = 80 + (swatches ? 6 + 26 + 4 + 26 : 0);
    const int  stepperW = 22 + 68 + 22;   // 68: "Default" in bold needs it

    std::vector<Cell> cells;
    cells.push_back ({ &cbMaxBins,   110, &lblMaxBins });     // FFT resolution
    cells.push_back ({ &cbAudioMode, 112, &lblAudioMode });   // Audio input
    if (showInstance) cells.push_back ({ &cbPluginInstance, 104, &lblInstance });
    if (showGain)     cells.push_back ({ &sGain,             55, &lblGain });
    cells.push_back ({ nullptr, themeW,   nullptr });         // theme + Custom swatches
    cells.push_back ({ nullptr, stepperW, nullptr });         // < shape >  (under Hold / above Close)

    auto laid = spread (controlRow, labelRow, cells);

    {
        auto theme = laid[laid.size() - 2];
        auto btn   = theme.removeFromLeft (80);
        btnTheme.setBounds (btn);
        lblTheme.setBounds (btn.getX(), labelRow.getY(), btn.getWidth(), labelRow.getHeight());
        if (swatches)
        {
            theme.removeFromLeft (6);
            btnThemeC1.setBounds (theme.removeFromLeft (26).reduced (0, 3));
            theme.removeFromLeft (4);
            btnThemeC2.setBounds (theme.removeFromLeft (26).reduced (0, 3));
        }

        auto step = laid.back();
        btnAspectPrev .setBounds (step.removeFromLeft (22).reduced (0, 3));
        lblAspectValue.setBounds (step.removeFromLeft (68));
        btnAspectNext .setBounds (step.removeFromLeft (22).reduced (0, 3));
    }
    // (always-on-top / Hold live in the top bar; Quit + Close are the strip below.)

    // left
    auto left = r.removeFromLeft (180);
    btnAdd.setBounds (left.removeFromTop (row));
    left.removeFromTop (gap);
    panelList.setBounds (left);

    // right editor area (scrollable via viewport)
    auto rightArea = r.reduced (10);
    editorViewport.setBounds (rightArea);
    const int panelW = juce::jmax (1, rightArea.getWidth() - editorViewport.getScrollBarThickness());

    // nothing selected
    if (selectedPanelId < 0)
    {
        btnDelete.setVisible (false);
        btnGetInfo.setVisible (false);
        cbModuleInstance.setVisible (false);
        lblModuleInstance.setVisible (false);
        setEditorVisible ({});
        editorPanel.setSize (panelW, rightArea.getHeight());
        return;
    }

    // Layout in editorPanel coordinates (0,0 origin, tall virtual height)
    const int virtualH = 10000;
    auto p = juce::Rectangle<int> (0, 0, panelW, virtualH);

    btnDelete.setVisible (true);
    btnGetInfo.setVisible (true);

    auto deleteRow = p.removeFromTop (row);
    btnDelete.setBounds (deleteRow.removeFromLeft (100));
    deleteRow.removeFromLeft (6);
    btnGetInfo.setBounds (deleteRow.removeFromLeft (100));

    // per-module audio source picker on the right of the destroy/explore row
    cbModuleInstance.setVisible (true);
    lblModuleInstance.setVisible (true);
    cbModuleInstance.setBounds (deleteRow.removeFromRight (130));
    deleteRow.removeFromRight (4);
    lblModuleInstance.setBounds (deleteRow.removeFromRight (50));

    p.removeFromTop (gap);

    auto panel = settings.getPanelById (selectedPanelId);
    const auto type = panel.isValid() ? panel.getProperty (AlterState::kType).toString() : juce::String();

    setEditorVisible (type);

    // Module rotation + HUD block. "Rotate" is hidden for the generative/tone modules
    // (setEditorVisible decides), so lay the row out only when something is in it.
    const bool showRotate = btnRotate.isVisible();
    const bool showMirror = tbSpecMirror.isVisible();
    const bool showLayer  = settings.hudLayers() > 1;
    lblModuleLayer.setVisible (showLayer);
    cbModuleLayer.setVisible  (showLayer);
    if (showRotate || showMirror || showLayer)
    {
        auto rotRow = p.removeFromTop (row);
        if (showRotate)
        {
            lblModuleRotation.setBounds (rotRow.removeFromLeft (70));
            rotRow.removeFromLeft (6);
            btnRotate.setBounds (rotRow.removeFromLeft (60));
            rotRow.removeFromLeft (12);
        }
        if (showLayer)
        {
            lblModuleLayer.setBounds (rotRow.removeFromLeft (44));
            rotRow.removeFromLeft (4);
            cbModuleLayer.setBounds (rotRow.removeFromLeft (70));
        }
        p.removeFromTop (gap);

        // Its OWN row, directly under Rotate rather than beside it. Label, button,
        // toggle and block picker together overrun the editor's width, and the
        // thing that would have been pushed off the end is the block picker — a
        // control that has nothing to do with either of them.
        if (showMirror)
        {
            auto mirRow = p.removeFromTop (row);
            mirRow.removeFromLeft (76);          // line up under the Rotate button
            tbSpecMirror.setBounds (mirRow.removeFromLeft (120));
            p.removeFromTop (gap);
        }
    }

    auto labelledRow = [&](juce::Label& l, juce::Component& c, int labelW = 70, int compW = -1)
    {
        auto a = p.removeFromTop (row);
        l.setBounds (a.removeFromLeft (labelW));
        a.removeFromLeft (6);
        c.setBounds (compW > 0 ? a.removeFromLeft (compW) : a);
        p.removeFromTop (gap);
    };

    // The base-colour row: label, swatch, picker button. Four editors laid this
    // out identically by hand; now that tone colour can hide it in all of them,
    // that is four places to keep in step, so it is written once.
    auto colourRow = [&] (int labelW = 70)
    {
        lblColor.setVisible (true);
        lblColorPreview.setVisible (true);
        btnColor.setVisible (true);
        auto a = p.removeFromTop (row);
        lblColor.setBounds (a.removeFromLeft (labelW));
        lblColorPreview.setBounds (a.removeFromLeft (24));
        a.removeFromLeft (6);
        btnColor.setBounds (a.removeFromLeft (90));
        p.removeFromTop (gap);
    };

    // 'Color by tone' + the two controls that only exist while it is on, then
    // either the tone rows or the base-colour picker — never both, because in tone
    // mode the picked colour has no effect and a live-looking control that does
    // nothing is worse than an absent one. Mirrors what Synesthesia, Geometry and
    // Chladni already do; shared here across the four analyser modules.
    //
    // @param withCheckbox  false for Spectrogram, whose colour choice is a
    //                      three-way combo laid out by the caller instead.
    auto toneRows = [&] (bool toneOn, int labelW = 70, bool withCheckbox = true)
    {
        if (withCheckbox)
        {
            tbToneColor.setVisible (true);
            auto t = p.removeFromTop (row);
            tbToneColor.setBounds (t.removeFromLeft (140));
            p.removeFromTop (gap);
        }
        else
            tbToneColor.setVisible (false);

        tbToneTwist.setVisible (toneOn);
        lblToneSmooth.setVisible (toneOn);
        sToneSmooth.setVisible (toneOn);

        if (toneOn)
        {
            auto tw = p.removeFromTop (row);
            tbToneTwist.setBounds (tw.removeFromLeft (170));
            p.removeFromTop (gap);
            labelledRow (lblToneSmooth, sToneSmooth, labelW);
        }
    };

    if (type == "audiometer" || type == "rms")
    {
        labelledRow (lblRmsMode, cbRmsPeakMode, 70, 160);

        const bool isLevelHistory = (cbRmsPeakMode.getSelectedId() == 4);
        const bool meterTone     = tbMeterToneColor.getToggleState();

        // The meter's colour block, laid out identically in all three of its views
        // (Momentary bar, Trend curve, Level history) because it means the same
        // thing in all three — the ladder the picture is drawn from.
        //
        // The Trend view used to hide it entirely as "irrelevant". It was not: the
        // curve is zone-coloured off exactly the same ladder as the bar, so hiding
        // the controls meant the one view whose colours you could not change was
        // the one drawn in the most colours.
        auto meterColourBlock = [&] (bool withShade)
        {
            {
                auto t = p.removeFromTop (row);
                tbMeterToneColor.setVisible (true);
                tbMeterToneColor.setBounds (t.removeFromLeft (140));
                p.removeFromTop (gap);
            }

            lblMeterToneShade.setVisible (meterTone && withShade);
            cbMeterToneShade.setVisible  (meterTone && withShade);

            if (meterTone)
            {
                // Level history draws one trace, not a zone ladder, so it has no
                // shade to pick — the note IS the colour and there is nothing to
                // step away from.
                if (withShade)
                    labelledRow (lblMeterToneShade, cbMeterToneShade, 70, 130);

                // The shared twist + 'Tone smooth' rows, without their checkbox:
                // this module supplies its own above (see kMeterToneColor).
                toneRows (true, 70, false);

                lblColorMode.setVisible (false);  cbColorMode.setVisible (false);
                lblColor.setVisible (false);      lblColorPreview.setVisible (false);
                btnColor.setVisible (false);
                return;
            }

            toneRows (false, 70, false);   // hides the twist/smooth rows

            // Level history has no zones either, so it takes a plain colour picker
            // where the bar views take the Standard/Gradient/Spectrum combo.
            if (withShade)
            {
                labelledRow (lblColorMode, cbColorMode, 70, 120);
                if ((int) panel.getProperty (AlterState::kColorMode, 0) == 0)
                {
                    // Standard: the fixed DAW green/yellow/red, nothing to pick.
                    lblColor.setVisible (false);
                    lblColorPreview.setVisible (false);
                    btnColor.setVisible (false);
                    return;
                }
            }
            else
            {
                lblColorMode.setVisible (false);
                cbColorMode.setVisible (false);
            }

            colourRow();
        };

        if (isLevelHistory)
        {
            // Level history: its own controls (Display layout + time window); the
            // meter view / measurement controls have nothing to act on here.
            lblMeterView.setVisible (false);   cbMeterView.setVisible (false);
            btnMeasureStart.setVisible (false); btnMeasureStop.setVisible (false);
            lblMeasureStatus.setVisible (false);
            tbMeterTwoBars.setVisible (false);

            // Smooth applies here too, off the same slider and the same scale as the
            // bar views — it smooths the envelope as it is RECORDED.
            lblRmsSmooth.setVisible (true);  sRmsSmooth.setVisible (true);
            labelledRow (lblRmsSmooth, sRmsSmooth);

            lblDisplayMode.setVisible (true);  cbDisplayMode.setVisible (true);
            lblOscLtWin.setVisible (true);     sOscLtWin.setVisible (true);
            labelledRow (lblDisplayMode, cbDisplayMode, 70, 120);
            labelledRow (lblOscLtWin,   sOscLtWin, 70, 150);

            // L/R colour only exists where there are two overlaid traces to tell
            // apart — the mirror layouts already separate them by position.
            const bool overlaidStereo = (cbDisplayMode.getSelectedId() == 2);
            lblLrColor.setVisible (overlaidStereo);
            cbLrColor.setVisible  (overlaidStereo);
            if (overlaidStereo)
                labelledRow (lblLrColor, cbLrColor, 70, 130);

            {
                auto a = p.removeFromTop (row);
                tbClipZone.setVisible (true);
                tbClipZone.setBounds (a.removeFromLeft (140));
                p.removeFromTop (gap);
            }

            meterColourBlock (false);
        }
        else
        {
            lblMeterView.setVisible (true);  cbMeterView.setVisible (true);
            lblRmsSmooth.setVisible (true);  sRmsSmooth.setVisible (true);
            lblDisplayMode.setVisible (false); cbDisplayMode.setVisible (false);
            lblOscLtWin.setVisible (false);    sOscLtWin.setVisible (false);
            lblLrColor.setVisible (false);     cbLrColor.setVisible (false);
            tbClipZone.setVisible (false);

            labelledRow (lblMeterView, cbMeterView, 70, 120);

            const bool isTrend = (cbMeterView.getSelectedId() == 2);

            labelledRow (lblRmsSmooth, sRmsSmooth);

            btnMeasureStart.setVisible (isTrend);
            btnMeasureStop.setVisible  (isTrend);
            lblMeasureStatus.setVisible (isTrend);

            if (isTrend)
            {
                auto btnRow = p.removeFromTop (row);
                btnMeasureStart.setBounds (btnRow.removeFromLeft (80));
                btnRow.removeFromLeft (6);
                btnMeasureStop.setBounds (btnRow.removeFromLeft (80));
                p.removeFromTop (gap);

                lblMeasureStatus.setBounds (p.removeFromTop (row));
                p.removeFromTop (gap);
            }

            // The second bar belongs to the bar view only — the Trend view plots one
            // curve over time and has no column for a hold to stand in.
            tbMeterTwoBars.setVisible (! isTrend);
            if (! isTrend)
            {
                auto a = p.removeFromTop (row);
                tbMeterTwoBars.setBounds (a.removeFromLeft (140));
                p.removeFromTop (gap);
            }

            meterColourBlock (true);
        }
    }
    else if (type == "synesthesia")
    {
        labelledRow (lblSynGhost, sSynGhost);
        labelledRow (lblZoom, sZoom);
        labelledRow (lblRotation, sRotation, 70, 80);
        labelledRow (lblSymmetry, sSymmetry, 70, 80);
        {
            auto m = p.removeFromTop (row);
            tbSynMirror.setBounds (m.removeFromLeft (120));
            p.removeFromTop (gap);
        }
        labelledRow (lblSaturation, sSaturation);
        labelledRow (lblSynBright, sSynBright);
        labelledRow (lblBloom, sBloom);
        labelledRow (lblSpeed, sSpeed);
        {
            auto bs = p.removeFromTop (row);
            tbSynBpmSync.setBounds (bs.removeFromLeft (120));
            p.removeFromTop (gap);
        }
        const bool synBpm = tbSynBpmSync.getToggleState();
        lblSynBpm.setVisible (synBpm);     sSynBpm.setVisible (synBpm);
        lblSynBeatDiv.setVisible (synBpm); sSynBeatDiv.setVisible (synBpm);
        if (synBpm)
        {
            labelledRow (lblSynBpm,     sSynBpm, 70);
            labelledRow (lblSynBeatDiv, sSynBeatDiv, 70, 80);
        }
        labelledRow (lblSynChange, sSynChange);
        labelledRow (lblSynTransmute, sSynTransmute);
        labelledRow (lblSynClear, sSynClear);
        labelledRow (lblSynDenoise, sSynDenoise);
        labelledRow (lblSynTunnel,  sSynTunnel);
        labelledRow (lblSynVortex,  sSynVortex);
        labelledRow (lblSynReact, sSynReact);

        // Tone colour checkbox, with Tone smooth directly beneath it.
        auto t = p.removeFromTop (row);
        tbSynTone.setBounds (t.removeFromLeft (120));
        p.removeFromTop (gap);

        // 'Mirror tone color' belongs to the tone mapping, so it only exists while
        // the colour actually follows the tone — in manual mode there is no wheel
        // direction to reverse and the box would just be a dead control.
        const bool tone = tbSynTone.getToggleState();
        tbSynTwist.setVisible (tone);
        if (tone)
        {
            auto tw = p.removeFromTop (row);
            tbSynTwist.setBounds (tw.removeFromLeft (170));
            p.removeFromTop (gap);
        }

        labelledRow (lblSynSmooth, sSynSmooth);

        // base colour picker (hidden when the colour follows the tone)
        lblColor.setVisible (! tone);
        lblColorPreview.setVisible (! tone);
        btnColor.setVisible (! tone);
        if (! tone)
        {
            auto a = p.removeFromTop (row);
            lblColor.setBounds (a.removeFromLeft (70));
            lblColorPreview.setBounds (a.removeFromLeft (24));
            a.removeFromLeft (6);
            btnColor.setBounds (a.removeFromLeft (90));
            p.removeFromTop (gap);
        }
    }
    else if (type == "geometry")
    {
        // Speed + BPM at the very top (above Complexity).
        labelledRow (lblSpeed,     sSpeed, 80);
        {
            auto bs = p.removeFromTop (row);
            tbGeoBpmSync.setBounds (bs.removeFromLeft (120));
            p.removeFromTop (gap);
        }
        const bool bpm = tbGeoBpmSync.getToggleState();
        lblGeoBpm.setVisible (bpm);   sGeoBpm.setVisible (bpm);
        lblGeoBeatDiv.setVisible (bpm); sGeoBeatDiv.setVisible (bpm);
        if (bpm)
        {
            labelledRow (lblGeoBpm,     sGeoBpm, 80);
            labelledRow (lblGeoBeatDiv, sGeoBeatDiv, 80, 80);
        }

        labelledRow (lblGeoComplexity, sGeoComplexity, 80);
        labelledRow (lblGeoRandom,  sGeoRandom, 80);
        labelledRow (lblGeoReact,   sGeoReact, 80);
        labelledRow (lblGeoTri,    sGeoTri,    80);
        labelledRow (lblGeoSquare, sGeoSquare, 80);
        labelledRow (lblGeoCircle, sGeoCircle, 80);
        labelledRow (lblSymmetry,  sSymmetry, 80, 80);
        {
            auto m = p.removeFromTop (row);
            tbSynMirror.setBounds (m.removeFromLeft (120));
            p.removeFromTop (gap);
        }
        labelledRow (lblZoom,       sZoom, 80);
        labelledRow (lblGeoTunnel,  sGeoTunnel, 80);
        labelledRow (lblGeoDepth,   sGeoDepth, 80);           // Depth sits right under Tunnel
        labelledRow (lblGeoAperture, sGeoAperture, 80);       // Aperture right under Depth
        labelledRow (lblRotation,     sRotation, 80, 80);     // per-object spin
        labelledRow (lblGeoGlobalRot, sGeoGlobalRot, 80, 80); // whole-module rotation

        labelledRow (lblSaturation, sSaturation, 80);
        labelledRow (lblSynBright,  sSynBright, 80);   // shared Brightness (light output)
        labelledRow (lblBloom,     sBloom, 80);

        // Tone colour checkbox, with Tone smooth directly beneath it.
        auto t = p.removeFromTop (row);
        tbGeoTone.setBounds (t.removeFromLeft (120));
        p.removeFromTop (gap);

        // 'Mirror tone color' — only while the colour follows the tone (see Synesthesia).
        const bool tone = tbGeoTone.getToggleState();
        tbGeoTwist.setVisible (tone);
        if (tone)
        {
            auto tw = p.removeFromTop (row);
            tbGeoTwist.setBounds (tw.removeFromLeft (170));
            p.removeFromTop (gap);
        }

        labelledRow (lblSynSmooth, sSynSmooth, 80);

        // base colour picker (hidden when colour follows the tone)
        lblColor.setVisible (! tone);
        lblColorPreview.setVisible (! tone);
        btnColor.setVisible (! tone);
        if (! tone)
        {
            auto a = p.removeFromTop (row);
            lblColor.setBounds (a.removeFromLeft (70));
            lblColorPreview.setBounds (a.removeFromLeft (24));
            a.removeFromLeft (6);
            btnColor.setBounds (a.removeFromLeft (90));
            p.removeFromTop (gap);
        }
    }
    else if (type == "fusion")
    {
        // THE STACK, bottom-up: slot A is the base and every slot above it brings
        // its own blend mode. Each slot's settings sit directly under its picker,
        // so a layer reads as one block rather than as scattered knobs.
        auto stage = [&] (juce::ToggleButton& tb)
        {
            auto r = p.removeFromTop (row);
            tb.setBounds (r.removeFromLeft (140));
            p.removeFromTop (gap);
        };

        // A fold header spans the full width and owns everything under it until
        // the next one. Folded, it is the only row the block costs.
        auto foldHeader = [&] (juce::TextButton& b)
        {
            auto r = p.removeFromTop (row);
            b.setBounds (r);
            p.removeFromTop (gap);
        };

        for (int i = 0; i < AlterState::kMaxFusionLayers; ++i)
        {
            foldHeader (btnFusionLayHead[i]);

            // Everything below is laid out only when it is visible, and
            // refreshRightEditorFromSelection has already hidden the whole block
            // if this layer is folded — so folding needs no second rule here.
            if (cbFusionLayer[i].isVisible())
                labelledRow (lblFusionLayer[i], cbFusionLayer[i], 70, 170);

            if (cbFusionBlend[i].isVisible())
            {
                auto blendRow = p.removeFromTop (row);
                blendRow.removeFromLeft (70);                // indent under the picker
                cbFusionBlend[i].setBounds (blendRow.removeFromLeft (120));
                p.removeFromTop (gap);
            }

            if (sFusionOpacity[i].isVisible())  labelledRow (lblFusionOpacity[i], sFusionOpacity[i], 80);
            if (sFusionAmount[i].isVisible())   labelledRow (lblFusionAmount[i],  sFusionAmount[i],  80);
            if (sFusionBands[i].isVisible())    labelledRow (lblFusionBands[i],   sFusionBands[i],   80);
            if (sFusionAngle[i].isVisible())    labelledRow (lblFusionAngle[i],   sFusionAngle[i],   80);
            if (sFusionScale[i].isVisible())    labelledRow (lblFusionScale[i],   sFusionScale[i],   80);

            // The layer's own post chain, last inside its block: it acts on the
            // layer AFTER everything above has said what the layer is.
            if (sFusionLayMirror[i].isVisible())
                labelledRow (lblFusionLayMirror[i], sFusionLayMirror[i], 80, 80);
            if (sFusionLayMirrorAng[i].isVisible())
                labelledRow (lblFusionLayMirrorAng[i], sFusionLayMirrorAng[i], 80);
            if (sFusionLaySymmetry[i].isVisible())
                labelledRow (lblFusionLaySymmetry[i], sFusionLaySymmetry[i], 80, 80);
            if (sFusionLaySpin[i].isVisible())
                labelledRow (lblFusionLaySpin[i], sFusionLaySpin[i], 80);
            if (sFusionLaySpeed[i].isVisible())
                labelledRow (lblFusionLaySpeed[i], sFusionLaySpeed[i], 80);
            if (sFusionLayZoom[i].isVisible())
                labelledRow (lblFusionLayZoom[i], sFusionLayZoom[i], 80);
        }

        // ── global: the same chain, applied to the RESULT ───────────────────
        foldHeader (btnFusionGlobalHead);

        // WARP and LIQUID are siblings — the two displacement stages — so their
        // toggles sit together at the top of the global block, Liquid directly under
        // the Warp block, before the folds.
        if (tbFusionWarp.isVisible())     stage (tbFusionWarp);
        if (sFusionWarpAmt.isVisible())   labelledRow (lblFusionWarpAmt,   sFusionWarpAmt,   80);
        if (sFusionWarpSwirl.isVisible()) labelledRow (lblFusionWarpSwirl, sFusionWarpSwirl, 80);
        if (sFusionWarpSmooth.isVisible()) labelledRow (lblFusionWarpSmooth, sFusionWarpSmooth, 80);
        if (sFusionWarpDenoise.isVisible()) labelledRow (lblFusionWarpDenoise, sFusionWarpDenoise, 80);
        if (cbFusionWarpSrc.isVisible())  labelledRow (lblFusionWarpSrc,   cbFusionWarpSrc,  80, 120);

        if (tbFusionLiquid.isVisible())      stage (tbFusionLiquid);
        if (sFusionLiquidAmt.isVisible())    labelledRow (lblFusionLiquidAmt,    sFusionLiquidAmt,    80);
        if (sFusionLiquidSmooth.isVisible()) labelledRow (lblFusionLiquidSmooth, sFusionLiquidSmooth, 80);
        if (sFusionLiquidDenoise.isVisible()) labelledRow (lblFusionLiquidDenoise, sFusionLiquidDenoise, 80);

        if (sFusionSymmetry.isVisible())    labelledRow (lblFusionSymmetry, sFusionSymmetry, 80, 80);
        if (sFusionMirror.isVisible())      labelledRow (lblFusionMirror,   sFusionMirror,   80, 80);
        if (sFusionMirrorAngle.isVisible()) labelledRow (lblFusionMirrorAngle, sFusionMirrorAngle, 80);
        if (sFusionSpin.isVisible())        labelledRow (lblFusionSpin,  sFusionSpin,  80);
        if (sFusionZoom.isVisible())        labelledRow (lblFusionZoom,  sFusionZoom,  80);
        if (sFusionVortex.isVisible())      labelledRow (lblFusionVortex, sFusionVortex, 80);
        if (sSpeed.isVisible())             labelledRow (lblSpeed,       sSpeed,       80);
        if (sFusionReact.isVisible())       labelledRow (lblFusionReact, sFusionReact, 80);

        // Global-chain layer targeting: label + one small toggle per slot on a row.
        if (tbFusionGlobLayer[0].isVisible())
        {
            auto rr = p.removeFromTop (row);
            lblFusionGlobLayers.setBounds (rr.removeFromLeft (80));
            rr.removeFromLeft (6);
            for (int i = 0; i < AlterState::kMaxFusionLayers; ++i)
            {
                tbFusionGlobLayer[i].setBounds (rr.removeFromLeft (50));
                rr.removeFromLeft (4);
            }
            p.removeFromTop (gap);
        }

        if (tbFusionTunnel.isVisible())      stage (tbFusionTunnel);

        if (cbFusionDetail.isVisible())     labelledRow (lblFusionDetail, cbFusionDetail, 80, 110);
    }
    else if (type == "oscillator" || type == "oscilator")
    {
        labelledRow (lblOscTerm, cbOscTerm, 70, 120);
        labelledRow (lblOscSmooth, sOscSmooth);

        auto a = p.removeFromTop (row);
        tbFill.setBounds (a.removeFromLeft (120));
        p.removeFromTop (gap);

        labelledRow (lblDisplayMode, cbDisplayMode, 70, 120);

        // short-term: zoom; long wave: window length
        const int term = cbOscTerm.getSelectedId() - 1;   // 0 = short, 1 = long wave
        lblOscZoom.setVisible  (term == 0);
        sOscZoom.setVisible    (term == 0);
        lblOscLtWin.setVisible (term == 1);
        sOscLtWin.setVisible   (term == 1);
        tbOscSymmetry.setVisible (false);
        if (term == 1)
            labelledRow (lblOscLtWin, sOscLtWin);
        else
            labelledRow (lblOscZoom, sOscZoom);

        // L/R colour only exists where the two channels are overlaid on one
        // baseline — the mirror layouts already separate them by position.
        const bool overlaidStereo = (cbDisplayMode.getSelectedId() == 2);
        lblLrColor.setVisible (overlaidStereo);
        cbLrColor.setVisible  (overlaidStereo);
        if (overlaidStereo)
            labelledRow (lblLrColor, cbLrColor, 70, 130);

        // The dB scale is meaningful only in the Long Waveform: the Short-Term Scope
        // normalises its trace to its own peak, so a 0 dB line there would sit at the
        // top of the picture at every level. See setClipZone in Oscilator.h.
        tbClipZone.setVisible (term == 1);
        if (term == 1)
        {
            auto c = p.removeFromTop (row);
            tbClipZone.setBounds (c.removeFromLeft (140));
            p.removeFromTop (gap);
        }

        const bool tone = tbToneColor.getToggleState();
        toneRows (tone);
        lblColor.setVisible (! tone);
        lblColorPreview.setVisible (! tone);
        btnColor.setVisible (! tone);
        if (! tone) colourRow();
    }
    else if (type == "spectrum")
    {
        labelledRow (lblSpecSmooth, sSpecSmooth);
        labelledRow (lblSpecPsycho, cbSpecPsycho, 70, 120);

        // ISO226 phon slider (visible only when ISO226 selected)
        const int psycho = (int) panel.getProperty (AlterState::kPsychoCurve, 0);
        lblSpecPhon.setVisible (psycho == 2);
        sSpecPhon.setVisible (psycho == 2);
        if (psycho == 2)
            labelledRow (lblSpecPhon, sSpecPhon);

        labelledRow (lblSpecRef, cbSpecRef, 70, 130);

        // Measurement has no meaning in Constant-Q — hide it then. STEREO works in
        // CQ too (true per-channel L/R constant-Q streams), and Peak-hold as well,
        // so both stay available in every mode.
        const bool cqOn = tbSpecConstantQ.getToggleState();
        tbSpecMeasurement.setVisible (! cqOn);
        tbSpecStereo.setVisible      (true);
        tbSpecPeakHold.setVisible    (true);

        {
            auto a = p.removeFromTop (row);
            if (! cqOn)
            {
                tbSpecMeasurement.setBounds (a.removeFromLeft (160));
                a.removeFromLeft (6);
            }
            tbSpecStereo.setBounds (a.removeFromLeft (cqOn ? 160 : 130));
            p.removeFromTop (gap);
        }

        {
            auto a = p.removeFromTop (row);   // Peak-hold + Constant-Q row (always shown)
            tbSpecPeakHold.setBounds (a.removeFromLeft (160));
            a.removeFromLeft (6);
            tbSpecConstantQ.setBounds (a.removeFromLeft (130));
            p.removeFromTop (gap);
        }

        // L/R colour relationship (relevant whenever stereo is on, incl. Constant-Q)
        const bool stereoOn = tbSpecStereo.getToggleState();
        lblSpecLrColor.setVisible (stereoOn);
        cbSpecLrColor.setVisible  (stereoOn);
        if (stereoOn)
            labelledRow (lblSpecLrColor, cbSpecLrColor, 70, 130);

        // The L/R relationship above stays available in tone mode: it offsets the
        // R hue FROM the base colour, whatever supplies that base — so complementary
        // / analogous keeps separating the channels around the note.
        const bool tone = tbToneColor.getToggleState();
        toneRows (tone);
        lblColor.setVisible (! tone);
        lblColorPreview.setVisible (! tone);
        btnColor.setVisible (! tone);
        if (! tone) colourRow();
    }
    else if (type == "chladni")
    {
        // Row: audio reactive toggle
        auto a = p.removeFromTop (row);
        tbChladniReactive.setBounds (a.removeFromLeft (180));
        p.removeFromTop (gap);

        // Row: preset selector (manual base figures)
        labelledRow (lblChladniPreset, cbChladniPreset, 80, 120);

        // Row: m / n  (side by side)
        a = p.removeFromTop (row);
        lblChladniM.setBounds (a.removeFromLeft (16));
        a.removeFromLeft (2);
        sChladniM.setBounds (a.removeFromLeft (90));
        a.removeFromLeft (10);
        lblChladniN.setBounds (a.removeFromLeft (16));
        a.removeFromLeft (2);
        sChladniN.setBounds (a.removeFromLeft (90));
        p.removeFromTop (gap);

        // Row: reactive shift
        labelledRow (lblChladniShift, sChladniShift, 80, 120);

        labelledRow (lblChladniAR, sChladniAR, 80);
        labelledRow (lblChladniSharp, sChladniSharp, 80);
        labelledRow (lblChladniParticles, sChladniParticles, 80);
        labelledRow (lblChladniMaterial, cbChladniMaterial, 80, 160);

        // Row: tone colour toggle
        a = p.removeFromTop (row);
        tbChladniToneColor.setBounds (a.removeFromLeft (160));
        p.removeFromTop (gap);

        // Row: 'Mirror tone color' — only while colour-by-tone is active (see Synesthesia).
        const bool byTone = tbChladniToneColor.getToggleState();
        tbChladniTwist.setVisible (byTone);
        lblToneSmooth.setVisible (byTone);
        sToneSmooth.setVisible   (byTone);
        if (byTone)
        {
            a = p.removeFromTop (row);
            tbChladniTwist.setBounds (a.removeFromLeft (170));
            p.removeFromTop (gap);
            // Chladni borrows the SHARED Tone smooth slider (it has no smoothing
            // control of its own to collide with, and kSmooth is unused here).
            labelledRow (lblToneSmooth, sToneSmooth, 80);
        }

        // Row: Sand colour (hidden when colour-by-tone is active)
        lblColor.setVisible (! byTone);
        lblColorPreview.setVisible (! byTone);
        btnColor.setVisible (! byTone);
        if (! byTone)
        {
            a = p.removeFromTop (row);
            lblColor.setBounds (a.removeFromLeft (70));
            lblColorPreview.setBounds (a.removeFromLeft (24));
            a.removeFromLeft (6);
            btnColor.setBounds (a.removeFromLeft (90));
            p.removeFromTop (gap);
        }
    }
    else if (type == "toneanalyzer")
    {
        labelledRow (lblToneSens, sToneSens, 80);
        tbToneTuner.setBounds (p.removeFromTop (row).removeFromLeft (160));
        p.removeFromTop (gap);

        // Same shared checkbox and same key as the other analysers — this module's
        // accent colour is a colour like any other, and the fact that it happens to
        // also NAME the note does not change where the colour comes from.
        const bool tone = tbToneColor.getToggleState();
        toneRows (tone, 80);
        lblColor.setVisible (! tone);
        lblColorPreview.setVisible (! tone);
        btnColor.setVisible (! tone);
        if (! tone) colourRow (70);
    }
    else if (type == "spectrogram")
    {
        labelledRow (lblSpectroSmooth, sSpectroSmooth, 80);
        labelledRow (lblSpectroWin, sSpectroWin, 80);
        labelledRow (lblSpectroFill, sSpectroFill, 80);

        auto a = p.removeFromTop (row);
        tbSpectroMirror.setBounds (a);
        p.removeFromTop (gap);

        {
            auto cq = p.removeFromTop (row);
            tbSpectroConstantQ.setBounds (cq.removeFromLeft (130));
            cq.removeFromLeft (6);
            tbSpectroReassign.setBounds (cq.removeFromLeft (140));
            p.removeFromTop (gap);
        }

        labelledRow (lblSpectroColorMode, cbSpectroColorMode, 80, 150);

        const int  cmode  = juce::jlimit (0, 2, (int) panel.getProperty (AlterState::kColorMode, 0));
        const bool custom = (cmode == 1);
        const bool tone   = (cmode == 2);

        // The tone rows carry no checkbox here — the combo above already made the
        // choice, and a checkbox restating it could disagree with it.
        toneRows (tone, 80, false);

        // Two pickers, and only for the custom gradient. The theme heat map has no
        // user colours at all, and in tone mode both ends of the ramp come from the
        // note, so neither picker would do anything.
        lblColor.setVisible (custom);
        lblColorPreview.setVisible (custom);
        btnColor.setVisible (custom);
        lblColor2.setVisible (custom);
        lblColor2Preview.setVisible (custom);
        btnColor2.setVisible (custom);
        if (custom)
        {
            colourRow (80);

            a = p.removeFromTop (row);
            lblColor2.setBounds (a.removeFromLeft (80));
            lblColor2Preview.setBounds (a.removeFromLeft (24));
            a.removeFromLeft (6);
            btnColor2.setBounds (a.removeFromLeft (90));
            p.removeFromTop (gap);
        }
    }
    else if (type == "stereoscope")
    {
        // id: 1=Particles, 2=Goniometer, 3=Vectorscope, 4=Correlation, 5=Correlometer
        const int sm = cbStereoMode.getSelectedId();
        const bool isParticles = (sm == 1);
        const bool isGonio     = (sm == 2);
        const bool isPolar     = (sm == 3);
        const bool isCorrelo   = (sm == 5);
        const bool gonioCloud  = isGonio && tbStereoParticles.getToggleState();
        const bool showToggle  = isGonio;                                   // particle on/off
        const bool showDensity = isParticles || isPolar || gonioCloud;
        const bool showBright  = isGonio || isPolar;

        // LINE WIDTH belongs to the goniometer's continuous beam, so it is shown
        // only while that beam is what is being drawn — flipping the Particles
        // toggle on replaces the line with dots and the control with Point size.
        const bool showLineW = isGonio && ! gonioCloud;
        // POINT SIZE tracks wherever dots are drawn, which is the same set of
        // modes Density applies to.
        const bool showPoint = showDensity;

        tbStereoParticles.setVisible (showToggle);
        lblStereoDensity.setVisible  (showDensity);
        sStereoDensity.setVisible    (showDensity);
        lblStereoBright.setVisible   (showBright);
        sStereoBright.setVisible     (showBright);
        lblStereoLineW.setVisible    (showLineW);
        sStereoLineW.setVisible      (showLineW);
        lblStereoPointSize.setVisible (showPoint);
        sStereoPointSize.setVisible   (showPoint);
        tbStereoCtrlBins.setVisible  (isCorrelo);

        labelledRow (lblStereoMode,   cbStereoMode, 80, 130);
        labelledRow (lblStereoSmooth, sStereoSmooth, 80);

        if (showToggle)
        {
            auto t = p.removeFromTop (row);
            tbStereoParticles.setBounds (t.removeFromLeft (110));
            p.removeFromTop (gap);
        }
        if (showDensity)
            labelledRow (lblStereoDensity, sStereoDensity, 80);
        if (showBright)
            labelledRow (lblStereoBright, sStereoBright, 80);
        if (showLineW)
            labelledRow (lblStereoLineW, sStereoLineW, 80);
        if (showPoint)
            labelledRow (lblStereoPointSize, sStereoPointSize, 80);
        if (isCorrelo)
        {
            auto t = p.removeFromTop (row);
            tbStereoCtrlBins.setBounds (t.removeFromLeft (160));
            p.removeFromTop (gap);
        }

        const bool tone = tbToneColor.getToggleState();
        toneRows (tone, 80);
        lblColor.setVisible (! tone);
        lblColorPreview.setVisible (! tone);
        btnColor.setVisible (! tone);
        if (! tone) colourRow (80);
    }

    // Set scrollable editor panel content size
    const int usedHeight = virtualH - p.getHeight();
    editorPanel.setSize (panelW, juce::jmax (rightArea.getHeight(), usedHeight));
}

// Re-read every controller-global widget from the settings tree. Called after a
// preset load: the tree already carries the new values (theme, custom colours,
// FFT bins / Constant-Q, gain, always-on-top…) and MainComponent has applied
// them — this keeps the visible controls from showing stale state (and from
// writing that stale state back on the next click).
void ControllerContent::syncGlobalWidgetsFromState()
{
    updateThemeButtonText();
    refreshThemeSwatches();

    cbMaxBins.setSelectedId (settings.globalCqt() ? 1 : settings.maxFftBins(),
                             juce::dontSendNotification);

    sGain.setValue ((float) settings.getTree().getProperty (AlterState::kSystemGain, 0.0f),
                    juce::dontSendNotification);

    alwaysOnTop.setToggleState (settings.controllerAlwaysOnTop(), juce::dontSendNotification);

    updateLogoColour();
}

void ControllerContent::updateThemeButtonText()
{
    // Theme index: 0 Cyber · 1 Dark · 2 Custom · 3 White
    static const char* const names[] = { "Cyber", "Dark", "Custom", "White" };
    const int t = juce::jlimit (0, 3, (int) settings.getTree().getProperty (AlterState::kTheme, 0));
    btnTheme.setButtonText (names[t]);
}

void ControllerContent::showThemeMenu()
{
    const int cur = (int) settings.getTree().getProperty (AlterState::kTheme, 0);

    // Apply a theme index (0 Cyber · 1 Dark · 2 Custom · 3 White) and refresh every window.
    auto applyTheme = [this] (int themeIdx)
    {
        settings.getTree().setProperty (AlterState::kTheme, themeIdx, nullptr);
        if (auto* lnf = dynamic_cast<AlterLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel()))
            lnf->refreshFromTheme();
        for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
            juce::TopLevelWindow::getTopLevelWindow (i)->sendLookAndFeelChange();
        updateLogoColour();
        updateThemeButtonText();
        refreshThemeSwatches();   // show/hide + recolour the footer swatches for Custom
        resized();                // swatches appearing/vanishing re-clamps the logo strip
        repaint();
    };

    juce::PopupMenu menu;
    menu.addItem (1, "Cyber",  true, cur == 0);
    menu.addItem (2, "Dark",   true, cur == 1);
    menu.addItem (3, "White",  true, cur == 3);
    menu.addItem (4, "Custom", true, cur == 2);   // colour swatches appear in the top bar

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&btnTheme),
        [applyTheme] (int r)
        {
            switch (r)
            {
                case 1: applyTheme (0); break;
                case 2: applyTheme (1); break;
                case 3: applyTheme (3); break;
                case 4: applyTheme (2); break;
                default: break;
            }
        });
}

void ControllerContent::openThemeColour (const juce::Identifier& prop, juce::Colour fallback)
{
    const juce::Colour initial ((uint32_t)(int) settings.getTree().getProperty (prop, (int) fallback.getARGB()));
    const int popW = juce::jlimit (220, 300, getWidth()  - 30);
    const int popH = juce::jlimit (200, 340, getHeight() - 30);

    auto popup = std::make_unique<ColorPopup> (initial, [this, prop] (juce::Colour c)
    {
        settings.getTree().setProperty (prop, (int) c.getARGB(), nullptr);

        if (auto* lnf = dynamic_cast<AlterLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel()))
            lnf->refreshFromTheme();
        for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
            juce::TopLevelWindow::getTopLevelWindow (i)->sendLookAndFeelChange();

        updateLogoColour();
        refreshThemeSwatches();   // reflect the new colour on the swatch square
        repaint();
    }, popW, popH);

    // Anchor the picker on the swatch that was clicked, if visible, else on the Theme button.
    auto& anchor = btnThemeC1.isVisible() ? (prop == AlterState::kThemeColor2 ? btnThemeC2 : btnThemeC1)
                                          : btnTheme;
    juce::CallOutBox::launchAsynchronously (std::move (popup), anchor.getScreenBounds(), this);
}

void ControllerContent::refreshThemeSwatches()
{
    const bool custom = ((int) settings.getTree().getProperty (AlterState::kTheme, 0) == 2);
    const juce::Colour c1 ((uint32_t)(int) settings.getTree().getProperty (AlterState::kThemeColor1, (int) 0xFF3D96E7));
    const juce::Colour c2 ((uint32_t)(int) settings.getTree().getProperty (AlterState::kThemeColor2, (int) 0xFF6902D6));
    btnThemeC1.setColour (juce::TextButton::buttonColourId, c1);
    btnThemeC2.setColour (juce::TextButton::buttonColourId, c2);
    btnThemeC1.setVisible (custom);
    btnThemeC2.setVisible (custom);
}

void ControllerContent::updateLogoColour()
{
    // Re-parse from the (black) source so replaceColour always starts from black,
    // then tint to suit the active theme.
    logo = juce::Drawable::createFromImageData (kQspLogoTextSvg, std::strlen (kQspLogoTextSvg));
    if (logo == nullptr) return;

    const int theme = (int) settings.getTree().getProperty (AlterState::kTheme, 0);
    juce::Colour c;
    switch (theme)
    {
        case 1:  c = juce::Colours::white;       break;   // Dark  → white
        case 2:  c = AlterTheme::accent;         break;   // Custom → user primary colour
        case 3:  c = juce::Colour (0xFF2A3440);  break;   // White → dark slate (harmonised, softer than black)
        default: c = AlterTheme::textNormal;     break;   // Cyber → same light blue as the text
    }
    logoColour = c;
    logo->replaceColour (juce::Colours::black, c);
    repaint();
}

void ControllerContent::paint (juce::Graphics& g)
{
    AlterTheme::paintBackground (g, getLocalBounds().toFloat());

    // brand logotype (centred strip at the very bottom) — drawn before the early-out below
    if (logo != nullptr && ! logoBounds.isEmpty())
        logo->drawWithin (g, logoBounds.toFloat(),
                          juce::RectanglePlacement::centred
                        | juce::RectanglePlacement::onlyReduceInSize, 1.0f);

    // ── "liquid" editor card: tinted with the selected module's accent and
    //    visually connected to its row in the Create list ────────────────────
    if (selectedPanelId < 0) return;

    auto panel = settings.getPanelById (selectedPanelId);
    if (! panel.isValid()) return;

    const auto accent = panel.hasProperty (AlterState::kColor)
        ? juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor))
        : AlterTheme::pictonBlue;

    auto card = editorViewport.getBounds().toFloat().expanded (8.0f);

    // soft accent fill + glow border
    g.setGradientFill ({ accent.withAlpha (0.10f), card.getX(), card.getY(),
                         accent.withAlpha (0.02f), card.getX(), card.getBottom(), false });
    g.fillRoundedRectangle (card, 10.0f);
    AlterTheme::glowRect (g, card, accent, 0.45f, 10.0f);

    // flowing connector from the selected list row into the card
    const int rowIdx = panelList.getSelectedRow();
    if (rowIdx >= 0)
    {
        const auto rowPos = panelList.getRowPosition (rowIdx, true);
        const float x0 = (float) panelList.getRight() + 2.0f;
        const float y0 = (float) panelList.getY() + (float) rowPos.getCentreY();
        const float x1 = card.getX();
        const float y1 = card.getCentreY();
        const float mx = x0 + (x1 - x0) * 0.5f;

        juce::Path link;
        link.startNewSubPath (x0, y0);
        link.cubicTo (mx, y0, mx, y1, x1, y1);

        g.setColour (accent.withAlpha (0.55f));
        g.strokePath (link, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
        g.setColour (accent);
        g.fillEllipse (x0 - 3.0f, y0 - 3.0f, 6.0f, 6.0f);
    }
}

// =============================================================================
// Presets – save / load the whole controller setup (the AlterState tree)
// =============================================================================
void ControllerContent::savePreset()
{
    auto xml = settings.getTree().createXml();
    const juce::String xmlStr = (xml != nullptr) ? xml->toString() : juce::String();
    if (xmlStr.isEmpty()) return;

    auto dir = alterPresetsDir();
    dir.createDirectory();

    presetChooser = std::make_unique<juce::FileChooser> (
        "Save controller preset", dir.getChildFile ("MyPreset.alterpreset"), "*.alterpreset");

    const auto chooserFlags = juce::FileBrowserComponent::saveMode
                            | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::warnAboutOverwriting;

    presetChooser->launchAsync (chooserFlags, [xmlStr] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File()) return;
        if (! file.hasFileExtension ("alterpreset"))
            file = file.withFileExtension ("alterpreset");
        file.replaceWithText (xmlStr);
    });
}

void ControllerContent::loadPreset()
{
    auto dir = alterPresetsDir();
    if (! dir.isDirectory())
        dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

    presetChooser = std::make_unique<juce::FileChooser> (
        "Load controller preset", dir, "*.alterpreset");

    const auto chooserFlags = juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles;

    presetChooser->launchAsync (chooserFlags, [this] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (! file.existsAsFile()) return;

        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr) return;

        auto tree = juce::ValueTree::fromXml (*xml);

        // Presets written before Alchemy was renamed to Fusion still carry the old
        // keys; rewrite them here, once, so nothing downstream has to know.
        AlterState::migrateLegacyFusionNames (tree);

        if (tree.isValid() && tree.hasType (settings.getTree().getType()))
            applyPreset (tree);
        else
            juce::NativeMessageBox::showMessageBoxAsync (
                juce::MessageBoxIconType::WarningIcon, "Invalid preset",
                "That file is not a valid ALTER controller preset.", nullptr);
    });
}

void ControllerContent::applyPreset (const juce::ValueTree& preset)
{
    auto& tree = settings.getTree();

    // global properties (theme, hud layers, controller colour, instance, bounds…)
    tree.copyPropertiesFrom (preset, nullptr);

    // Replace the modules. Work with the EXISTING "panels" node (getPanelsRoot
    // creates one on demand, so removing/adding a whole node would duplicate it
    // and the load would silently fail). Clearing its children then copying the
    // saved panels in rebuilds the HUD reliably on the first try.
    auto panels = settings.getPanelsRoot();
    panels.removeAllChildren (nullptr);

    auto srcPanels = preset.getChildWithName (AlterState::kPanelsNode);
    if (srcPanels.isValid())
        for (int i = 0; i < srcPanels.getNumChildren(); ++i)
            panels.addChild (srcPanels.getChild (i).createCopy(), -1, nullptr);

    // re-skin: MainComponent applies the palette on the kTheme change above; here we
    // refresh the LookAndFeel + every window chrome and the controller widgets.
    if (auto* lnf = dynamic_cast<AlterLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel()))
        lnf->refreshFromTheme();
    for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
        juce::TopLevelWindow::getTopLevelWindow (i)->sendLookAndFeelChange();

    syncGlobalWidgetsFromState();   // theme (+custom colours), FFT bins, gain, on-top…

    selectedPanelId = -1;
    panelList.deselectAllRows();
    panelList.updateContent();
    panelList.repaint();
    refreshRightEditorFromSelection();
    resized();
    repaint();
}

void ControllerContent::launchColourPicker (const juce::Identifier& prop,
                                            juce::Label& preview, juce::Component& anchor)
{
    juce::Colour initial = AlterTheme::pictonBlue;
    if (selectedPanelId >= 0)
    {
        auto p = settings.getPanelById (selectedPanelId);
        if (p.isValid() && p.hasProperty (prop))
            initial = juce::Colour ((uint32_t)(int) p.getProperty (prop));
    }

    // Size the popup to fit inside the controller so OK/Cancel are always reachable,
    // even when the controller window is small.
    const int popW = juce::jlimit (220, 300, getWidth()  - 30);
    const int popH = juce::jlimit (200, 340, getHeight() - 30);

    auto popup = std::make_unique<ColorPopup> (initial, [this, prop, &preview] (juce::Colour c)
    {
        if (selectedPanelId < 0) return;
        auto p = settings.getPanelById (selectedPanelId);
        if (! p.isValid()) return;

        p.setProperty (prop, (int) c.getARGB(), nullptr);
        preview.setColour (juce::Label::backgroundColourId, c);
        repaint();   // editor card + list dot follow the new accent
    }, popW, popH);

    juce::CallOutBox::launchAsynchronously (std::move (popup), anchor.getScreenBounds(), this);
}

void ControllerContent::setInfoWindowAlwaysOnTop (bool on)
{
    if (currentInfoWindow != nullptr)
        currentInfoWindow->setAlwaysOnTop (on);
}

// ===== ListBoxModel =====

// ── Block-bubble row model: panels grouped per block + a placeholder row for
//    every EMPTY block (its bubble stays visible; modules can be dropped in). ──
std::vector<ControllerContent::RowRef> ControllerContent::rowModel() const
{
    std::vector<RowRef> rows;
    auto panels = settings.getPanelsRoot();
    const int n      = panels.isValid() ? panels.getNumChildren() : 0;
    const int layers = settings.hudLayers();

    if (layers <= 1)
    {
        for (int i = 0; i < n; ++i)
            rows.push_back ({ i, 0, false });
        return rows;
    }

    for (int layer = 0; layer < layers; ++layer)
    {
        bool any = false;
        for (int i = 0; i < n; ++i)
            if ((int) panels.getChild (i).getProperty (AlterState::kLayer, 0) == layer)
            {
                rows.push_back ({ i, layer, false });
                any = true;
            }
        if (! any)
            rows.push_back ({ -1, layer, true });   // empty block → visible bubble
    }
    return rows;
}

int ControllerContent::rowForPanelId (int panelId) const
{
    const auto rows = rowModel();
    auto panels = settings.getPanelsRoot();
    for (int i = 0; i < (int) rows.size(); ++i)
        if (! rows[(size_t) i].placeholder
            && (int) panels.getChild (rows[(size_t) i].panelIndex)
                            .getProperty (AlterState::kId) == panelId)
            return i;
    return -1;
}

void ControllerContent::deselectModule()
{
    selectedPanelId = -1;
    panelList.deselectAllRows();
    refreshRightEditorFromSelection();
    resized();
    repaint();
}

void ControllerContent::backgroundClicked (const juce::MouseEvent&) { deselectModule(); }
void ControllerContent::mouseDown (const juce::MouseEvent&)         { deselectModule(); }

int ControllerContent::getNumRows()
{
    return (int) rowModel().size();
}

juce::var ControllerContent::getDragSourceDescription (const juce::SparseSet<int>& selectedRows)
{
    if (selectedRows.size() == 1)
        return "panel_" + juce::String (selectedRows[0]);
    return {};
}

void ControllerContent::paintListBoxItem (int rowNumber, juce::Graphics& g,
                                          int width, int height, bool rowIsSelected)
{
    auto panels = settings.getPanelsRoot();
    const auto rows = rowModel();
    if (! panels.isValid() || rowNumber < 0 || rowNumber >= (int) rows.size())
        return;

    const auto ref = rows[(size_t) rowNumber];

    // ── NEON block bubble (drawn for every row of the block, incl. empty ones):
    //    soft accent-style gradient fill + glow frame — the same visual language
    //    as the editor card on the right. Middle rows extend the rounded rect
    //    past their edges (clipped to the row) so the group fuses into ONE bubble.
    auto drawBubble = [&]()
    {
        if (settings.hudLayers() <= 1) return;

        const auto shade = AlterTheme::blockShade (ref.layer);
        auto layerOfRow = [&rows] (int r) -> int
        {
            return (r < 0 || r >= (int) rows.size()) ? -1 : rows[(size_t) r].layer;
        };
        const bool firstOfGroup = layerOfRow (rowNumber - 1) != ref.layer;
        const bool lastOfGroup  = layerOfRow (rowNumber + 1) != ref.layer;

        const float rad = 8.0f;
        const float top    = firstOfGroup ? 2.0f : -rad - 2.0f;
        const float bottom = lastOfGroup  ? (float) height - 2.0f : (float) height + rad + 2.0f;
        juce::Rectangle<float> r (2.0f, top, (float) width - 4.0f, bottom - top);

        g.saveState();
        g.reduceClipRegion (0, 0, width, height);

        g.setGradientFill ({ shade.withAlpha (0.12f), 0.0f, top,
                             shade.withAlpha (0.03f), 0.0f, bottom, false });
        g.fillRoundedRectangle (r, rad);
        AlterTheme::glowRect (g, r, shade, 0.45f, rad);   // neon frame

        g.restoreState();

        g.setColour (shade.withAlpha (0.9f));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("B" + juce::String (ref.layer + 1),
                    width - 26, 0, 20, height, juce::Justification::centredRight);
    };

    if (ref.placeholder)
    {
        // EMPTY block: the bubble alone + a drop hint
        drawBubble();
        g.setColour (AlterTheme::blockShade (ref.layer).withAlpha (0.75f));
        g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Italic")));
        g.drawText ("empty block - drop modules here", 12, 0, width - 44, height,
                    juce::Justification::centredLeft, false);

        if (isDragging && rowNumber == dragInsertIndex)   // drop indicator still works
        {
            g.setColour (AlterTheme::cerise);
            g.fillRect (0, 0, width, 3);
        }
        return;
    }

    auto p = panels.getChild (ref.panelIndex);
    const int id = (int) p.getProperty (AlterState::kId);
    const auto type = p.getProperty (AlterState::kType).toString();
    const bool hidden = (bool) p.getProperty (AlterState::kHidden, false);

    const auto accent = p.hasProperty (AlterState::kColor)
        ? juce::Colour ((uint32_t)(int) p.getProperty (AlterState::kColor))
        : AlterTheme::pictonBlue;

    // Highlight being dragged
    if (isDragging && rowNumber == draggedRow)
    {
        g.setColour (AlterTheme::electricViolet.withAlpha (0.25f));
        g.fillAll();
    }
    else if (rowIsSelected)
    {
        // selection flows in the module's own accent colour (links it to the editor card)
        g.setGradientFill ({ accent.withAlpha (0.45f), 0.0f, 0.0f,
                             accent.withAlpha (0.10f), (float) width, 0.0f, false });
        g.fillAll();
        g.setColour (accent.withAlpha (0.9f));
        g.fillRect (0, 0, 3, height);   // accent ribbon on the left edge
    }

    // Draw drop indicator line
    if (isDragging && rowNumber == dragInsertIndex)
    {
        g.setColour (AlterTheme::cerise);
        g.fillRect (0, 0, width, 3);
    }

    // module accent dot
    if (p.hasProperty (AlterState::kColor))
    {
        g.setColour (juce::Colour ((uint32_t)(int) p.getProperty (AlterState::kColor)));
        g.fillEllipse (8.0f, (float) height * 0.5f - 3.5f, 7.0f, 7.0f);
    }

    auto textCol = rowIsSelected ? AlterTheme::textBright : AlterTheme::textNormal;
    if (hidden) textCol = AlterTheme::textDim.withAlpha (0.6f);   // visually greyed = hidden from HUD
    g.setColour (textCol);
    juce::String displayName = type;
    if (type == "oscillator" || type == "oscilator") displayName = "Oscilloscope";
    g.drawText (displayName.toUpperCase() + "  " + juce::String (id) + (hidden ? "   (hidden)" : ""),
                22, 0, width - 28, height, juce::Justification::centredLeft);

    // NEON block bubble on top (frame + badge; fill is translucent so the row
    // content stays readable)
    drawBubble();
}

void ControllerContent::showRowMenu (int rowNumber)
{
    auto panels = settings.getPanelsRoot();
    const auto rows = rowModel();
    if (! panels.isValid() || rowNumber < 0 || rowNumber >= (int) rows.size()
        || rows[(size_t) rowNumber].placeholder)
        return;

    const int  pIdx    = rows[(size_t) rowNumber].panelIndex;
    const int  panelId = (int) panels.getChild (pIdx).getProperty (AlterState::kId);
    const bool hidden  = (bool) panels.getChild (pIdx).getProperty (AlterState::kHidden, false);

    juce::PopupMenu m;
    m.addItem (1, "Destroy");
    m.addItem (2, hidden ? "Show in HUD" : "Hide from HUD");
    m.addItem (3, "Duplicate");
    m.showMenuAsync (juce::PopupMenu::Options(), [this, panelId] (int r)
    {
        auto pp = settings.getPanelById (panelId);
        if (! pp.isValid()) return;

        if (r == 1)
        {
            settings.removePanelById (panelId);
            if (selectedPanelId == panelId) selectedPanelId = -1;
            panelList.deselectAllRows();
            panelList.updateContent();
            resized();
            repaint();
        }
        else if (r == 2)
        {
            pp.setProperty (AlterState::kHidden,
                            ! (bool) pp.getProperty (AlterState::kHidden, false), nullptr);
            panelList.repaint();
        }
        else if (r == 3)
        {
            const int newId = settings.duplicatePanel (panelId);   // copy below, same settings, new id
            panelList.updateContent();
            if (newId > 0)
            {
                selectedPanelId = newId;
                const int row = rowForPanelId (newId);
                if (row >= 0)
                    panelList.selectRow (row);
                refreshRightEditorFromSelection();
            }
            resized();
            repaint();
        }
    });
}

void ControllerContent::selectedRowsChanged (int lastRowSelected)
{
    auto panels = settings.getPanelsRoot();
    const auto rows = rowModel();

    if (! panels.isValid() || lastRowSelected < 0 || lastRowSelected >= (int) rows.size()
        || rows[(size_t) lastRowSelected].placeholder)     // empty-block row → deselect
    {
        selectedPanelId = -1;
        refreshRightEditorFromSelection();
        resized();
        repaint();
        return;
    }

    auto p = panels.getChild (rows[(size_t) lastRowSelected].panelIndex);
    selectedPanelId = (int) p.getProperty (AlterState::kId);

    refreshRightEditorFromSelection();
    resized();
    repaint();
}

juce::Component* ControllerContent::refreshComponentForRow (int row, bool, juce::Component* existing)
{
    auto* item = dynamic_cast<DraggableListItem*> (existing);
    if (item == nullptr)
        item = new DraggableListItem (*this);

    item->setRow (row);
    return item;
}

void ControllerContent::finalizeDrag()
{
    if (! isDragging || draggedRow < 0) return;

    auto panels = settings.getPanelsRoot();
    if (! panels.isValid()) return;

    const auto rows   = rowModel();
    const int numRows = (int) rows.size();

    auto reset = [this]
    {
        isDragging = false;
        draggedRow = -1;
        dragInsertIndex = -1;
        panelList.updateContent();
        panelList.repaint();
    };

    if (draggedRow >= numRows || rows[(size_t) draggedRow].placeholder)
        { reset(); return; }

    const int srcPanel = rows[(size_t) draggedRow].panelIndex;
    auto dragged = panels.getChild (srcPanel);   // grab BEFORE any move

    // Which BUBBLE (block) was the module dropped into? Probe the row at the
    // insert position; dropping below the last row targets the last bubble.
    // Dropping onto an EMPTY block's placeholder moves the module into it.
    const int probeRow    = juce::jlimit (0, numRows - 1, dragInsertIndex);
    const int targetLayer = (settings.hudLayers() > 1) ? rows[(size_t) probeRow].layer : 0;

    // Position within the panels tree = number of panel-rows above the insert line
    int targetPanelIndex = 0;
    for (int r = 0; r < juce::jmin (dragInsertIndex, numRows); ++r)
        if (! rows[(size_t) r].placeholder)
            ++targetPanelIndex;

    if (targetPanelIndex > srcPanel)
        --targetPanelIndex;
    targetPanelIndex = juce::jlimit (0, juce::jmax (0, panels.getNumChildren() - 1), targetPanelIndex);

    if (targetPanelIndex != srcPanel)
        panels.moveChild (srcPanel, targetPanelIndex, nullptr);

    if (settings.hudLayers() > 1 && dragged.isValid()
        && (int) dragged.getProperty (AlterState::kLayer, 0) != targetLayer)
        dragged.setProperty (AlterState::kLayer, targetLayer, nullptr);

    settings.sortPanelsByLayer();   // keep every bubble contiguous

    reset();

    // re-select the dragged module wherever it ended up
    if (dragged.isValid())
    {
        const int row = rowForPanelId ((int) dragged.getProperty (AlterState::kId));
        if (row >= 0)
            panelList.selectRow (row);
    }
}

// ===== helpers =====

// =============================================================================
//  Fusion layer pickers
//
//  A layer is one of the modules that already exist, so the list is built from
//  the panel tree every time it is shown. Item id 1 is "(none)" and every other
//  item id is the panel id + 1 — ComboBox ids must be non-zero, and offsetting is
//  cheaper to reason about than a lookup table that could fall out of step.
// =============================================================================
void ControllerContent::refreshFusionLayerBox (int slot)
{
    if (slot < 0 || slot >= AlterState::kMaxFusionLayers) return;

    auto& box = cbFusionLayer[slot];
    box.clear (juce::dontSendNotification);
    box.addItem ("(none)", 1);

    if (selectedPanelId < 0) return;

    const int myId = settings.fusionLayerIdInSlot (selectedPanelId, slot);

    auto panels = settings.getPanelsRoot();
    for (int i = 0; i < panels.getNumChildren(); ++i)
    {
        auto p    = panels.getChild (i);
        const int id   = (int) p.getProperty (AlterState::kId, -1);
        const auto typ = p.getProperty (AlterState::kType).toString();

        if (id < 0 || id == selectedPanelId) continue;
        if (typ == "fusion") continue;                       // no nesting

        // Offer the free modules plus whatever THIS slot already holds. A module
        // taken by another slot (or another Fusion) is not offered, so the same
        // module can never end up as two layers at once.
        const int host = (int) p.getProperty (AlterState::kFusionHost, 0);
        if (host != 0 && id != myId) continue;

        juce::String name = typ;
        if (typ == "oscillator" || typ == "oscilator") name = "Oscilloscope";
        else if (typ == "audiometer" || typ == "rms")  name = "Audio Meter";
        else if (typ == "toneanalyzer")                name = "Tone Analyzer";
        else name = typ.substring (0, 1).toUpperCase() + typ.substring (1);

        // The id is the module's REAL id, the same one the list on the left shows —
        // it counts every module ever created, so it is routinely something like 67
        // even when only three exist. That is confusing in a picker of three slots,
        // so it is only shown when it is actually needed to tell two of the same
        // kind apart.
        int sameType = 0;
        for (int j = 0; j < panels.getNumChildren(); ++j)
            if (panels.getChild (j).getProperty (AlterState::kType).toString() == typ)
                ++sameType;

        box.addItem (sameType > 1 ? name + "  #" + juce::String (id) : name, id + 1);
    }

    box.setSelectedId (myId > 0 ? myId + 1 : 1, juce::dontSendNotification);
}

// =============================================================================
//  Fusion fold blocks
//
//  A fusion is four blocks of a dozen controls, and all of it at once cannot be
//  read — you cannot compare two layers when neither fits on the screen. Each
//  block folds to a single header row naming what is in it.
//
//  The open/closed flag lives in the TREE, not in the controller. It therefore
//  survives closing the window and travels with a saved preset, which matters
//  because a stack of three is unreadable if every slot springs back open the
//  moment it is loaded. It costs one bool per slot and is not a setting anything
//  downstream reads, so nothing has to be told when it changes.
// =============================================================================
bool ControllerContent::isFusionBlockOpen (int slot) const
{
    if (selectedPanelId < 0) return true;

    auto panel = settings.getPanelById (selectedPanelId);
    if (! panel.isValid()) return true;

    // Open is the default: a fusion built before folding existed, or one just
    // created, should explain itself rather than hide.
    if (slot < 0)
        return (bool) panel.getProperty (AlterState::kFusionGlobalOpen, true);

    if (slot >= AlterState::kMaxFusionLayers) return true;

    return (bool) panel.getProperty (
        AlterState::fusionLayerProp (AlterState::kFusionLayOpen, slot), true);
}

void ControllerContent::toggleFusionBlock (int slot)
{
    if (selectedPanelId < 0) return;

    auto panel = settings.getPanelById (selectedPanelId);
    if (! panel.isValid()) return;

    const bool nowOpen = ! isFusionBlockOpen (slot);

    if (slot < 0)
        panel.setProperty (AlterState::kFusionGlobalOpen, nowOpen, nullptr);
    else if (slot < AlterState::kMaxFusionLayers)
        panel.setProperty (AlterState::fusionLayerProp (AlterState::kFusionLayOpen, slot),
                           nowOpen, nullptr);
    else
        return;

    // refreshRightEditorFromSelection decides what is visible AND lays it out, in
    // that order — which is the only order that works, since resized() places only
    // what is visible.
    refreshRightEditorFromSelection();
}

void ControllerContent::refreshFusionHeaders()
{
    // What a header says is WHAT IS IN THE SLOT, not "Layer A". A folded stack
    // has to still read as a stack, and three rows saying A, B, C would be three
    // rows saying nothing you did not already know from their order.
    auto slotName = [this] (int slot) -> juce::String
    {
        if (selectedPanelId < 0) return "empty";

        const int id = settings.fusionLayerIdInSlot (selectedPanelId, slot);
        if (id <= 0) return "empty";

        auto p = settings.getPanelById (id);
        if (! p.isValid()) return "empty";

        const auto typ = p.getProperty (AlterState::kType).toString();
        if (typ == "oscillator" || typ == "oscilator") return "Oscilloscope";
        if (typ == "audiometer" || typ == "rms")       return "Audio Meter";
        if (typ == "toneanalyzer")                     return "Tone Analyzer";
        return typ.substring (0, 1).toUpperCase() + typ.substring (1);
    };

    for (int i = 0; i < AlterState::kMaxFusionLayers; ++i)
    {
        const juce::String arrow = isFusionBlockOpen (i) ? juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe"))
                                                         : juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb8"));
        btnFusionLayHead[i].setButtonText (arrow + "  Layer " + juce::String ((char) ('A' + i))
                                           + "  -  " + slotName (i));
    }

    const juce::String garrow = isFusionBlockOpen (-1) ? juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe"))
                                                       : juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xb8"));
    btnFusionGlobalHead.setButtonText (garrow + "  Global  -  the whole stack");
}

void ControllerContent::applyFusionLayerPick (int slot, int chosenPanelId)
{
    if (selectedPanelId < 0 || slot < 0 || slot >= AlterState::kMaxFusionLayers) return;

    const int had = settings.fusionLayerIdInSlot (selectedPanelId, slot);

    if (had == chosenPanelId) return;

    // Release first: without this, picking a replacement while the Fusion is
    // already full would be refused by setFusionLayer and the slot would silently
    // keep the old module.
    if (had > 0)
        settings.setFusionLayer (had, 0);

    // Into THIS slot, explicitly. The stack is bottom-up and slot A is the base, so
    // which box the user picked in is the whole answer to what gets composited onto
    // what — it must not be re-derived from anything else.
    if (chosenPanelId > 0 && ! settings.setFusionLayer (chosenPanelId, selectedPanelId, slot))
    {
        // Refused (full, or a module that cannot be a layer). Put the old one back
        // so the picker and the state cannot disagree.
        if (had > 0) settings.setFusionLayer (had, selectedPanelId, slot);
    }

    refreshRightEditorFromSelection();
}

void ControllerContent::setEditorVisible (const juce::String& type)
{
    const bool rms     = (type == "rms" || type == "audiometer");
    const bool spec    = (type == "spectrum");
    const bool osc     = (type == "oscillator" || type == "oscilator");
    const bool syn     = (type == "synesthesia");
    const bool chladni = (type == "chladni");
    const bool tone    = (type == "toneanalyzer");
    const bool spectro = (type == "spectrogram");
    const bool stereo  = (type == "stereoscope");
    const bool geo     = (type == "geometry");
    const bool fusion     = (type == "fusion");
    const bool anySelected = rms || spec || osc || syn || chladni || tone || spectro || stereo || geo || fusion;

    // RMS
    lblRmsMode.setVisible   (rms);
    cbRmsPeakMode.setVisible(rms);
    lblMeterView.setVisible (rms);
    cbMeterView.setVisible  (rms);
    lblRmsSmooth.setVisible (rms);
    sRmsSmooth.setVisible   (rms);
    lblColorMode.setVisible (rms);
    cbColorMode.setVisible  (rms);
    tbMeterToneColor.setVisible (rms);
    tbMeterTwoBars.setVisible   (rms);
    // The shade combo only exists inside tone mode, and the clip / L/R rows only in
    // the views that have something to annotate — all narrowed in resized().
    lblMeterToneShade.setVisible (false);
    cbMeterToneShade.setVisible  (false);
    // Trend controls visibility managed in resized()
    btnMeasureStart.setVisible  (false);
    btnMeasureStop.setVisible   (false);
    lblMeasureStatus.setVisible (false);

    // Spectrum
    lblSpecSmooth.setVisible    (spec);
    sSpecSmooth.setVisible      (spec);
    tbSpecMeasurement.setVisible(spec);
    tbSpecPeakHold.setVisible   (spec);
    tbSpecConstantQ.setVisible  (spec);
    lblSpecPsycho.setVisible    (spec);
    cbSpecPsycho.setVisible     (spec);
    lblSpecRef.setVisible       (spec);
    cbSpecRef.setVisible        (spec);
    tbSpecStereo.setVisible     (spec);
    lblSpecLrColor.setVisible   (spec);   // refined in resized() (stereo only)
    cbSpecLrColor.setVisible    (spec);
    // phon slider: shown in resized() only when spectrum + ISO226
    if (! spec)
    {
        lblSpecPhon.setVisible (false);
        sSpecPhon.setVisible   (false);
    }

    // Oscillator (zoom vs long-term window refined in resized())
    lblOscSmooth.setVisible (osc);
    sOscSmooth.setVisible   (osc);
    tbFill.setVisible       (osc);
    lblDisplayMode.setVisible(osc);
    cbDisplayMode.setVisible (osc);
    lblOscZoom.setVisible   (osc);
    sOscZoom.setVisible     (osc);
    lblOscTerm.setVisible   (osc);
    cbOscTerm.setVisible    (osc);
    lblOscLtWin.setVisible  (osc);
    sOscLtWin.setVisible    (osc);
    tbOscSymmetry.setVisible(false);   // Level history (its only use) moved to the Audio Meter

    // Synesthesia (sliders shared with the Geometry module)
    const bool synOrGeo = syn || geo;
    lblSynSmooth.setVisible (synOrGeo);
    sSynSmooth.setVisible   (synOrGeo);
    lblSynGhost.setVisible  (syn);
    sSynGhost.setVisible    (syn);
    lblZoom.setVisible      (synOrGeo);   // Fusion has its own zoom, inside the Mirror stage
    sZoom.setVisible        (synOrGeo);
    lblRotation.setVisible  (synOrGeo);
    sRotation.setVisible    (synOrGeo);
    lblSymmetry.setVisible  (synOrGeo);     // Symmetry: kaleidoscope (Syn) / even placement (Geo)
    sSymmetry.setVisible    (synOrGeo);
    tbSynMirror.setVisible  (synOrGeo);     // Mirror toggle shared by Synesthesia + Geometry
    lblSaturation.setVisible(synOrGeo);
    sSaturation.setVisible  (synOrGeo);
    lblSynBright.setVisible (synOrGeo);     // brightness: shared by Synesthesia + Geometry
    sSynBright.setVisible   (synOrGeo);
    lblBloom.setVisible     (synOrGeo);
    sBloom.setVisible       (synOrGeo);
    lblSpeed.setVisible     (synOrGeo || fusion);   // Fusion: drift of the fold / weave phase
    sSpeed.setVisible       (synOrGeo || fusion);
    lblSynReact.setVisible  (syn);
    sSynReact.setVisible    (syn);
    lblSynChange.setVisible (syn);
    sSynChange.setVisible   (syn);
    lblSynTransmute.setVisible (syn);
    sSynTransmute.setVisible   (syn);
    lblSynClear.setVisible  (syn);
    sSynClear.setVisible    (syn);
    lblSynDenoise.setVisible (syn);
    sSynDenoise.setVisible   (syn);
    lblSynTunnel.setVisible  (syn);
    sSynTunnel.setVisible    (syn);
    lblSynVortex.setVisible  (syn);
    sSynVortex.setVisible    (syn);
    tbSynBpmSync.setVisible  (syn);
    if (! syn)   // BPM tempo/beat-div rows are shown by the synesthesia layout only when sync is on
    {
        lblSynBpm.setVisible (false);     sSynBpm.setVisible (false);
        lblSynBeatDiv.setVisible (false); sSynBeatDiv.setVisible (false);
    }
    tbSynTone.setVisible    (syn);
    tbSynTwist.setVisible   (syn);   // resized() narrows this to "syn AND tone mode on"

    // Geometry-specific
    lblGeoTri.setVisible        (geo);
    sGeoTri.setVisible          (geo);
    lblGeoSquare.setVisible     (geo);
    sGeoSquare.setVisible       (geo);
    lblGeoCircle.setVisible     (geo);
    sGeoCircle.setVisible       (geo);
    lblGeoComplexity.setVisible (geo);
    sGeoComplexity.setVisible   (geo);
    lblGeoRandom.setVisible      (geo);
    sGeoRandom.setVisible        (geo);
    lblGeoReact.setVisible       (geo);
    sGeoReact.setVisible         (geo);
    lblGeoDepth.setVisible       (geo);
    sGeoDepth.setVisible         (geo);
    lblGeoTunnel.setVisible      (geo);
    sGeoTunnel.setVisible        (geo);
    lblGeoAperture.setVisible    (geo);
    sGeoAperture.setVisible      (geo);
    lblGeoGlobalRot.setVisible   (geo);
    sGeoGlobalRot.setVisible     (geo);
    tbGeoBpmSync.setVisible      (geo);
    // BPM tempo + beat-division rows are shown by the geometry layout only when BPM sync is on
    if (! geo)
    {
        lblGeoBpm.setVisible (false);     sGeoBpm.setVisible (false);
        lblGeoBeatDiv.setVisible (false); sGeoBeatDiv.setVisible (false);
    }
    tbGeoTone.setVisible        (geo);
    tbGeoTwist.setVisible       (geo);   // resized() narrows this to "geo AND tone mode on"

    // Chladni
    tbChladniReactive.setVisible  (chladni);
    lblChladniPreset.setVisible   (chladni);
    cbChladniPreset.setVisible    (chladni);
    lblChladniM.setVisible        (chladni);
    sChladniM.setVisible          (chladni);
    lblChladniN.setVisible        (chladni);
    sChladniN.setVisible          (chladni);
    lblChladniShift.setVisible    (chladni);
    sChladniShift.setVisible      (chladni);
    lblChladniAR.setVisible       (chladni);
    sChladniAR.setVisible         (chladni);
    lblChladniSharp.setVisible    (chladni);
    sChladniSharp.setVisible      (chladni);
    lblChladniParticles.setVisible(chladni);
    sChladniParticles.setVisible  (chladni);
    lblChladniMaterial.setVisible (chladni);
    cbChladniMaterial.setVisible  (chladni);
    tbChladniToneColor.setVisible (chladni);
    tbChladniTwist.setVisible     (chladni);   // resized() narrows this to "chladni AND tone mode on"

    // ToneAnalyzer
    lblToneSens.setVisible (tone);
    sToneSens.setVisible   (tone);
    tbToneTuner.setVisible (tone);

    // Spectrogram
    lblSpectroSmooth.setVisible (spectro);
    sSpectroSmooth.setVisible   (spectro);
    lblSpectroWin.setVisible    (spectro);
    sSpectroWin.setVisible      (spectro);
    lblSpectroFill.setVisible   (spectro);
    sSpectroFill.setVisible     (spectro);
    tbSpectroMirror.setVisible  (spectro);
    tbSpectroConstantQ.setVisible (spectro);
    tbSpectroReassign.setVisible (spectro);
    lblSpectroColorMode.setVisible (spectro);
    cbSpectroColorMode.setVisible  (spectro);

    // Shared colour-by-tone widgets. resized() narrows these further — the twist
    // and smooth rows only appear while tone colour is actually on, and Spectrogram
    // suppresses the checkbox because its combo already carries the choice.
    {
        // The Audio Meter is in this set for the twist and 'Tone smooth' rows only;
        // its checkbox is tbMeterToneColor, because the shared one writes kColorMode
        // and on that module kColorMode is already the zone shade.
        const bool toneCapable = spec || osc || stereo || spectro || tone || rms;
        tbToneColor.setVisible   (toneCapable && ! spectro && ! rms);
        tbToneTwist.setVisible   (toneCapable);
        // Chladni has its own tone checkbox and twist but no smoothing control of
        // its own, so it borrows this slider — hence the extra term here.
        lblToneSmooth.setVisible (toneCapable || chladni);
        sToneSmooth.setVisible   (toneCapable || chladni);
    }

    // Level history / Oscilloscope waveform annotations — narrowed per view in
    // resized(), which knows which term or meter mode is actually selected.
    tbClipZone.setVisible (false);
    lblLrColor.setVisible  (false);
    cbLrColor.setVisible   (false);

    // Stereoscope (Particles toggle + Density row refined in resized() per mode)
    lblStereoMode.setVisible   (stereo);
    cbStereoMode.setVisible    (stereo);
    lblStereoSmooth.setVisible (stereo);
    sStereoSmooth.setVisible   (stereo);
    tbStereoParticles.setVisible (stereo);
    lblStereoDensity.setVisible  (stereo);
    sStereoDensity.setVisible    (stereo);
    tbStereoCtrlBins.setVisible  (stereo);
    lblStereoBright.setVisible   (stereo);
    sStereoBright.setVisible     (stereo);
    lblStereoLineW.setVisible    (stereo);
    sStereoLineW.setVisible      (stereo);
    lblStereoPointSize.setVisible (stereo);
    sStereoPointSize.setVisible   (stereo);

    // Fusion. Segments and Angle only mean something to the folding modes, so
    // they follow the selected fusion rather than the module type: Mirror uses the
    // wedge count and the fold angle, Weave the band count and the band direction,
    // and Merge/Warp have neither.
    {
        // Read the stage switches from the STATE, not from the toggles: this runs
        // BEFORE the widgets are synced to a newly selected panel, so a toggle still
        // holds the previously selected module's value.
        bool warp = false;
        bool liquid = false;
        int  mirrorAxes = 0;
        int  layerBlend[AlterState::kMaxFusionLayers] = { 0, 0, 0 };
        bool slotFilled[AlterState::kMaxFusionLayers] = { false, false, false };
        bool layerOpen [AlterState::kMaxFusionLayers] = { true, true, true };
        int  layerMirror[AlterState::kMaxFusionLayers] = { 0, 0, 0 };
        bool globalOpen = true;

        if (fusion && selectedPanelId >= 0)
            if (auto panel = settings.getPanelById (selectedPanelId); panel.isValid())
            {
                warp       = (bool) panel.getProperty (AlterState::kFusionWarp,   false);
                liquid     = (bool) panel.getProperty (AlterState::kFusionLiquid, false);
                mirrorAxes = (int)  panel.getProperty (AlterState::kFusionMirror, 0);
                globalOpen = isFusionBlockOpen (-1);

                for (int i = 0; i < AlterState::kMaxFusionLayers; ++i)
                {
                    layerBlend[i] = (int) panel.getProperty (
                        AlterState::fusionLayerProp (AlterState::kFusionLayBlend, i), 0);
                    slotFilled[i] = settings.fusionLayerIdInSlot (selectedPanelId, i) > 0;
                    layerOpen[i]  = isFusionBlockOpen (i);
                    layerMirror[i] = (int) panel.getProperty (
                        AlterState::fusionLayerProp (AlterState::kFusionLayMirror, i), 0);
                }
            }

        for (int i = 0; i < AlterState::kMaxFusionLayers; ++i)
        {
            // FOLDED beats everything below it. A layer's header is the one row
            // that stays: it names what is in the slot, so a folded stack still
            // reads as a stack rather than as three anonymous bars.
            btnFusionLayHead[i].setVisible (fusion);

            const bool open = fusion && layerOpen[i];

            // A slot's settings only exist once something is IN it — an empty slot
            // showing a full row of knobs that affect nothing is just noise. The
            // picker itself survives an empty slot, because it is how you fill it.
            const bool filled = open && slotFilled[i];

            lblFusionLayer[i].setVisible (open);
            cbFusionLayer[i].setVisible  (open);

            // Slot A is the base: nothing is underneath it, so it has no blend.
            cbFusionBlend[i].setVisible (filled && i > 0);

            lblFusionOpacity[i].setVisible (filled);  sFusionOpacity[i].setVisible (filled);
            lblFusionScale[i].setVisible   (filled);  sFusionScale[i].setVisible   (filled);

            const int blend = filled ? layerBlend[i] : 0;
            const bool isMerge = filled && i > 0 && blend == 1;
            const bool isWeave = filled && i > 0 && blend == 2;

            lblFusionAmount[i].setVisible (isMerge || isWeave);
            sFusionAmount[i].setVisible   (isMerge || isWeave);
            lblFusionAmount[i].setText (isMerge ? "Bias" : "Share", juce::dontSendNotification);

            lblFusionBands[i].setVisible (isWeave);  sFusionBands[i].setVisible (isWeave);
            lblFusionAngle[i].setVisible (isWeave);  sFusionAngle[i].setVisible (isWeave);

            // The layer's own post chain. Axis follows the axis COUNT, exactly as
            // it does globally: an angle for a fold that is not happening is a
            // control that answers a question nobody asked.
            // From the STATE, not from the slider: this runs BEFORE the widgets are
            // synced to a newly selected panel, so the slider still holds the
            // previously selected fusion's value.
            const bool layMirror = filled && layerMirror[i] > 0;

            lblFusionLayMirror[i].setVisible (filled);      sFusionLayMirror[i].setVisible (filled);
            lblFusionLayMirrorAng[i].setVisible (layMirror); sFusionLayMirrorAng[i].setVisible (layMirror);
            lblFusionLaySymmetry[i].setVisible (filled);    sFusionLaySymmetry[i].setVisible (filled);
            lblFusionLaySpin[i].setVisible (filled);        sFusionLaySpin[i].setVisible (filled);
            lblFusionLaySpeed[i].setVisible (filled);       sFusionLaySpeed[i].setVisible (filled);
            lblFusionLayZoom[i].setVisible (filled);        sFusionLayZoom[i].setVisible (filled);
        }

        btnFusionGlobalHead.setVisible (fusion);

        const bool g = fusion && globalOpen;

        tbFusionWarp.setVisible      (g);
        lblFusionSymmetry.setVisible (g);   sFusionSymmetry.setVisible (g);
        lblFusionMirror.setVisible   (g);   sFusionMirror.setVisible   (g);
        lblFusionMirrorAngle.setVisible (g && mirrorAxes > 0);
        sFusionMirrorAngle.setVisible   (g && mirrorAxes > 0);
        lblFusionSpin.setVisible     (g);   sFusionSpin.setVisible  (g);
        lblFusionZoom.setVisible     (g);   sFusionZoom.setVisible  (g);
        lblFusionVortex.setVisible   (g);   sFusionVortex.setVisible (g);
        lblFusionReact.setVisible    (g);   sFusionReact.setVisible (g);

        lblFusionGlobLayers.setVisible (g);
        for (int i = 0; i < AlterState::kMaxFusionLayers; ++i)
            tbFusionGlobLayer[i].setVisible (g);

        tbFusionLiquid.setVisible (g);
        tbFusionTunnel.setVisible (g);
        lblFusionLiquidAmt.setVisible    (g && liquid);  sFusionLiquidAmt.setVisible    (g && liquid);
        lblFusionLiquidSmooth.setVisible (g && liquid);  sFusionLiquidSmooth.setVisible (g && liquid);
        lblFusionLiquidDenoise.setVisible (g && liquid); sFusionLiquidDenoise.setVisible (g && liquid);

        lblFusionDetail.setVisible   (g);   cbFusionDetail.setVisible (g);

        const bool w = g && warp;
        lblFusionWarpAmt.setVisible (w);    sFusionWarpAmt.setVisible (w);
        lblFusionWarpSwirl.setVisible (w);  sFusionWarpSwirl.setVisible (w);
        lblFusionWarpSmooth.setVisible (w); sFusionWarpSmooth.setVisible (w);
        lblFusionWarpDenoise.setVisible (w); sFusionWarpDenoise.setVisible (w);
        lblFusionWarpSrc.setVisible (w);    cbFusionWarpSrc.setVisible (w);

        // Speed is a shared slider (Synesthesia and Geometry use it too), so its
        // visibility was decided far above — narrow it here, or it would float
        // alone under a folded global block.
        if (fusion)
        {
            lblSpeed.setVisible (g);
            sSpeed.setVisible   (g);
        }

        if (fusion)
            refreshFusionHeaders();
    }

    // Color (synesthesia excluded; geometry uses it as the base colour)
    const bool showColor = rms || spec || osc || chladni || tone || spectro || stereo || geo;
    lblColor.setVisible        (showColor);
    lblColorPreview.setVisible (showColor);
    btnColor.setVisible        (showColor);

    // Color 2: spectrogram only (shown in resized() when custom colour is on)
    lblColor2.setVisible        (false);
    lblColor2Preview.setVisible (false);
    btnColor2.setVisible        (false);

    // Rotation + block selector. The 0/90/180/270 "Rotate" makes no sense for the
    // generative / tone modules — hidden for Synesthesia, Geometry, Chladni, Tone Analyzer.
    //
    // It IS offered for a Fusion and for its layers. Neither goes through the
    // ordinary panel transform — a Fusion is drawn by the GL host into its own
    // rectangle, and a layer is not drawn in a rectangle at all — so both are
    // rotated inside the fusion shader instead: the Fusion through the affine
    // basis it hands the host, a layer through its quarter turn. From the outside
    // the knob behaves exactly as it does on an ordinary HUD panel, which is the
    // whole point: a spectrum should turn the same way whether it is sitting in
    // the HUD or lending its frames to a fusion.
    //
    // A module lent to a Fusion keeps this knob, and it is the ONLY place its
    // rotation is set. The fusion reads the module's own angle and its shader
    // performs the turn; it holds no second, private opinion about which way up a
    // module is. Two knobs that could disagree would have been one too many.
    const bool showRotate = anySelected && ! (syn || geo || chladni || tone);
    lblModuleRotation.setVisible (showRotate);
    btnRotate.setVisible         (showRotate);
    tbSpecMirror.setVisible      (spec);
    lblModuleLayer.setVisible    (anySelected && settings.hudLayers() > 1);
    cbModuleLayer.setVisible     (anySelected && settings.hudLayers() > 1);
}

// Point the SHARED colour-by-tone widgets at the selected panel. Called from each
// tone-capable module's branch of refreshRightEditorFromSelection — the widgets are
// one set reused by four editors, so without this they would still be showing (and,
// on the next click, writing back) the previously selected module's values.
// The window slider is ONE widget serving two views that do not bottom out in the
// same place: the Oscilloscope's Long Waveform stores raw samples and stays readable
// down to 20 ms, while the meter's Level history stores one envelope point per 30 Hz
// tick and has nothing left to draw below 0.1 s. Callers pass their own floor.
void ControllerContent::setWindowSliderRange (double minSec)
{
    if (std::abs (sOscLtWin.getMinimum() - minSec) < 1.0e-9) return;   // no needless clamping
    sOscLtWin.setRange (minSec, 30.0, 0.01);
    sOscLtWin.setSkewFactorFromMidPoint (5.0);   // fine control in the short range
}

void ControllerContent::syncToneWidgets (const juce::ValueTree& panel)
{
    // Spectrogram stores 2 for "by tone"; the others store 1. Both are "not the
    // manual/preset colour", which is exactly what the checkbox means, so it reads
    // as "anything but 0" rather than testing for a particular number.
    tbToneColor.setToggleState ((int) panel.getProperty (AlterState::kColorMode, 0) != 0,
                                juce::dontSendNotification);
    tbToneTwist.setToggleState ((bool) panel.getProperty (AlterState::kToneTwist, false),
                                juce::dontSendNotification);
    sToneSmooth.setValue ((float) panel.getProperty (AlterState::kToneSmooth, 0.5f),
                          juce::dontSendNotification);
}

void ControllerContent::refreshRightEditorFromSelection()
{
    auto panel = (selectedPanelId >= 0) ? settings.getPanelById (selectedPanelId)
                                        : juce::ValueTree();
    if (! panel.isValid())
    {
        setEditorVisible ({});
        resized();
        lblColorPreview.setColour (juce::Label::backgroundColourId, AlterTheme::pictonBlue);
        return;
    }

    updateModuleInstanceSelection();   // sync the per-module source picker to this panel

    const auto type = panel.getProperty (AlterState::kType).toString();

    if (type == "rms" || type == "audiometer")
    {
        cbRmsPeakMode.setSelectedId ((int) panel.getProperty (AlterState::kMeterMode, 0) + 1,
                                     juce::dontSendNotification);
        cbMeterView.setSelectedId ((int) panel.getProperty (AlterState::kMeterView, 0) + 1,
                                   juce::dontSendNotification);
        sRmsSmooth.setValue ((float) panel.getProperty (AlterState::kSmooth, 0.5f),
                             juce::dontSendNotification);
        cbColorMode.setSelectedId ((int) panel.getProperty (AlterState::kColorMode, 0) + 1,
                                   juce::dontSendNotification);
        // Level-history controls (shared widgets, reused for the meter panel)
        cbDisplayMode.setSelectedId (juce::jlimit (1, 4, (int) panel.getProperty (AlterState::kDisplayMode, 0) + 1),
                                     juce::dontSendNotification);
        // BEFORE the value, not after: Slider::setRange clamps whatever is currently
        // held to the new bounds, so setting a 20 ms window into a slider still
        // carrying the meter's 0.1 s floor would silently round it up to 0.1 and the
        // control would then disagree with the state it is supposed to be showing.
        setWindowSliderRange (0.1);
        sOscLtWin.setValue ((float) panel.getProperty (AlterState::kOscLtWindow, 10.0f),
                            juce::dontSendNotification);

        cbLrColor.setSelectedId ((int) panel.getProperty (AlterState::kOscLrColor, 0) + 1,
                                 juce::dontSendNotification);
        tbClipZone.setToggleState ((bool) panel.getProperty (AlterState::kClipZone, true),
                                    juce::dontSendNotification);
        tbMeterTwoBars.setToggleState ((bool) panel.getProperty (AlterState::kMeterTwoBars, true),
                                       juce::dontSendNotification);
        // Flexible == no fixed pixel width. Reading the state itself rather than a
        // mirror of it means the box is also right after a divider drag, which sets
        // a width without knowing this control exists.
        tbMeterToneColor.setToggleState ((bool) panel.getProperty (AlterState::kMeterToneColor, false),
                                         juce::dontSendNotification);
        cbMeterToneShade.setSelectedId (juce::jlimit (1, 2, (int) panel.getProperty (AlterState::kMeterToneShade, 0) + 1),
                                        juce::dontSendNotification);
        // Only the twist + smooth halves apply here; the checkbox that syncToneWidgets
        // would set is tbToneColor, which this module does not use (see kMeterToneColor).
        tbToneTwist.setToggleState ((bool) panel.getProperty (AlterState::kToneTwist, false),
                                    juce::dontSendNotification);
        sToneSmooth.setValue ((float) panel.getProperty (AlterState::kToneSmooth, 0.5f),
                              juce::dontSendNotification);

        const int measureState = (int) panel.getProperty (AlterState::kMeasureState, 0);
        lblMeasureStatus.setColour (juce::Label::textColourId,
                                    measureState == 1 ? AlterTheme::mintGlow : AlterTheme::textDim);
        lblMeasureStatus.setText (measureState == 1 ? "* Measuring..." : "[ Stopped ]",
                                  juce::dontSendNotification);
    }
    else if (type == "spectrum")
    {
        const int psycho = (int) panel.getProperty (AlterState::kPsychoCurve, 0);

        sSpecSmooth.setValue ((float) panel.getProperty (AlterState::kSmooth, 0.5f), juce::dontSendNotification);
        cbSpecPsycho.setSelectedId (psycho + 1, juce::dontSendNotification);
        tbSpecMeasurement.setToggleState ((int) panel.getProperty (AlterState::kMeasurementMode, 0) == 1, juce::dontSendNotification);
        tbSpecPeakHold.setToggleState ((bool) panel.getProperty (AlterState::kPeakHold, false), juce::dontSendNotification);
        tbSpecConstantQ.setToggleState ((bool) panel.getProperty (AlterState::kConstantQ, false), juce::dontSendNotification);
        sSpecPhon.setValue ((double)(int) panel.getProperty (AlterState::kPhon, 60), juce::dontSendNotification);
        lblSpecPhon.setVisible (psycho == 2);
        sSpecPhon.setVisible (psycho == 2);
        cbSpecRef.setSelectedId ((int) panel.getProperty (AlterState::kSpecReference, 0) + 1,
                                 juce::dontSendNotification);
        tbSpecStereo.setToggleState ((bool) panel.getProperty (AlterState::kSpecStereo, false),
                                     juce::dontSendNotification);
        cbSpecLrColor.setSelectedId ((int) panel.getProperty (AlterState::kSpecLrColor, 0) + 1,
                                     juce::dontSendNotification);
        syncToneWidgets (panel);
    }
    else if (type == "oscillator" || type == "oscilator")
    {
        sOscSmooth.setValue ((float) panel.getProperty (AlterState::kSmooth, 0.5f), juce::dontSendNotification);
        tbFill.setToggleState ((bool) panel.getProperty (AlterState::kNeon, false), juce::dontSendNotification);
        cbDisplayMode.setSelectedId (juce::jlimit (1, 4, (int) panel.getProperty (AlterState::kDisplayMode, 0) + 1),
                                     juce::dontSendNotification);
        sOscZoom.setValue ((float) panel.getProperty (AlterState::kZoom, 0.485f), juce::dontSendNotification);
        cbOscTerm.setSelectedId (juce::jlimit (0, 1,
                                     (int) panel.getProperty (AlterState::kOscLongTerm, 0)) + 1,
                                 juce::dontSendNotification);
        setWindowSliderRange (VisualOscilator::kMinWindowSec);   // see the note above
        sOscLtWin.setValue ((float) panel.getProperty (AlterState::kOscLtWindow, 10.0f), juce::dontSendNotification);
        cbLrColor.setSelectedId ((int) panel.getProperty (AlterState::kOscLrColor, 0) + 1,
                                 juce::dontSendNotification);
        tbClipZone.setToggleState ((bool) panel.getProperty (AlterState::kClipZone, true),
                                    juce::dontSendNotification);
        syncToneWidgets (panel);
    }
    else if (type == "synesthesia")
    {
        sSynSmooth.setValue ((float) panel.getProperty (AlterState::kSmooth, 0.15f), juce::dontSendNotification);
        sZoom.setValue ((float) panel.getProperty (AlterState::kZoom, 1.0f), juce::dontSendNotification);
        sRotation.setValue ((float) panel.getProperty (AlterState::kRotation, 0.0f), juce::dontSendNotification);
        sSymmetry.setValue ((int) panel.getProperty (AlterState::kSymmetry, 1), juce::dontSendNotification);
        sSaturation.setValue ((float) panel.getProperty (AlterState::kSaturation, 1.0f), juce::dontSendNotification);
        sSynBright.setValue ((float) panel.getProperty (AlterState::kSynBrightness, 1.0f), juce::dontSendNotification);
        sBloom.setValue ((float) panel.getProperty (AlterState::kBloom, 0.0f), juce::dontSendNotification);
        sSpeed.setRange (-2.0, 2.0, 0.01);   // bipolar here; a Fusion narrows it
        sSpeed.setValue ((float) panel.getProperty (AlterState::kSpeed, 1.0f), juce::dontSendNotification);
        sSynReact.setValue ((float) panel.getProperty (AlterState::kSynReact, 0.0f), juce::dontSendNotification);
        sSynGhost.setValue ((float) panel.getProperty (AlterState::kCurveSmooth, 0.0f), juce::dontSendNotification);
        sSynChange.setValue ((float) panel.getProperty (AlterState::kFragment, 0.0f), juce::dontSendNotification);
        sSynTransmute.setValue ((float) panel.getProperty (AlterState::kTransmute, 0.0f), juce::dontSendNotification);
        sSynClear.setValue ((float) panel.getProperty (AlterState::kClear, 0.0f), juce::dontSendNotification);
        sSynDenoise.setValue ((float) panel.getProperty (AlterState::kDenoise, 0.0f), juce::dontSendNotification);
        sSynTunnel.setValue  ((float) panel.getProperty (AlterState::kSynTunnel, 0.0f), juce::dontSendNotification);
        sSynVortex.setValue  ((float) panel.getProperty (AlterState::kSynVortex, 0.0f), juce::dontSendNotification);
        tbSynBpmSync.setToggleState ((bool) panel.getProperty (AlterState::kSynBpmSync, false), juce::dontSendNotification);
        sSynBpm.setValue ((float) panel.getProperty (AlterState::kSynBpm, 120.0f), juce::dontSendNotification);
        sSynBeatDiv.setValue ((int) panel.getProperty (AlterState::kSynBeatDiv, 5), juce::dontSendNotification);
        sSynBeatDiv.updateText();
        tbSynMirror.setToggleState ((bool) panel.getProperty (AlterState::kMirror, false), juce::dontSendNotification);
        tbSynTone.setToggleState ((int) panel.getProperty (AlterState::kColorMode, 1) == 1, juce::dontSendNotification);
        tbSynTwist.setToggleState ((bool) panel.getProperty (AlterState::kToneTwist, false), juce::dontSendNotification);
    }
    else if (type == "geometry")
    {
        sSynSmooth.setValue ((float) panel.getProperty (AlterState::kSmooth, 0.15f), juce::dontSendNotification);
        sZoom.setValue ((float) panel.getProperty (AlterState::kZoom, 1.0f), juce::dontSendNotification);
        sRotation.setValue ((float) panel.getProperty (AlterState::kRotation, 0.0f), juce::dontSendNotification);
        sGeoGlobalRot.setValue ((float) panel.getProperty (AlterState::kGeoGlobalRot, 0.0f), juce::dontSendNotification);
        sSymmetry.setValue ((int) panel.getProperty (AlterState::kSymmetry, 6), juce::dontSendNotification);
        tbSynMirror.setToggleState ((bool) panel.getProperty (AlterState::kMirror, false), juce::dontSendNotification);
        sSaturation.setValue ((float) panel.getProperty (AlterState::kSaturation, 1.0f), juce::dontSendNotification);
        sSynBright.setValue ((float) panel.getProperty (AlterState::kSynBrightness, 1.0f), juce::dontSendNotification);
        sBloom.setValue ((float) panel.getProperty (AlterState::kBloom, 0.4f), juce::dontSendNotification);
        sSpeed.setRange (-2.0, 2.0, 0.01);   // bipolar here; a Fusion narrows it
        sSpeed.setValue ((float) panel.getProperty (AlterState::kSpeed, 1.0f), juce::dontSendNotification);
        sGeoTri.setValue ((float) panel.getProperty (AlterState::kGeoTri, 0.5f), juce::dontSendNotification);
        sGeoSquare.setValue ((float) panel.getProperty (AlterState::kGeoSquare, 0.5f), juce::dontSendNotification);
        sGeoCircle.setValue ((float) panel.getProperty (AlterState::kGeoCircle, 0.5f), juce::dontSendNotification);
        sGeoComplexity.setValue ((int) panel.getProperty (AlterState::kGeoComplexity, 12), juce::dontSendNotification);
        sGeoRandom.setValue ((float) panel.getProperty (AlterState::kGeoRandom, 1.0f), juce::dontSendNotification);
        sGeoDepth.setValue ((float) panel.getProperty (AlterState::kGeoDepth, 0.7f), juce::dontSendNotification);
        sGeoTunnel.setValue ((float) panel.getProperty (AlterState::kGeoTunnel, 0.0f), juce::dontSendNotification);
        sGeoAperture.setValue ((float) panel.getProperty (AlterState::kGeoAperture, 0.8f), juce::dontSendNotification);
        sGeoReact.setValue ((float) panel.getProperty (AlterState::kGeoReact, 0.0f), juce::dontSendNotification);
        tbGeoBpmSync.setToggleState ((bool) panel.getProperty (AlterState::kGeoBpmSync, false), juce::dontSendNotification);
        sGeoBpm.setValue ((float) panel.getProperty (AlterState::kBpm, 100.0f), juce::dontSendNotification);
        sGeoBeatDiv.setValue ((int) panel.getProperty (AlterState::kGeoBeatDiv, 5), juce::dontSendNotification);
        sGeoBeatDiv.updateText();
        tbGeoTone.setToggleState ((int) panel.getProperty (AlterState::kColorMode, 1) == 1, juce::dontSendNotification);
        tbGeoTwist.setToggleState ((bool) panel.getProperty (AlterState::kToneTwist, false), juce::dontSendNotification);
    }
    else if (type == "fusion")
    {
        for (int i = 0; i < AlterState::kMaxFusionLayers; ++i)
            refreshFusionLayerBox (i);

        for (int i = 0; i < AlterState::kMaxFusionLayers; ++i)
        {
            const int blend = juce::jlimit (0, 2, (int) panel.getProperty (
                AlterState::fusionLayerProp (AlterState::kFusionLayBlend, i), 0));
            cbFusionBlend[i].setSelectedId (blend + 1, juce::dontSendNotification);

            sFusionOpacity[i].setValue ((float) panel.getProperty (
                AlterState::fusionLayerProp (AlterState::kFusionLayOpacity, i), 1.0f), juce::dontSendNotification);
            sFusionAmount[i].setValue ((float) panel.getProperty (
                AlterState::fusionLayerProp (AlterState::kFusionLayAmount, i), 0.5f), juce::dontSendNotification);
            sFusionScale[i].setValue ((float) panel.getProperty (
                AlterState::fusionLayerProp (AlterState::kFusionLayScale, i), 1.0f), juce::dontSendNotification);
            sFusionBands[i].setValue ((int) panel.getProperty (
                AlterState::fusionLayerProp (AlterState::kFusionLayBands, i), 8), juce::dontSendNotification);
            sFusionAngle[i].setValue ((float) panel.getProperty (
                AlterState::fusionLayerProp (AlterState::kFusionLayAngle, i), 0.0f), juce::dontSendNotification);

            // The layer's own post chain.
            sFusionLayMirror[i].setValue (juce::jlimit (0, 8, (int) panel.getProperty (
                AlterState::fusionLayerProp (AlterState::kFusionLayMirror, i), 0)),
                juce::dontSendNotification);
            sFusionLayMirror[i].updateText();

            sFusionLayMirrorAng[i].setValue ((float) panel.getProperty (
                AlterState::fusionLayerProp (AlterState::kFusionLayMirrorAng, i), 0.0f),
                juce::dontSendNotification);

            sFusionLaySymmetry[i].setValue (juce::jlimit (1, 11, (int) panel.getProperty (
                AlterState::fusionLayerProp (AlterState::kFusionLaySymmetry, i), 1)),
                juce::dontSendNotification);
            sFusionLaySymmetry[i].updateText();

            sFusionLaySpin[i].setValue ((float) panel.getProperty (
                AlterState::fusionLayerProp (AlterState::kFusionLaySpin, i), 0.0f),
                juce::dontSendNotification);
            sFusionLaySpeed[i].setValue ((float) panel.getProperty (
                AlterState::fusionLayerProp (AlterState::kFusionLaySpeed, i), 0.0f),
                juce::dontSendNotification);
            sFusionLayZoom[i].setValue ((float) panel.getProperty (
                AlterState::fusionLayerProp (AlterState::kFusionLayZoom, i), 1.0f),
                juce::dontSendNotification);
        }

        tbFusionWarp.setToggleState ((bool) panel.getProperty (AlterState::kFusionWarp, false),
                                  juce::dontSendNotification);
        sFusionWarpAmt.setValue   ((float) panel.getProperty (AlterState::kFusionWarpAmt,   0.5f), juce::dontSendNotification);
        sFusionWarpSwirl.setValue ((float) panel.getProperty (AlterState::kFusionWarpSwirl, 0.6f), juce::dontSendNotification);
        sFusionWarpSmooth.setValue ((float) panel.getProperty (AlterState::kFusionWarpSmooth, 0.0f), juce::dontSendNotification);
        sFusionWarpDenoise.setValue ((float) panel.getProperty (AlterState::kFusionWarpDenoise, 0.0f), juce::dontSendNotification);
        cbFusionWarpSrc.setSelectedId (juce::jlimit (0, AlterState::kMaxFusionLayers - 1,
            (int) panel.getProperty (AlterState::kFusionWarpSrc, 0)) + 1, juce::dontSendNotification);

        sFusionSymmetry.setValue ((int) panel.getProperty (AlterState::kFusionSymmetry, 1),
                               juce::dontSendNotification);
        sFusionSymmetry.updateText();

        // A count now, not a toggle. An old preset stored a bool here; var turns
        // false into 0 and true into 1, which are exactly the two values that
        // meant the same thing before, so nothing needs migrating.
        sFusionMirror.setValue (juce::jlimit (0, 8, (int) panel.getProperty (AlterState::kFusionMirror, 0)),
                                juce::dontSendNotification);
        sFusionMirror.updateText();

        sFusionMirrorAngle.setValue ((float) panel.getProperty (AlterState::kFusionMirrorAngle, 0.0f),
                                  juce::dontSendNotification);

        refreshFusionHeaders();

        sFusionSpin.setValue  ((float) panel.getProperty (AlterState::kFusionSpin,  0.0f), juce::dontSendNotification);
        sFusionZoom.setValue  ((float) panel.getProperty (AlterState::kFusionZoom,  1.0f), juce::dontSendNotification);
        sFusionVortex.setValue ((float) panel.getProperty (AlterState::kFusionVortex, 0.0f), juce::dontSendNotification);
        sFusionReact.setValue ((float) panel.getProperty (AlterState::kFusionReact, 0.0f), juce::dontSendNotification);
        cbFusionDetail.setSelectedId (juce::jlimit (0, 3,
            (int) panel.getProperty (AlterState::kFusionDetail, 1)) + 1, juce::dontSendNotification);

        tbFusionGlobLayer[0].setToggleState ((bool) panel.getProperty (AlterState::kFusionGlobL0, true), juce::dontSendNotification);
        tbFusionGlobLayer[1].setToggleState ((bool) panel.getProperty (AlterState::kFusionGlobL1, true), juce::dontSendNotification);
        tbFusionGlobLayer[2].setToggleState ((bool) panel.getProperty (AlterState::kFusionGlobL2, true), juce::dontSendNotification);

        tbFusionLiquid.setToggleState ((bool) panel.getProperty (AlterState::kFusionLiquid, false), juce::dontSendNotification);
        sFusionLiquidAmt.setValue    ((float) panel.getProperty (AlterState::kFusionLiquidAmt,    0.5f), juce::dontSendNotification);
        sFusionLiquidSmooth.setValue ((float) panel.getProperty (AlterState::kFusionLiquidSmooth, 0.5f), juce::dontSendNotification);
        sFusionLiquidDenoise.setValue ((float) panel.getProperty (AlterState::kFusionLiquidDenoise, 0.0f), juce::dontSendNotification);

        tbFusionTunnel.setToggleState ((bool) panel.getProperty (AlterState::kFusionTunnel, false), juce::dontSendNotification);

        // The Speed slider is shared with Synesthesia and Geometry, where it is a
        // bipolar evolution rate. Here it is one-directional: 0 stands still, 1
        // turns fast. Narrow it while a Fusion is selected so the knob cannot be
        // put somewhere that means nothing.
        sSpeed.setRange (0.0, 1.0, 0.01);
        sSpeed.setValue ((float) panel.getProperty (AlterState::kSpeed, 0.0f),
                         juce::dontSendNotification);
    }
    else if (type == "chladni")
    {
        const bool reactive = (bool) panel.getProperty (AlterState::kChladniReactive, true);
        tbChladniReactive.setToggleState (reactive, juce::dontSendNotification);
        sChladniM.setEnabled (! reactive);
        sChladniN.setEnabled (! reactive);
        cbChladniPreset.setEnabled (! reactive);
        sChladniShift.setEnabled (reactive);   // shift works on the matched mode
        sChladniShift.setValue ((int) panel.getProperty (AlterState::kChladniShift, 0),
                                juce::dontSendNotification);

        const int mVal = (int) panel.getProperty (AlterState::kChladniM, 2);
        const int nVal = (int) panel.getProperty (AlterState::kChladniN, 3);
        sChladniM.setValue (mVal, juce::dontSendNotification);
        sChladniN.setValue (nVal, juce::dontSendNotification);

        // match preset combo to the current (m,n) if it corresponds to a preset
        int presetId = 1; // Custom
        for (int i = 2; i <= cbChladniPreset.getNumItems(); ++i)
            if (cbChladniPreset.getItemText (i - 1)
                    == "(" + juce::String (mVal) + "," + juce::String (nVal) + ")")
            { presetId = i; break; }
        cbChladniPreset.setSelectedId (presetId, juce::dontSendNotification);

        sChladniAR.setValue ((double)(float) panel.getProperty (AlterState::kChladniAR, 1.0f), juce::dontSendNotification);
        sChladniSharp.setValue ((double)(float) panel.getProperty (AlterState::kChladniSharp, 0.5f), juce::dontSendNotification);
        sChladniParticles.setValue ((int) panel.getProperty (AlterState::kChladniParticles, 5000), juce::dontSendNotification);
        cbChladniMaterial.setSelectedId ((int) panel.getProperty (AlterState::kChladniMaterial, 0) + 1,
                                         juce::dontSendNotification);
        tbChladniToneColor.setToggleState ((int) panel.getProperty (AlterState::kColorMode, 1) == 1,
                                           juce::dontSendNotification);
        tbChladniTwist.setToggleState ((bool) panel.getProperty (AlterState::kToneTwist, false),
                                        juce::dontSendNotification);
        sToneSmooth.setValue ((float) panel.getProperty (AlterState::kToneSmooth, 0.374f),
                              juce::dontSendNotification);
    }
    else if (type == "toneanalyzer")
    {
        sToneSens.setValue ((float) panel.getProperty (AlterState::kToneSens, 0.5f), juce::dontSendNotification);
        tbToneTuner.setToggleState ((bool) panel.getProperty (AlterState::kToneTuner, false), juce::dontSendNotification);
        syncToneWidgets (panel);
    }
    else if (type == "spectrogram")
    {
        sSpectroSmooth.setValue ((float) panel.getProperty (AlterState::kSmooth, 0.35f), juce::dontSendNotification);
        sSpectroWin.setValue ((float) panel.getProperty (AlterState::kSpectroWindow, 30.0f), juce::dontSendNotification);
        sSpectroFill.setValue ((float) panel.getProperty (AlterState::kSpectroLineFill, 0.0f), juce::dontSendNotification);
        tbSpectroMirror.setToggleState ((bool) panel.getProperty (AlterState::kSpectroMirror, false), juce::dontSendNotification);
        tbSpectroConstantQ.setToggleState ((bool) panel.getProperty (AlterState::kConstantQ, false), juce::dontSendNotification);
        tbSpectroReassign.setToggleState ((bool) panel.getProperty (AlterState::kSpectroReassign, false), juce::dontSendNotification);
        cbSpectroColorMode.setSelectedId (
            juce::jlimit (0, 2, (int) panel.getProperty (AlterState::kColorMode, 0)) + 1,
            juce::dontSendNotification);
        syncToneWidgets (panel);
    }
    else if (type == "stereoscope")
    {
        cbStereoMode.setSelectedId (juce::jlimit (1, 5, (int) panel.getProperty (AlterState::kStereoMode, 0) + 1),
                                    juce::dontSendNotification);
        sStereoSmooth.setValue ((float) panel.getProperty (AlterState::kSmooth, 0.5f), juce::dontSendNotification);
        tbStereoParticles.setToggleState ((bool) panel.getProperty (AlterState::kStereoParticles, false),
                                          juce::dontSendNotification);
        sStereoDensity.setValue ((float) panel.getProperty (AlterState::kStereoDensity, 0.5f), juce::dontSendNotification);
        tbStereoCtrlBins.setToggleState ((bool) panel.getProperty (AlterState::kStereoCtrlBins, false),
                                         juce::dontSendNotification);
        sStereoBright.setValue ((float) panel.getProperty (AlterState::kStereoBright, 0.5f), juce::dontSendNotification);
        sStereoLineW.setValue ((float) panel.getProperty (AlterState::kStereoLineW, 0.5f), juce::dontSendNotification);
        sStereoPointSize.setValue ((float) panel.getProperty (AlterState::kStereoPointSize, 0.5f), juce::dontSendNotification);
        syncToneWidgets (panel);
    }

    setEditorVisible (type);

    // update color previews from panel properties if present
    if (panel.hasProperty (AlterState::kColor))
        lblColorPreview.setColour (juce::Label::backgroundColourId,
                                   juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor)));
    if (panel.hasProperty (AlterState::kColor2))
        lblColor2Preview.setColour (juce::Label::backgroundColourId,
                                    juce::Colour ((uint32_t)(int) panel.getProperty (AlterState::kColor2)));

    // Update rotation button text
    static const char* rotationText[] = { "0 deg", "90 deg", "180 deg", "270 deg" };
    tbSpecMirror.setToggleState ((bool) panel.getProperty (AlterState::kSpecMirror, false),
                                 juce::dontSendNotification);

    btnRotate.setButtonText (rotationText[juce::jlimit (0, 3,
        (int) panel.getProperty (AlterState::kRotationAngle, 0))]);

    // HUD block selector: rebuild items for the current block count
    const int blocks = settings.hudLayers();
    cbModuleLayer.clear (juce::dontSendNotification);
    for (int b = 1; b <= blocks; ++b)
        cbModuleLayer.addItem (juce::String (b), b);
    cbModuleLayer.setSelectedId (juce::jlimit (0, blocks - 1,
        (int) panel.getProperty (AlterState::kLayer, 0)) + 1, juce::dontSendNotification);

    // LAY IT OUT. Deciding what is visible and deciding where it goes are one
    // operation, not two — resized() places only what is visible, so a refresh
    // without it leaves the newly shown controls at whatever bounds they last
    // had (usually none) and the newly hidden ones still holding their gap.
    //
    // That is what made a fusion slot look broken: picking a module for an empty
    // Layer B writes fusionHost on the LAYER's panel, not on the fusion's, so the
    // tree listener never flagged the selected panel and the timer's refresh never
    // ran. The settings did appear — folding the block and opening it again called
    // resized() by another route, which is why that "fixed" it. Setting a slot
    // back to (none) left the mirror image of the same bug: an empty gap exactly
    // as tall as the settings that used to be there.
    resized();
}

// ── recording orchestration ──────────────────────────────────────────────────

void ControllerContent::configurePluginAudio (HudRecorder& r, juce::uint32 instanceId)
{
    if (instanceId == 0 || ! pullPluginStereo)
    {
        r.setSystemAudioSource();
        return;
    }

    // Measured rate, snapped to the nearest standard one so the exported track
    // plays back at the right speed. Read HERE, at start time, because the 'W'
    // stream has only just been switched on for us and its first honest window
    // is what we waited for.
    const double measured = getPluginStreamRate ? getPluginStreamRate (instanceId) : 0.0;
    static const int kStdRates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
    int rate = 48000;
    if (measured > 1000.0)
    {
        double best = 1.0e9;
        for (int sr : kStdRates)
            if (const double d = std::abs (measured - (double) sr); d < best)
                { best = d; rate = sr; }
    }

    // The AAC encoders take 44.1 or 48 kHz only, so a high-rate session is
    // decimated by an integer factor with a box average.
    int decim = 1;
    while (rate / decim > 48000 && decim < 8) decim *= 2;
    const int outRate = rate / decim;

    DBG ("ALTER REC: module audio from instance " << (int) instanceId
         << ", measured " << juce::String (measured, 1) << " Hz -> " << outRate << " Hz");

    auto pull      = pullPluginStereo;
    auto streamPos = std::make_shared<std::uint64_t> (0);   // not `cursor`: Component has one
    auto pendL     = std::make_shared<std::vector<float>>();
    auto pendR     = std::make_shared<std::vector<float>>();

    r.setExternalAudioSource (
        [pull, instanceId, streamPos, pendL, pendR, decim] (std::vector<float>& out) -> int
        {
            std::vector<float> l, rr;
            if (! pull (instanceId, *streamPos, l, rr)) return 0;

            const size_t n = juce::jmin (l.size(), rr.size());
            if (n == 0) return 0;

            if (decim <= 1)
            {
                out.reserve (out.size() + n * 2);
                for (size_t i = 0; i < n; ++i) { out.push_back (l[i]); out.push_back (rr[i]); }
                return (int) n;
            }

            pendL->insert (pendL->end(), l.begin(),  l.begin()  + (long) n);
            pendR->insert (pendR->end(), rr.begin(), rr.begin() + (long) n);

            const size_t d = (size_t) decim;
            const size_t frames = pendL->size() / d;
            out.reserve (out.size() + frames * 2);
            for (size_t f = 0; f < frames; ++f)
            {
                float sl = 0.0f, sr = 0.0f;
                for (size_t k = 0; k < d; ++k)
                {
                    sl += (*pendL)[f * d + k];
                    sr += (*pendR)[f * d + k];
                }
                out.push_back (sl / (float) d);
                out.push_back (sr / (float) d);
            }
            pendL->erase (pendL->begin(), pendL->begin() + (long) (frames * d));
            pendR->erase (pendR->begin(), pendR->begin() + (long) (frames * d));
            return (int) frames;
        },
        outRate);
}

bool ControllerContent::startPerModuleRecording (const AlterExportSettings& cfg,
                                                 const juce::File& baseFile)
{
    stopAllRecordings();

    auto modules = getExportModules ? getExportModules() : std::vector<AlterExportModule>{};
    if (modules.empty()) return false;

    const auto effective = getEffectiveAudioInstance ? getEffectiveAudioInstance() : 0;

    // Claim the waveform stream of EVERY instance any of these modules reads, in
    // one go — the claim is a set now precisely because this loop exists.
    if (setRecordingAudioInstances)
    {
        std::vector<juce::uint32> ids;
        for (const auto& m : modules)
            if (const auto id = m.audioInstance != 0 ? m.audioInstance : effective; id != 0)
                ids.push_back (id);

        setRecordingAudioInstances (ids);
    }

    for (const auto& m : modules)
    {
        if (m.view == nullptr) continue;

        auto r = std::make_unique<HudRecorder>();

        // Each file is named after its module, so a folder of them is readable
        // without opening any of them.
        const auto safe = juce::File::createLegalFileName (m.name);
        const auto file = baseFile.getParentDirectory()
                              .getChildFile (baseFile.getFileNameWithoutExtension()
                                             + " - " + safe + baseFile.getFileExtension());

        r->setTargetResolution (cfg.targetWidth  (m.screenArea.getWidth(), m.screenArea.getHeight()),
                                cfg.targetHeight (m.screenArea.getWidth(), m.screenArea.getHeight()));

        if (cfg.transparent && grabAlphaFrameFor)
        {
            auto* view = m.view;
            auto grab  = grabAlphaFrameFor;
            r->setAlphaSource ([grab, view] (int w, int h) { return grab (view, w, h); });
        }
        else
        {
            r->setCaptureSource (HudRecorder::CaptureSource::ScreenRect);
            r->setCaptureRect (m.screenArea);
        }

        // The module's own track, either way.
        configurePluginAudio (*r, m.audioInstance != 0 ? m.audioInstance : effective);

        if (r->start (m.view, file))
            moduleRecorders.push_back (std::move (r));
    }

    DBG ("ALTER REC: per-module recording started, " << (int) moduleRecorders.size()
         << " of " << (int) modules.size() << " modules");

    return ! moduleRecorders.empty();
}

void ControllerContent::stopAllRecordings()
{
    for (auto& r : moduleRecorders)
        if (r != nullptr)
            r->stop();

    moduleRecorders.clear();
}
