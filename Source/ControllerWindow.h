#pragma once
#include <JuceHeader.h>
#include <vector>
#include "AlterState.h"
#include "AlterTheme.h"
#include "UdpReceiver.h"          // PluginInstanceInfo
#include "InfoWindowAudioMeters.h"
#include "HudRecorder.h"

struct AlterExportSettings;   // ExportDialog.h

/** One HUD module as something that can be exported on its own. Mirrors
    MainComponent::ExportableModule; declared here so the controller does not have
    to include MainComponent.h (which includes this). */
struct AlterExportModule
{
    juce::Component*     view = nullptr;
    juce::String         name;
    juce::uint32         audioInstance = 0;   // 0 = auto (the effective instance)
    juce::Rectangle<int> screenArea;          // physical px
};

// Controller v2:
// - left: Add... + list panel instances (RMS #id, SPECTRUM #id)
// - right: editor vybraného panelu + Delete
// - footer: Always on top + Audio Mode (+ VST instance picker) + Quit
// - drag & drop support for reordering panels

class ControllerContent : public juce::Component,
                          private juce::ListBoxModel,
                          private juce::Timer,
                          private juce::ValueTree::Listener
{
public:
    explicit ControllerContent (AlterState& s);
    ~ControllerContent() override;

    std::function<void (bool)> onAlwaysOnTopChanged;
    std::function<void (int)> onAudioModeChangedInternal;
    std::function<void()> onCloseRequested;

    // VST plugin instance picker (wired in Main.cpp)
    std::function<std::vector<PluginInstanceInfo>()> getPluginInstances;
    std::function<void (juce::uint32)> onPluginInstanceSelected;

    // Recording audio source (wired in Main.cpp): lets the HUD recorder take its
    // audio track straight from ONE plugin instance instead of the system mix.
    //   pullPluginStereo (instanceId, ioTotal, outL, outR) -> stream available?
    std::function<bool (juce::uint32, std::uint64_t&,
                        std::vector<float>&, std::vector<float>&)> pullPluginStereo;
    std::function<double (juce::uint32)> getPluginStreamRate;
    //   setRecordingAudioInstance (instanceId, 0 = stopped) -> keeps the plugin's
    //   waveform ('W') stream alive for the whole take
    std::function<void (juce::uint32)> setRecordingAudioInstance;
    /** Same claim, for the several instances a multi-module recording reads. */
    std::function<void (const std::vector<juce::uint32>&)> setRecordingAudioInstances;

    /** Reshape the HUD window to an aspect ratio (width / height). Pass 0 to
        restore the shape the app starts with. A one-shot resize, not a lock. */
    std::function<void (double)> setHudAspect;

    /** Put the HUD into (or out of) transparent-capture mode, and grab one
        transparent frame. Both are MESSAGE THREAD only. */
    std::function<void (bool)>        setAlphaCaptureMode;
    std::function<juce::Image (int, int)> grabAlphaFrame;

    /** "Export each module separately": the modules to make a file for, and a
        transparent frame of ONE of them. */
    std::function<std::vector<AlterExportModule>()>       getExportModules;
    std::function<juce::Image (juce::Component*, int, int)> grabAlphaFrameFor;
    /** Which instance feeds a module set to "auto". */
    std::function<juce::uint32()>                         getEffectiveAudioInstance;

    // HUD window provider for the recorder (wired by ControllerWindow)
    std::function<juce::Component*()> getHudComponent;

    void timerCallback() override;
    void resized() override;
    void paint (juce::Graphics& g) override;
    void setInfoWindowAlwaysOnTop (bool on);

    /** Ctrl+Z / Ctrl+Shift+Z: state-wide undo / redo (works with the controller focused). */
    bool keyPressed (const juce::KeyPress& key) override
    {
        return settings.handleUndoRedoKey (key);
    }

private:
    // ===== ValueTree::Listener =====
    // The editor widgets used to be written ONLY by the user, so a value changed
    // from outside (a Control-mode VST automating the module, a preset load) left
    // the controller showing stale numbers until the module was re-selected.
    // We just flag it here and coalesce the actual refresh into the 2 Hz timer.
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override;
    bool pendingEditorRefresh = false;
    // Set while THIS controller is writing a property, so its own ValueTree
    // callback can tell an edit it made from one arriving from outside.
    bool writingOwnProperty  = false;
    // The timer runs at 20 Hz for editor-refresh latency; the polling work in it
    // is divided back down to ~2 Hz by this counter (see timerCallback).
    int  slowTickCounter = 0;

    // ===== ListBoxModel =====
    int getNumRows() override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>& selectedRows) override;
    void paintListBoxItem (int rowNumber, juce::Graphics& g,
                           int width, int height, bool rowIsSelected) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void backgroundClicked (const juce::MouseEvent&) override;   // empty list area → deselect
    juce::Component* refreshComponentForRow (int row, bool isSelected, juce::Component* existing) override;

    // ===== block-bubble row model =====
    // With multiple HUD blocks the list shows the panels grouped per block PLUS one
    // placeholder row for every EMPTY block, so its bubble is always visible and
    // modules can be dropped straight into it.
    struct RowRef { int panelIndex = -1; int layer = 0; bool placeholder = false; };
    std::vector<RowRef> rowModel() const;
    int rowForPanelId (int panelId) const;   // -1 if not found
    void deselectModule();                   // clear selection + refresh the editor

    // Run once at the end of construction: gives every slider under `parent` a
    // double-click-to-default, and (when asked) a text box so its value can be
    // typed. Recurses, but never into a Slider — its value box is its own child.
    void finaliseSliders (juce::Component& parent, bool addMissingTextBoxes);

    void mouseDown (const juce::MouseEvent& e) override;   // click on free space → deselect

    // ===== Drag & Drop Helper Component =====
    class DraggableListItem : public juce::Component
    {
    public:
        DraggableListItem (ControllerContent& o) : owner (o)
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void setRow (int r) { row = r; }

        void mouseDown (const juce::MouseEvent& e) override
        {
            owner.panelList.selectRow (row);

            if (e.mods.isPopupMenu())   // right-click: Destroy / Hide-Show
            {
                owner.showRowMenu (row);
                return;
            }

            dragging = false;
            dragStartPos = e.getPosition();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! dragging && e.getDistanceFromDragStart() > 5)
            {
                dragging = true;
                owner.isDragging = true;
                owner.draggedRow = row;
                setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            }

            if (dragging)
            {
                const auto pos = e.getEventRelativeTo (&owner.panelList).position.toInt();
                const int numRows = owner.getNumRows();
                const int rowHeight = owner.panelList.getRowHeight();
                int newIndex = juce::jlimit (0, numRows, (pos.y + rowHeight / 2) / rowHeight);

                if (newIndex != owner.dragInsertIndex)
                {
                    owner.dragInsertIndex = newIndex;
                    owner.panelList.repaint();
                }
            }
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (dragging)
            {
                dragging = false;
                setMouseCursor (juce::MouseCursor::PointingHandCursor);
                owner.finalizeDrag();
            }
        }

        void mouseExit (const juce::MouseEvent&) override
        {
            if (! dragging)
                setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

    private:
        ControllerContent& owner;
        int row = 0;
        bool dragging = false;
        juce::Point<int> dragStartPos;
    };

    // ===== helpers =====
    void finalizeDrag();
    void setEditorVisible (const juce::String& type);   // "" = nothing selected
    void refreshRightEditorFromSelection();

    /** Point the shared colour-by-tone widgets (tbToneColor / tbToneTwist /
        sToneSmooth) at `panel`. One widget set serves four editors, so every
        tone-capable branch of refreshRightEditorFromSelection must call this. */
    void syncToneWidgets (const juce::ValueTree& panel);

    /** Re-floor the shared window slider for the view about to use it. Must be
        called BEFORE the value is written into it — setRange clamps. */
    void setWindowSliderRange (double minSec);

    // ===== presets (save/load the whole controller setup) =====
    void savePreset();
    void loadPreset();
    void applyPreset (const juce::ValueTree& preset);
    void refreshInstanceCombo();
    void updateModuleInstanceSelection();   // sync per-module picker to the selected panel
    void showRowMenu (int rowNumber);       // right-click menu (Destroy / Hide-Show) for a list row
    void launchColourPicker (const juce::Identifier& prop, juce::Label& preview,
                             juce::Component& anchor);

    AlterState& settings;

    // left
    juce::TextButton btnAdd { "Create" };
    juce::TextButton btnSavePreset { "Save" };
    juce::TextButton btnLoadPreset { "Load" };
    juce::TextButton btnRecord { "Record" };
    std::unique_ptr<juce::FileChooser> presetChooser;

    // HUD window recorder
    HudRecorder recorder;

    /** "Export each module separately" runs one of these per module, ALL AT ONCE.
        Sequentially would be wrong, not merely slow: each file would capture a
        different passage of the music and the set could never be laid on top of
        each other in an edit. */
    std::vector<std::unique_ptr<HudRecorder>> moduleRecorders;

    /** Points one recorder at one plugin instance's stream (0 = system mix).
        Shared by the single-file and per-module paths so the rate snapping and the
        decimation exist once. */
    void configurePluginAudio (HudRecorder& r, juce::uint32 instanceId);
    bool startPerModuleRecording (const AlterExportSettings& cfg, const juce::File& baseFile);
    void stopAllRecordings();
    juce::ListBox    panelList;
    int selectedPanelId = -1;

    // drag & drop state
    bool isDragging = false;
    int draggedRow = -1;
    int dragInsertIndex = -1;
    juce::Component dragOverlay;
    std::unique_ptr<ModuleInfoWindow> currentInfoWindow;

    // right common
    juce::TextButton btnDelete { "Destroy" };
    juce::TextButton btnGetInfo{ "Explore" };
    juce::Label      lblModuleInstance;       // "Source" label for the per-module picker
    juce::ComboBox   cbModuleInstance;        // per-module audio instance (Auto + instances)

    // RMS editor
    juce::Label    lblRmsMode;
    juce::ComboBox cbRmsPeakMode;   // Type: RMS / True Peak / LUFS
    juce::Label    lblMeterView;
    juce::ComboBox cbMeterView;     // View: Momentary / Trend
    juce::Label    lblRmsSmooth;
    juce::Slider   sRmsSmooth;

    // Trend measurement controls (shown when View = Trend)
    juce::TextButton btnMeasureStart { "Start" };
    juce::TextButton btnMeasureStop  { "Stop"  };
    juce::Label      lblMeasureStatus;

    // Audio Meter: colour by tone + the shade the loudness zones step through.
    // Its own checkbox rather than the shared tbToneColor because that one writes
    // kColorMode, which on this module already carries Standard/Gradient/Spectrum
    // — see kMeterToneColor in AlterState. The twist and 'Tone smooth' controls ARE
    // the shared ones; only the checkbox has to differ.
    juce::ToggleButton tbMeterToneColor;
    juce::Label        lblMeterToneShade;
    juce::ComboBox     cbMeterToneShade;    // 0=Gradient, 1=Complementary, 2=Analogous
    juce::ToggleButton tbMeterTwoBars;      // level bar + peak/max-hold bar

    // Shared by the Audio Meter's Level history and the Oscilloscope: both draw a
    // waveform against an absolute scale, so both take the same 0 dBFS annotation
    // and the same L/R colour relationship, off the same two state keys.
    juce::ToggleButton tbClipZone;          // 0 dBFS boundary + dB ruler + over-scale marks
    juce::Label        lblLrColor;
    juce::ComboBox     cbLrColor;           // R colour: complementary / analogous

    // Spectrum editor
    juce::Label        lblSpecSmooth;
    juce::Slider       sSpecSmooth;
    juce::ToggleButton tbSpecMeasurement;   // Measurement mode (SPAN-like)
    juce::Label        lblSpecPsycho;
    juce::ComboBox     cbSpecPsycho;        // 0=Flat, 1=A-weight, 2=ISO226
    juce::Label        lblSpecPhon;
    juce::Slider       sSpecPhon;           // ISO226 phon level (40/60/80)
    juce::Label        lblSpecRef;
    juce::ComboBox     cbSpecRef;           // reference curve: Off/EDM/Bass/House/HipHop/Pop/Rock
    juce::ToggleButton tbSpecStereo;        // overlaid L/R spectra
    juce::ToggleButton tbSpecPeakHold;      // peak-hold overlay
    juce::ToggleButton tbSpecConstantQ;     // constant-Q log-band aggregation
    juce::Label        lblSpecLrColor;
    juce::ComboBox     cbSpecLrColor;       // R colour: complementary / analogous

    // Oscillator editor
    juce::Label  lblOscSmooth;
    juce::Slider sOscSmooth;
    juce::ToggleButton tbFill;
    juce::Label  lblDisplayMode;
    juce::ComboBox cbDisplayMode;
    juce::Label  lblOscZoom;
    juce::Slider sOscZoom;
    juce::Label    lblOscTerm;
    juce::ComboBox cbOscTerm;               // Short term / Long term
    juce::Label  lblOscLtWin;
    juce::Slider sOscLtWin;                 // window 0.1-30 s
    juce::ToggleButton tbOscSymmetry;       // mirrored |peak| envelope

    // Synesthesia editor
    juce::Label  lblSynSmooth;
    juce::Slider sSynSmooth;
    juce::Label  lblZoom;
    juce::Slider sZoom;
    juce::Label  lblRotation;
    juce::Slider sRotation;
    juce::Label  lblSymmetry;
    juce::Slider sSymmetry;
    juce::ToggleButton tbSynMirror;         // synesthesia: mirror fold (reflective symmetry)
    juce::Label  lblSaturation;
    juce::Slider sSaturation;
    juce::Label  lblSynBright;              // synesthesia: overall light output (0..2)
    juce::Slider sSynBright;
    juce::Label  lblBloom;
    juce::Slider sBloom;
    juce::Label  lblSpeed;
    juce::Slider sSpeed;                    // shader evolution speed (-2..2, 0 = still)
    juce::Label  lblSynGhost;
    juce::Slider sSynGhost;                 // synesthesia curve/ghost motion smoothing (0..1)
    juce::Label  lblSynReact;
    juce::Slider sSynReact;                 // synesthesia audio reactivity (0..1)
    juce::Label  lblSynChange;
    juce::Slider sSynChange;                // synesthesia shader variation 'Change' (0..1)
    juce::Label  lblSynTransmute;
    juce::Slider sSynTransmute;             // synesthesia second (symmetric) morph 'Transmute' (0..1)
    juce::Label  lblSynClear;
    juce::Slider sSynClear;                 // synesthesia 'Clear' cleanup/merge top layer (0..1)
    juce::Label  lblSynDenoise;
    juce::Slider sSynDenoise;               // synesthesia 'Denoise' fuse dashed secondary curves (0..1)
    juce::Label  lblSynTunnel;
    juce::Slider sSynTunnel;                // synesthesia 'Tunnel' symmetric tunnel morph (0..1)
    juce::Label  lblSynVortex;
    juce::Slider sSynVortex;                // synesthesia 'Vortex' tunnel swirl (0..1)
    juce::ToggleButton tbSynBpmSync;        // synesthesia BPM mode on/off
    juce::Label  lblSynBpm;
    juce::Slider sSynBpm;                   // synesthesia BPM tempo
    juce::Label  lblSynBeatDiv;
    juce::Slider sSynBeatDiv;               // synesthesia beat-division for the light pulse
    juce::ToggleButton tbSynTone;           // synesthesia: tone colour vs manual base colour
    juce::ToggleButton tbSynTwist;          // synesthesia: mirror the tone→hue wheel (tone mode only)

    // Chladni Pattern editor
    juce::ToggleButton tbChladniReactive;        // audio-reactive m/n
    juce::Label    lblChladniPreset;
    juce::ComboBox cbChladniPreset;              // classic (m,n) figures (manual base)
    juce::Label    lblChladniM,    lblChladniN;
    juce::Slider   sChladniM,     sChladniN;     // IncDecButtons, range 1–12
    juce::Label    lblChladniShift;
    juce::Slider   sChladniShift;                // reactive mode shift 0–6
    juce::Label    lblChladniAR;
    juce::Slider   sChladniAR;                   // LinearHorizontal, 0.25–4.0
    juce::Label    lblChladniSharp;
    juce::Slider   sChladniSharp;                // sand movement smoothness 0–1
    juce::Label    lblChladniParticles;
    juce::Slider   sChladniParticles;            // grain count 1000–15000
    juce::Label    lblChladniMaterial;
    juce::ComboBox cbChladniMaterial;
    juce::ToggleButton tbChladniToneColor;       // colour follows dominant tone
    juce::ToggleButton tbChladniTwist;           // mirror the tone→hue wheel (tone mode only)

    // ToneAnalyzer editor
    juce::Label  lblToneSens;
    juce::Slider sToneSens;
    juce::ToggleButton tbToneTuner;         // tuner mode: accurate mono pitch + needle

    // Spectrogram editor
    juce::Label  lblSpectroSmooth;
    juce::Slider sSpectroSmooth;
    juce::Label  lblSpectroWin;
    juce::Slider sSpectroWin;               // visible window 5-120 s
    juce::Label  lblSpectroFill;
    juce::Slider sSpectroFill;              // low-band line thickness: 0 thin .. 1 filled
    juce::ToggleButton tbSpectroMirror;     // mirror frequency axis
    juce::ToggleButton tbSpectroConstantQ;  // constant-Q log-band aggregation
    juce::ToggleButton tbSpectroReassign;   // enhanced-frequency (spectral reassignment)
    // Spectrogram's colour choice became three-way (theme / custom / by tone), so
    // the old "Custom color" checkbox is gone rather than sitting next to a combo
    // that can contradict it. Stored presets are unaffected: the combo's ids are the
    // same kColorMode values the checkbox wrote.
    juce::Label    lblSpectroColorMode;
    juce::ComboBox cbSpectroColorMode;      // 0=Theme heat map, 1=Custom gradient, 2=By tone

    // ── Shared "colour by tone" controls ─────────────────────────────────────
    //  ONE set of widgets, reused by Spectrum / Oscilloscope / Stereoscope /
    //  Spectrogram — exactly as lblColor/btnColor are already shared across every
    //  module's editor. Only one module's editor is ever laid out at a time, so a
    //  per-module copy would be four times the widgets to keep in sync for no
    //  behavioural difference. (Synesthesia, Geometry and Chladni keep their own
    //  toggles: theirs bind 'Tone smooth' to kSmooth, these bind it to
    //  kToneSmooth — see the note on that key in AlterState.)
    juce::ToggleButton tbToneColor;         // colour follows the dominant tone
    juce::ToggleButton tbToneTwist;         // mirror the tone→hue wheel (tone mode only)
    juce::Label    lblToneSmooth;
    juce::Slider   sToneSmooth;             // 0..1 hue chase rate

    // Stereoscope editor
    juce::Label    lblStereoMode;
    juce::ComboBox cbStereoMode;            // Particles / Goniometer / Polar / Correlation / Correlometer
    juce::Label    lblStereoSmooth;
    juce::Slider   sStereoSmooth;
    juce::ToggleButton tbStereoParticles;   // goniometer: particle cloud vs line
    juce::Label    lblStereoDensity;
    juce::Slider   sStereoDensity;          // particle count (Particles + Goniometer cloud)
    juce::ToggleButton tbStereoCtrlBins;    // correlometer: use controller Max-bins (else 512)
    juce::Label    lblStereoBright;
    juce::Slider   sStereoBright;           // gonio/polar trace brightness
    juce::Label    lblStereoLineW;
    juce::Slider   sStereoLineW;            // goniometer beam thickness (line trace only)
    juce::Label    lblStereoPointSize;
    juce::Slider   sStereoPointSize;        // dot size: Particles, gonio cloud, Polar

    // Geometry editor (reuses the Synesthesia sliders for the shared params)
    juce::Label    lblGeoTri;
    juce::Slider   sGeoTri;                 // triangle weight
    juce::Label    lblGeoSquare;
    juce::Slider   sGeoSquare;              // square weight
    juce::Label    lblGeoCircle;
    juce::Slider   sGeoCircle;              // circle weight
    juce::Label    lblGeoComplexity;
    juce::Slider   sGeoComplexity;          // layer count 1..48
    juce::Label    lblGeoRandom;
    juce::Slider   sGeoRandom;              // birth-angle randomization 0..1
    juce::Label    lblGeoReact;
    juce::Slider   sGeoReact;               // audio reactivity 0..1
    juce::Label    lblGeoDepth;
    juce::Slider   sGeoDepth;               // spawn distance (perspective depth) 0..1
    juce::Label    lblGeoTunnel;
    juce::Slider   sGeoTunnel;              // tunnel: 1 = centred point, 0 = fly-through tube
    juce::Label    lblGeoAperture;
    juce::Slider   sGeoAperture;            // iris: 1 = edges fill centre, 0 = open ring
    juce::Label    lblGeoGlobalRot;
    juce::Slider   sGeoGlobalRot;           // whole-module rotation speed 0..360
    juce::ToggleButton tbGeoBpmSync;        // BPM spawn mode on/off
    juce::Label    lblGeoBpm;
    juce::Slider   sGeoBpm;                 // tempo (default 100; plugin overrides)
    juce::Label    lblGeoBeatDiv;
    juce::Slider   sGeoBeatDiv;             // beat-division knob (when shapes spawn)
    juce::ToggleButton tbGeoTone;           // tone-dependent colour
    juce::ToggleButton tbGeoTwist;          // mirror the tone→hue wheel (tone mode only)

    // ── Fusion (fuses other modules) ───────────────────────────────────────
    // The layer pickers list the modules that EXIST right now, by id, because a
    // layer is one of them and not a fresh copy. Item id 1 is always "(none)";
    // every other item id is the panel id + 1, so the mapping needs no table.
    juce::Label    lblFusionLayer[AlterState::kMaxFusionLayers];
    juce::ComboBox cbFusionLayer[AlterState::kMaxFusionLayers];

    /** The fold header for each layer's block of settings, and for the global
        block below them.

        A fusion is now four blocks of a dozen controls each, and all of it at
        once is unreadable — you cannot compare two layers when neither fits on
        the screen. Folding a layer away leaves one row naming what is in it,
        which is exactly the amount of it you need while you work on another.

        A TextButton and not a real disclosure widget on purpose: the whole
        controller is drawn by hand in resized(), so a header that reports its own
        height would be the odd one out. */
    juce::TextButton btnFusionLayHead[AlterState::kMaxFusionLayers];
    juce::TextButton btnFusionGlobalHead;

    // PER LAYER. The stack accumulates bottom-up and every layer carries its own
    // blend mode, so these are arrays and not one global set — slot A is the base
    // and its blend is not offered, because there is nothing underneath it.
    juce::ComboBox cbFusionBlend  [AlterState::kMaxFusionLayers];
    juce::Label    lblFusionOpacity[AlterState::kMaxFusionLayers];
    juce::Slider   sFusionOpacity [AlterState::kMaxFusionLayers];
    juce::Label    lblFusionAmount[AlterState::kMaxFusionLayers];
    juce::Slider   sFusionAmount  [AlterState::kMaxFusionLayers];   // merge bias / weave share
    juce::Label    lblFusionScale [AlterState::kMaxFusionLayers];
    juce::Slider   sFusionScale   [AlterState::kMaxFusionLayers];
    // Weave only
    juce::Label    lblFusionBands [AlterState::kMaxFusionLayers];
    juce::Slider   sFusionBands   [AlterState::kMaxFusionLayers];
    juce::Label    lblFusionAngle [AlterState::kMaxFusionLayers];
    juce::Slider   sFusionAngle   [AlterState::kMaxFusionLayers];

    // PER LAYER, the layer's OWN post chain — the same stages the global block
    // below offers, asked once more of this layer alone. Two layers folded
    // differently and then met is a picture the module could not make before,
    // because every layer saw the same folded space.
    juce::Label    lblFusionLayMirror   [AlterState::kMaxFusionLayers];
    juce::Slider   sFusionLayMirror     [AlterState::kMaxFusionLayers];   // axis COUNT, 0 = none
    juce::Label    lblFusionLayMirrorAng[AlterState::kMaxFusionLayers];
    juce::Slider   sFusionLayMirrorAng  [AlterState::kMaxFusionLayers];
    juce::Label    lblFusionLaySymmetry [AlterState::kMaxFusionLayers];
    juce::Slider   sFusionLaySymmetry   [AlterState::kMaxFusionLayers];
    juce::Label    lblFusionLaySpin     [AlterState::kMaxFusionLayers];
    juce::Slider   sFusionLaySpin       [AlterState::kMaxFusionLayers];
    juce::Label    lblFusionLaySpeed    [AlterState::kMaxFusionLayers];
    juce::Slider   sFusionLaySpeed      [AlterState::kMaxFusionLayers];
    juce::Label    lblFusionLayZoom     [AlterState::kMaxFusionLayers];
    juce::Slider   sFusionLayZoom       [AlterState::kMaxFusionLayers];

    // ── post chain ──────────────────────────────────────────────────────────
    juce::ToggleButton tbFusionWarp;
    juce::Label    lblFusionWarpAmt;   juce::Slider   sFusionWarpAmt;
    juce::Label    lblFusionWarpSwirl; juce::Slider   sFusionWarpSwirl;
    // How much of the source's detail the displacement follows. 0 is the raw
    // field Warp always had; up is the difference between flowing and tearing.
    juce::Label    lblFusionWarpSmooth; juce::Slider  sFusionWarpSmooth;
    // Denoise: a small blur on the warped reads. Kills the pixel speckle a strong
    // warp tears in and merges nearby colours/curves into simpler shapes.
    juce::Label    lblFusionWarpDenoise; juce::Slider sFusionWarpDenoise;
    juce::Label    lblFusionWarpSrc;   juce::ComboBox cbFusionWarpSrc;

    // Symmetry: how many congruent wedges the picture folds into.
    juce::Label    lblFusionSymmetry; juce::Slider sFusionSymmetry;
    // Mirror: how many reflection axes, independent of the wedge count. A COUNT
    // and no longer a toggle, so 0 is the off state and the switch it used to
    // need is a value the control already has.
    juce::Label    lblFusionMirror;      juce::Slider sFusionMirror;
    juce::Label    lblFusionMirrorAngle; juce::Slider sFusionMirrorAngle;

    juce::Label    lblFusionSpin;   juce::Slider sFusionSpin;
    juce::Label    lblFusionZoom;   juce::Slider sFusionZoom;
    // Swirl growing with the radius. Bipolar, and it runs before the folds.
    juce::Label    lblFusionVortex; juce::Slider sFusionVortex;
    juce::Label    lblFusionReact;  juce::Slider sFusionReact;

    // Which layers the whole global chain (fold, tunnel, liquid, warp) touches.
    // All on = fold the whole result, as before.
    juce::Label        lblFusionGlobLayers;
    juce::ToggleButton tbFusionGlobLayer[AlterState::kMaxFusionLayers];

    // LIQUID: melt the targeted layers together — a sibling of Warp.
    juce::ToggleButton tbFusionLiquid;
    juce::Label    lblFusionLiquidAmt;    juce::Slider sFusionLiquidAmt;
    juce::Label    lblFusionLiquidSmooth; juce::Slider sFusionLiquidSmooth;
    // Denoise: the same blur as Warp's, so the melted flow reads as one soft body
    // of colour rather than a noisy smear.
    juce::Label    lblFusionLiquidDenoise; juce::Slider sFusionLiquidDenoise;

    // TUNNEL: radially symmetric receding depth. Just an on/off.
    juce::ToggleButton tbFusionTunnel;

    /** How many pixels each LAYER may render at. A layer is a texture the fusion
        then folds and composites, not the picture, so past a point its resolution
        buys nothing while costing area — see kFusionDetail. */
    juce::Label    lblFusionDetail; juce::ComboBox cbFusionDetail;

    /** Refills a layer picker from the modules that currently exist, keeping the
        slot's own module selected. `slot` is which of the Fusion's layers this
        box drives. */
    void refreshFusionLayerBox (int slot);
    /** Applies a pick: releases whatever was in the slot and takes the new module. */
    void applyFusionLayerPick (int slot, int chosenPanelId);

    /** Is this block of fusion settings folded open? `slot` 0..kMaxFusionLayers-1
        is a layer, -1 is the global block. Open is the default, so a fusion built
        before folding existed still explains itself. */
    bool isFusionBlockOpen (int slot) const;
    /** Folds a block the other way and re-lays the editor out. */
    void toggleFusionBlock (int slot);
    /** Rewrites the fold headers: the arrow, and what is actually in the slot. */
    void refreshFusionHeaders();

    // footer
    juce::ToggleButton alwaysOnTop;
    juce::ToggleButton btnHold;                  // freezes the whole HUD (Hold)
    juce::ToggleButton btnHideInfo;              // hides the per-module top-right info overlays
    juce::ComboBox     cbMaxBins;                // global max FFT bins (moved to footer, bottom-left)
    juce::Label        lblMaxBins;               // "FFT bins" label above the selector
    std::unique_ptr<juce::Drawable> logo;        // brand logotype (recoloured per theme)
    juce::Rectangle<int> logoBounds;             // centred logotype strip pinned to the very bottom
    juce::Colour         logoColour { juce::Colours::white };   // current logo colour
    void updateLogoColour();                     // recolour the logo for the active theme
    juce::Label        lblAudioMode;
    juce::ComboBox     cbAudioMode;
    juce::Label        lblInstance;
    juce::ComboBox     cbPluginInstance;         // visible in VST Plugin mode
    std::vector<PluginInstanceInfo> cachedInstances;
    juce::TextButton   btnQuit { "Quit" };
    juce::TextButton   btnClose{ "Close" };

    // color UI (used in module settings)
    juce::Label        lblColor;
    juce::Label        lblColorPreview;
    juce::TextButton   btnColor { "Color..." };
    juce::Label        lblColor2;
    juce::Label        lblColor2Preview;
    juce::TextButton   btnColor2 { "Color 2..." };
    juce::Label        lblColorMode;
    juce::ComboBox     cbColorMode;

    // Global visual theme — a button that opens a popup menu; the Custom entry has a
    // ">" submenu holding the accent + background colour pickers. Lives in the
    // footer's control row with the other settings, swatches directly beside it.
    juce::Label        lblTheme;
    juce::TextButton   btnTheme;
    juce::TextButton   btnThemeC1, btnThemeC2;   // custom accent/background swatches (beside btnTheme, Custom only)

    // ── HUD shape presets (footer control row, last) ─────────────────────────
    // The aspect ratio of an export is decided by the SHAPE OF THE HUD WINDOW,
    // not by the export dialog: what you frame on screen is what you get, and
    // the file has no black bars because nothing had to be fitted into a frame
    // it did not match. These buttons just resize the window to a ratio — they
    // do NOT lock it, so it stays freely draggable afterwards.
    struct AspectPreset { const char* label; double ratio; const char* tip; };
    static constexpr int kNumAspects = 5;
    static const AspectPreset kAspectPresets[kNumAspects];

    juce::TextButton btnAspectPrev { "<" }, btnAspectNext { ">" };
    juce::Label      lblAspectValue;   // no caption: the ratio text speaks for itself
    int              currentAspect = 0;          // index into kAspectPresets
    void applyAspect (int index);
    void refreshAspectButtons();
    void showThemeMenu();                         // popup: Cyber / Dark / White / Custom
    void updateThemeButtonText();                 // reflect the active theme on the button
    void refreshThemeSwatches();                  // colour + show/hide the footer swatches (Custom only)
    void openThemeColour (const juce::Identifier& prop, juce::Colour fallback);  // custom colour picker
    void syncGlobalWidgetsFromState();           // re-read ALL global widgets from the tree (preset load)

    // Module rotation (common for all modules)
    juce::Label        lblModuleRotation { {}, "Rotate" };
    juce::TextButton   btnRotate { "0 deg" };
    /** Spectrum only, and it sits in the Rotate row: mirroring the frequency axis
        is the other half of "which way round is this module". */
    juce::ToggleButton tbSpecMirror { "Mirror" };

    // HUD block (layer) selector (common, visible when blocks > 1)
    juce::Label        lblModuleLayer { {}, "Block" };
    juce::ComboBox     cbModuleLayer;

    // Scrollable editor viewport
    juce::Viewport     editorViewport;
    juce::Component    editorPanel;

    // System Audio gain
    juce::Label        lblGain;
    juce::Slider       sGain;
};

// Window wrapper
class ControllerWindow : public juce::DocumentWindow
{
public:
    explicit ControllerWindow (AlterState& s, juce::DocumentWindow* hudWindow = nullptr)
        : juce::DocumentWindow ("ALTER Controller",
                                AlterTheme::bgDeep,
                                juce::DocumentWindow::allButtons),
          content (s)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, true);
        setContentOwned (&content, false);
        // Width is sized to the WIDEST row, nothing more. Hold (top bar), the HUD
        // shape stepper (control row) and Close (logotype strip) are all anchored to
        // the right edge, so nothing has to reserve dead space in the middle to keep
        // the two ends apart — the fullest control row (FFT + audio input + VST
        // instance + theme + both Custom swatches + stepper) needs ~624px with
        // margins, and the module editor ~628. That is the whole budget.
        setResizeLimits (640, 380, 100000, 100000);
        setSize (650, 462);

        // Notify both the controller window and optionally the HUD window
        content.onAlwaysOnTopChanged = [this, hudWindow](bool on)
        {
            setAlwaysOnTop (on);
            if (hudWindow) hudWindow->setAlwaysOnTop (on);
            content.setInfoWindowAlwaysOnTop (on);
            if (onAlwaysOnTopChanged) onAlwaysOnTopChanged (on);
        };

        content.onCloseRequested = [this]() { closeButtonPressed(); };

        // Forward audio mode changes
        content.onAudioModeChangedInternal = [this](int mode)
        {
            if (onAudioModeChanged)
                onAudioModeChanged (mode);
        };

        // Forward plugin instance plumbing
        content.getPluginInstances = [this]() -> std::vector<PluginInstanceInfo>
        {
            return getPluginInstances ? getPluginInstances() : std::vector<PluginInstanceInfo>{};
        };
        content.onPluginInstanceSelected = [this](juce::uint32 id)
        {
            if (onPluginInstanceSelected) onPluginInstanceSelected (id);
        };
        content.pullPluginStereo = [this](juce::uint32 id, std::uint64_t& ioTotal,
                                          std::vector<float>& l, std::vector<float>& r) -> bool
        {
            return pullPluginStereo ? pullPluginStereo (id, ioTotal, l, r) : false;
        };
        content.getPluginStreamRate = [this](juce::uint32 id) -> double
        {
            return getPluginStreamRate ? getPluginStreamRate (id) : 0.0;
        };
        content.setRecordingAudioInstance = [this](juce::uint32 id)
        {
            if (setRecordingAudioInstance) setRecordingAudioInstance (id);
        };
        content.setHudAspect = [this](double aspect)
        {
            if (setHudAspect) setHudAspect (aspect);
        };
        content.setAlphaCaptureMode = [this](bool on)
        {
            if (setAlphaCaptureMode) setAlphaCaptureMode (on);
        };
        content.grabAlphaFrame = [this](int w, int h) -> juce::Image
        {
            return grabAlphaFrame ? grabAlphaFrame (w, h) : juce::Image();
        };
        content.grabAlphaFrameFor = [this](juce::Component* c, int w, int h) -> juce::Image
        {
            return grabAlphaFrameFor ? grabAlphaFrameFor (c, w, h) : juce::Image();
        };
        content.getExportModules = [this]() -> std::vector<AlterExportModule>
        {
            return getExportModules ? getExportModules() : std::vector<AlterExportModule>{};
        };
        content.getEffectiveAudioInstance = [this]() -> juce::uint32
        {
            return getEffectiveAudioInstance ? getEffectiveAudioInstance() : 0;
        };
        content.setRecordingAudioInstances = [this](const std::vector<juce::uint32>& ids)
        {
            if (setRecordingAudioInstances) setRecordingAudioInstances (ids);
        };

        // HUD window provider (for the recorder)
        content.getHudComponent = [this]() -> juce::Component*
        {
            return getHudComponent ? getHudComponent() : nullptr;
        };

        setVisible (false);
    }

    void closeButtonPressed() override { setVisible (false); }

    // external hooks
    std::function<void(bool)> onAlwaysOnTopChanged;
    std::function<void(int)> onAudioModeChanged;
    std::function<std::vector<PluginInstanceInfo>()> getPluginInstances;
    std::function<void (juce::uint32)> onPluginInstanceSelected;
    std::function<bool (juce::uint32, std::uint64_t&,
                        std::vector<float>&, std::vector<float>&)> pullPluginStereo;
    std::function<double (juce::uint32)> getPluginStreamRate;
    /** Tell the UDP layer that the recorder is taking its audio from this plugin
        instance (0 = not recording), so the waveform stream it needs keeps being
        sent even when no module on screen draws a waveform. */
    std::function<void (juce::uint32)> setRecordingAudioInstance;
    /** Same claim, for the several instances a multi-module recording reads. */
    std::function<void (const std::vector<juce::uint32>&)> setRecordingAudioInstances;
    /** Reshape the HUD window (width / height; 0 = the app's startup shape). */
    std::function<void (double)> setHudAspect;
    /** Transparent capture: mode switch + one-frame grab (message thread). */
    std::function<void (bool)>   setAlphaCaptureMode;
    std::function<juce::Image (int, int)> grabAlphaFrame;
    std::function<juce::Image (juce::Component*, int, int)> grabAlphaFrameFor;
    std::function<std::vector<AlterExportModule>()> getExportModules;
    std::function<juce::uint32()>                   getEffectiveAudioInstance;
    std::function<juce::Component*()> getHudComponent;

private:
    ControllerContent content;
};
