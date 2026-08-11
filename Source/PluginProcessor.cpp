#include "PluginProcessor.h"
#include <cmath>
#include <vector>

// ===================================================================
// Host quirks
// ===================================================================
// Two things in this plug-in exist ONLY to satisfy Ableton Live, and both are
// actively harmful in every other DAW:
//
//   1. The 70 "reserved" parameters (see createParameterLayout). They push the
//      automatable count over Live's 64-param threshold so the device panel
//      opens EMPTY and the user adds knobs via Configure. Anywhere else they
//      are 70 rows of dead weight in the automation list.
//
//   2. Runtime isAutomatable() flipping on the Hideable params. Most VST3 hosts
//      cache the parameter list at scan time and ignore kParamTitlesChanged
//      (Reaper, FL Studio, Studio One), and a few re-read it mid-session in
//      ways that can invalidate existing automation lanes. Live is the only
//      host where this reliably does the intended thing.
//
// PluginHostType reads the host executable name, which is already valid by the
// time the AudioProcessor constructor runs, so this is safe to query from
// createParameterLayout(). Cached in a function-local static: the host cannot
// change for the lifetime of the process.
static bool alterHostIsAbletonLive()
{
    static const bool isLive = juce::PluginHostType().isAbletonLive();
    return isLive;
}

// ===================================================================
// Hideable parameters: a param whose group != the active group reports
// isAutomatable()==false. Combined with updateHostDisplay(parameterInfoChanged)
// this asks the host to drop the irrelevant group from the automation list.
//
// Only wired up under Ableton Live (see the constructor) — elsewhere `active`
// stays null and every param is permanently automatable.
// ===================================================================
struct HideableParam
{
    int               group  = -1;   // 0=Synesthesia, 1=Chladni
    std::atomic<int>* active = nullptr;
    bool automatableNow() const noexcept
    {
        if (active == nullptr) return true;
        const int a = active->load();
        return a == 255 || a == group;
    }
};

// ===================================================================
// Stepped float range.
//
// A plain NormalisableRange<float>(20, 400) is CONTINUOUS: the host hands us a
// normalised 0..1 whose resolution is its own business, and we turn whatever it
// sends into a value. In Ableton Live's device panel that control is ~85 px wide,
// so one pixel of travel is a couple of BPM and the numbers that come out are
// arbitrary 7-decimal junk — the parameter "steps", but on values nobody can
// name, and 128.00 is unreachable.
//
// ALTER's own controller has always snapped these to a grid (0.01 for the 0..1
// knobs, 1 degree for rotation, 0.1 BPM for tempo). The plugin did not, so the
// same parameter behaved differently depending on which end you turned it from.
// This closes that gap: snapping happens inside convertFrom0to1, so EVERY value
// the parameter can ever hold is a multiple of the step, no matter which host
// sets it or how coarse its slider is.
//
// interval is also stored on the range, purely so JUCE derives a sensible number
// of decimal places for the text the host displays (1 for BPM, 2 for the knobs)
// instead of printing seven. isDiscrete() stays false on purpose — see below.
static juce::NormalisableRange<float> steppedRange (float lo, float hi, float step)
{
    auto snap = [step] (float s, float e, float v)
    {
        if (step <= 0.0f) return juce::jlimit (s, e, v);
        return juce::jlimit (s, e, s + std::round ((v - s) / step) * step);
    };

    juce::NormalisableRange<float> r
    {
        lo, hi,
        [snap] (float s, float e, float p)   // 0..1 -> value (snapped)
        {
            return snap (s, e, s + (e - s) * juce::jlimit (0.0f, 1.0f, p));
        },
        [] (float s, float e, float v)       // value -> 0..1
        {
            return e > s ? juce::jlimit (0.0f, 1.0f, (v - s) / (e - s)) : 0.0f;
        },
        snap                                 // snapToLegalValue
    };

    r.interval = step;
    return r;
}

struct HideableFloat : juce::AudioParameterFloat, HideableParam
{
    HideableFloat (const juce::ParameterID& pid, const juce::String& nm,
                   juce::NormalisableRange<float> range, float def, int grp)
        : juce::AudioParameterFloat (pid, nm, range, def) { group = grp; }
    bool isAutomatable() const override { return automatableNow(); }

    // The range carries an interval, which would normally make JUCE report the
    // parameter to VST3 as DISCRETE with a stepCount of a few thousand. Live then
    // draws it as a stepped control and automation lanes turn into staircases.
    // We want the opposite: a smooth control that happens to land on clean values,
    // so the host is told it is continuous and the snapping stays our business.
    bool isDiscrete() const override { return false; }
};

struct HideableInt : juce::AudioParameterInt, HideableParam
{
    HideableInt (const juce::ParameterID& pid, const juce::String& nm,
                 int mn, int mx, int def, int grp)
        : juce::AudioParameterInt (pid, nm, mn, mx, def) { group = grp; }
    bool isAutomatable() const override { return automatableNow(); }
};

struct HideableChoice : juce::AudioParameterChoice, HideableParam
{
    HideableChoice (const juce::ParameterID& pid, const juce::String& nm,
                    const juce::StringArray& choices, int def, int grp)
        : juce::AudioParameterChoice (pid, nm, choices, def) { group = grp; }
    bool isAutomatable() const override { return automatableNow(); }
};

struct HideableBool : juce::AudioParameterBool, HideableParam
{
    HideableBool (const juce::ParameterID& pid, const juce::String& nm,
                  bool def, int grp)
        : juce::AudioParameterBool (pid, nm, def) { group = grp; }
    bool isAutomatable() const override { return automatableNow(); }
};

// ===================================================================
// Parameter layout
// ===================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
AlterListenerAudioProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

  #ifdef ALTER_PURE_LISTENER
    // Pure listener: NO automatable parameters (no control, no editor knobs).
    // It only captures audio and streams packets to ALTER on demand.
    return layout;
  #endif

    constexpr int kSyn = 0, kChl = 1, kGeo = 2;

    // Kept for state compatibility, but the plugin now always operates in Control
    // mode (it controls a module AND streams audio at the same time).
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { "mode", 1 }, "Mode",
        StringArray { "Listener", "Control" }, 1));

    // ---- Synesthesia (continuous) ----
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synSmooth", 1 }, "Syn Smooth",
        steppedRange (0.01f, 0.99f, 0.01f), 0.5f, kSyn));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synZoom", 1 }, "Syn Zoom",
        steppedRange (0.5f, 2.0f, 0.01f), 1.0f, kSyn));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synRotation", 1 }, "Syn Rotation",
        steppedRange (-360.0f, 360.0f, 1.0f), 0.0f, kSyn));   // bipolar (matches controller)
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synSaturation", 1 }, "Syn Saturation",
        steppedRange (0.0f, 2.0f, 0.01f), 1.0f, kSyn));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synBrightness", 1 }, "Syn Brightness",
        steppedRange (0.0f, 2.0f, 0.01f), 1.0f, kSyn));   // overall light output (1 = neutral)
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synBloom", 1 }, "Syn Bloom",
        steppedRange (-1.0f, 1.0f, 0.01f), 0.0f, kSyn));     // bipolar (matches controller)
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synSpeed", 1 }, "Syn Speed",
        steppedRange (-2.0f, 2.0f, 0.01f), 1.0f, kSyn));       // bipolar (matches controller)
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synReact", 1 }, "Syn React",
        steppedRange (0.0f, 1.0f, 0.01f), 0.0f, kSyn));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synFragment", 1 }, "Syn Fragment",
        steppedRange (0.0f, 1.0f, 0.01f), 0.0f, kSyn));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synTransmute", 1 }, "Syn Transmute",
        steppedRange (0.0f, 1.0f, 0.01f), 0.0f, kSyn));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synClear", 1 }, "Syn Clear",
        steppedRange (0.0f, 1.0f, 0.01f), 0.0f, kSyn));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synDenoise", 1 }, "Syn Denoise",
        steppedRange (0.0f, 1.0f, 0.01f), 0.0f, kSyn));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synCurveSmooth", 1 }, "Syn Curve Smooth",
        steppedRange (0.0f, 1.0f, 0.01f), 0.0f, kSyn));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synTunnel", 1 }, "Syn Tunnel",
        steppedRange (0.0f, 1.0f, 0.01f), 0.0f, kSyn));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synVortex", 1 }, "Syn Vortex",
        steppedRange (-1.0f, 1.0f, 0.01f), 0.0f, kSyn));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synBpm", 1 }, "Syn BPM",
        steppedRange (20.0f, 400.0f, 0.1f), 120.0f, kSyn));

    // ---- Synesthesia (quantized) ----
    layout.add (std::make_unique<HideableInt> (
        ParameterID { "synSymmetry", 1 }, "Syn Symmetry", 1, 8, 1, kSyn));
    // A CHOICE, not an int: the twelve entries are note values, not a numeric
    // scale, so both the DAW's automation lane and the plugin's own control need
    // to show "1/8T" rather than "8". (The wire value is still the index, so this
    // is invisible to the protocol.)
    layout.add (std::make_unique<HideableChoice> (
        ParameterID { "synBeatDiv", 1 }, "Syn Beat Div", AlterCtrl::beatDivNames(), 5, kSyn));
    layout.add (std::make_unique<HideableBool> (
        ParameterID { "synMirror", 1 }, "Syn Mirror", false, kSyn));
    layout.add (std::make_unique<HideableBool> (
        ParameterID { "synBpmSync", 1 }, "Syn BPM Sync", false, kSyn));
    layout.add (std::make_unique<HideableChoice> (
        ParameterID { "synTone", 1 }, "Syn Tone Colour",
        StringArray { "Base colour", "Tone" }, 0, kSyn));

    // ---- Synesthesia base colour (the controller's "Color..." picker) ----
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synColHue", 1 }, "Syn Colour Hue",
        steppedRange (0.0f, 1.0f, 0.001f), 0.5f, kSyn));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synColSat", 1 }, "Syn Colour Sat",
        steppedRange (0.0f, 1.0f, 0.001f), 1.0f, kSyn));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "synColBri", 1 }, "Syn Colour Bright",
        steppedRange (0.0f, 1.0f, 0.001f), 1.0f, kSyn));

    // ---- Chladni (continuous) ----
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "chAspect", 1 }, "Chladni Aspect",
        steppedRange (0.25f, 4.0f, 0.01f), 1.0f, kChl));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "chSandSmooth", 1 }, "Chladni Sand Smooth",
        steppedRange (0.0f, 1.0f, 0.01f), 0.5f, kChl));

    // ---- Chladni (quantized) ----
    layout.add (std::make_unique<HideableInt> (
        ParameterID { "chShift", 1 }, "Chladni Shift", 0, 6, 0, kChl));
    layout.add (std::make_unique<HideableInt> (
        ParameterID { "chParticles", 1 }, "Chladni Particles", 1000, 15000, 5000, kChl));
    layout.add (std::make_unique<HideableChoice> (
        ParameterID { "chMaterial", 1 }, "Chladni Material",
        StringArray { "Aluminium", "Steel", "Glass", "Acrylic" }, 1, kChl));
    layout.add (std::make_unique<HideableInt> (
        ParameterID { "chM", 1 }, "Chladni M", 1, 12, 2, kChl));
    layout.add (std::make_unique<HideableInt> (
        ParameterID { "chN", 1 }, "Chladni N", 1, 12, 3, kChl));
    layout.add (std::make_unique<HideableBool> (
        ParameterID { "chReactive", 1 }, "Chladni Audio Reactive", true, kChl));
    layout.add (std::make_unique<HideableChoice> (
        ParameterID { "chTone", 1 }, "Chladni Tone Colour",
        StringArray { "Base colour", "Tone" }, 0, kChl));

    // Preset combo: "Custom" + the 12 classic (m,n) figures. Picking a preset
    // switches the module to manual mode and sets m/n, exactly like the app.
    {
        StringArray presetNames { "Custom" };
        for (auto& pr : AlterCtrl::kChladniPresets)
            presetNames.add ("(" + String (pr[0]) + "," + String (pr[1]) + ")");
        layout.add (std::make_unique<HideableChoice> (
            ParameterID { "chPreset", 1 }, "Chladni Preset", presetNames, 0, kChl));
    }

    // ---- Chladni base colour (sand colour picker) ----
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "chColHue", 1 }, "Chladni Colour Hue",
        steppedRange (0.0f, 1.0f, 0.001f), 0.5f, kChl));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "chColSat", 1 }, "Chladni Colour Sat",
        steppedRange (0.0f, 1.0f, 0.001f), 1.0f, kChl));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "chColBri", 1 }, "Chladni Colour Bright",
        steppedRange (0.0f, 1.0f, 0.001f), 1.0f, kChl));

    // ---- Geometry (continuous) ----
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoSmooth", 1 }, "Geo Smooth",
        steppedRange (0.01f, 0.99f, 0.01f), 0.15f, kGeo));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoZoom", 1 }, "Geo Zoom",
        steppedRange (0.5f, 2.0f, 0.01f), 1.0f, kGeo));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoRotation", 1 }, "Geo Rotation",
        steppedRange (-360.0f, 360.0f, 1.0f), 0.0f, kGeo));  // bipolar (matches controller)
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoSaturation", 1 }, "Geo Saturation",
        steppedRange (0.0f, 2.0f, 0.01f), 1.0f, kGeo));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoBrightness", 1 }, "Geo Brightness",
        steppedRange (0.0f, 2.0f, 0.01f), 1.0f, kGeo));   // overall light output (1 = neutral)
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoBloom", 1 }, "Geo Bloom",
        steppedRange (-1.0f, 1.0f, 0.01f), 0.2f, kGeo));     // bipolar (matches controller)
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoSpeed", 1 }, "Geo Speed",
        steppedRange (-2.0f, 2.0f, 0.01f), 1.0f, kGeo));      // bipolar (matches controller)
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoTri", 1 }, "Geo Triangle",
        steppedRange (0.0f, 1.0f, 0.01f), 0.5f, kGeo));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoSquare", 1 }, "Geo Square",
        steppedRange (0.0f, 1.0f, 0.01f), 0.5f, kGeo));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoCircle", 1 }, "Geo Circle",
        steppedRange (0.0f, 1.0f, 0.01f), 0.5f, kGeo));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoRandom", 1 }, "Geo Random",
        steppedRange (0.0f, 1.0f, 0.01f), 1.0f, kGeo));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoReact", 1 }, "Geo React",
        steppedRange (0.0f, 1.0f, 0.01f), 0.0f, kGeo));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoDepth", 1 }, "Geo Depth",
        steppedRange (0.0f, 1.0f, 0.01f), 0.7f, kGeo));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoTunnel", 1 }, "Geo Tunnel",
        steppedRange (0.0f, 1.0f, 0.01f), 0.0f, kGeo));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoAperture", 1 }, "Geo Aperture",
        steppedRange (0.0f, 1.0f, 0.01f), 0.8f, kGeo));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoGlobalRot", 1 }, "Geo Module Rotation",
        steppedRange (-360.0f, 360.0f, 1.0f), 0.0f, kGeo));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoBpm", 1 }, "Geo BPM",
        steppedRange (20.0f, 400.0f, 0.1f), 100.0f, kGeo));

    // ---- Geometry (quantized) ----
    layout.add (std::make_unique<HideableInt> (
        ParameterID { "geoSymmetry", 1 }, "Geo Symmetry", 1, 8, 1, kGeo));
    layout.add (std::make_unique<HideableInt> (
        ParameterID { "geoComplexity", 1 }, "Geo Complexity", 1, 256, 12, kGeo));
    layout.add (std::make_unique<HideableChoice> (
        ParameterID { "geoBeatDiv", 1 }, "Geo Beat Div", AlterCtrl::beatDivNames(), 5, kGeo));
    layout.add (std::make_unique<HideableBool> (
        ParameterID { "geoBpmSync", 1 }, "Geo BPM Sync", false, kGeo));
    layout.add (std::make_unique<HideableBool> (
        ParameterID { "geoMirror", 1 }, "Geo Mirror", false, kGeo));   // 4-fold reflective fold (shared toggle in the controller)
    layout.add (std::make_unique<HideableChoice> (
        ParameterID { "geoTone", 1 }, "Geo Tone Colour",
        StringArray { "Base colour", "Tone" }, 0, kGeo));

    // ---- Geometry base colour (the controller's "Color..." picker) ----
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoColHue", 1 }, "Geo Colour Hue",
        steppedRange (0.0f, 1.0f, 0.001f), 0.5f, kGeo));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoColSat", 1 }, "Geo Colour Sat",
        steppedRange (0.0f, 1.0f, 0.001f), 1.0f, kGeo));
    layout.add (std::make_unique<HideableFloat> (
        ParameterID { "geoColBri", 1 }, "Geo Colour Bright",
        steppedRange (0.0f, 1.0f, 0.001f), 1.0f, kGeo));

    // ---- Padding so Ableton opens with an EMPTY device panel (like Vital) ----
    // Live shows every parameter as a slider only for plug-ins with <= 64
    // AUTOMATABLE params; above that it opens empty and you add knobs via Configure
    // (touch them in our editor). So the reserves must be automatable, and we use
    // enough (70) that the count stays > 64 even when Hideable hides a module group.
    //
    // LIVE ONLY. Every other host just lists them as 70 useless "Reserved NN"
    // entries. Note that JUCE derives VST3 param IDs by hashing the ParameterID
    // string, not from the index, so omitting these does NOT shift the IDs of the
    // real parameters — existing projects keep their automation either way.
    if (alterHostIsAbletonLive())
    {
        for (int i = 1; i <= 70; ++i)
            layout.add (std::make_unique<juce::AudioParameterFloat> (
                ParameterID { "reserved" + juce::String (i).paddedLeft ('0', 2), 1 },
                "Reserved " + juce::String (i),
                juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f));
    }

    return layout;
}

// ===================================================================
AlterListenerAudioProcessor::AlterListenerAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor (BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "ALTERLISTENER", createParameterLayout())
#else
    : apvts (*this, nullptr, "ALTERLISTENER", createParameterLayout())
#endif
{
    // Unique non-zero instance id (persisted via get/setStateInformation)
    do { instanceId = (juce::uint32) juce::Random::getSystemRandom().nextInt(); }
    while (instanceId == 0);
    rebuildInstanceName();

    // Pure listener has no parameters, so only attach listeners that exist.
    for (uint8_t pid = AlterCtrl::SynSmooth; pid <= AlterCtrl::LastParam; ++pid)
        if (apvts.getParameter (AlterCtrl::idForParam ((AlterCtrl::ParamId) pid)) != nullptr)
            apvts.addParameterListener (AlterCtrl::idForParam ((AlterCtrl::ParamId) pid), this);
    if (apvts.getParameter ("mode") != nullptr)
        apvts.addParameterListener ("mode", this);

   #ifdef ALTER_PURE_LISTENER
    mode.store (0);   // pure listener (streams audio only)
   #else
    mode.store (1);   // Creator: control + listen
   #endif

    // wire each hideable param to the shared active-group flag.
    // LIVE ONLY: leaving `active` null makes automatableNow() return true forever,
    // which is what we want in hosts that cache the parameter list at scan time —
    // a param they already published must not start reporting isAutomatable()==false.
    if (alterHostIsAbletonLive())
        for (auto* p : getParameters())
            if (auto* h = dynamic_cast<HideableParam*> (p))
                h->active = &activeGroup;

    socket = std::make_unique<juce::DatagramSocket>();
    socket->setEnablePortReuse (true);
    socket->setMulticastLoopbackEnabled (false);
    socket->bindToPort (0);

    senderThread = std::make_unique<SenderThread> (ringBuffer, *socket, kPort,
                                                   instanceId, instanceName,
                                                   [this] (const uint8_t* d, int n) { handleIncomingPacket (d, n); });
    senderThread->buildCqt      = [this] (uint8_t* out) { return buildCqtPacket (out); };
    senderThread->cqtWanted     = [this] { return (dataNeeds.load() & 32) != 0; };
    senderThread->flushControls = [this] { flushPendingControlValues(); };
    senderThread->hooksReady();   // publish all three; the thread is already running

    // Allocate to the MAX size once; the active window uses a subset (fftSize).
    fftIn .allocate (kMaxFftSize, true);
    fftOut.allocate (kMaxFftSize, true);
    fifo.setSize (1, kMaxFftSize);
    fifo.clear();
}

AlterListenerAudioProcessor::~AlterListenerAudioProcessor()
{
    if (apvts.getParameter ("mode") != nullptr)
        apvts.removeParameterListener ("mode", this);
    for (uint8_t pid = AlterCtrl::SynSmooth; pid <= AlterCtrl::LastParam; ++pid)
        if (apvts.getParameter (AlterCtrl::idForParam ((AlterCtrl::ParamId) pid)) != nullptr)
            apvts.removeParameterListener (AlterCtrl::idForParam ((AlterCtrl::ParamId) pid), this);

    // Sender thread treba zastavit pred zrusenim socketu
    senderThread.reset();
    socket.reset();
}

// ===================================================================
// Parameter change -> send control packet (only in Control mode)
// ===================================================================
void AlterListenerAudioProcessor::parameterChanged (const juce::String& paramID, float newValue)
{
    if (paramID == "mode")
    {
        juce::ignoreUnused (newValue);
        mode.store (1);                 // always Control (controls + listens)
        sendModePacket (1);
        const auto t = targetModuleId.load();
        if (t != 0) sendBindPacket (t); // ask ALTER for current values
        return;
    }

    if (suppressOutgoing.load())
        return;
    if (mode.load() != 1)
        return;

    const auto target = targetModuleId.load();
    if (target == 0)
        return;

    const auto pid = AlterCtrl::paramForId (paramID);
    if (pid == AlterCtrl::None)
        return;

    juce::ignoreUnused (target);

    // NOT sent from here. This runs on the AUDIO thread, once per block per moving
    // automation lane; park the value and let the sender thread emit it at ~60 Hz
    // (see PendingCtrl). Sending per block is what used to flood ALTER's message
    // thread and stutter the whole HUD.
    queueControlValue ((uint8_t) pid, newValue);
}

void AlterListenerAudioProcessor::setTargetModule (juce::uint32 id)
{
    // Anything still queued was aimed at the PREVIOUS module — drop it rather than
    // let it land on the new one.
    discardPendingControlValues();

    targetModuleId.store (id);
    updateActiveGroup();       // show only the bound module's param group
    if (mode.load() == 1)
        sendBindPacket (id);   // ALTER will reply with a 'V' value-sync packet
}

void AlterListenerAudioProcessor::updateActiveGroup()
{
    int g = 255;   // 255 = show both groups (nothing bound / unknown type)
    const auto t = targetModuleId.load();
    if (t != 0)
    {
        const juce::ScopedLock sl (registryLock);
        for (auto& e : registry)
            if (e.id == t)
            {
                g = (e.type == AlterCtrl::TypeSynesthesia) ? 0
                  : (e.type == AlterCtrl::TypeChladni)     ? 1
                  : (e.type == AlterCtrl::TypeGeometry)    ? 2 : 255;
                break;
            }
    }

    if (activeGroup.exchange (g) != g && alterHostIsAbletonLive())
    {
        // ask the host to re-read parameter info (drops the hidden group from
        // the automation list in hosts that honour kParamTitlesChanged).
        // LIVE ONLY: Cubase/Nuendo and Studio One treat a parameterInfoChanged
        // burst as a reason to rebuild their automation view, and Pro Tools can
        // drop lanes outright. Since non-Live hosts never hide anything (see the
        // constructor), there is nothing for them to re-read anyway.
        juce::MessageManager::callAsync ([this]
        {
            updateHostDisplay (juce::AudioProcessorListener::ChangeDetails().withParameterInfoChanged (true));
        });
    }
}

// ===================================================================
// Outgoing control packets (pushed to ring buffer, sent by SenderThread)
// ===================================================================
void AlterListenerAudioProcessor::sendModePacket (int m)
{
    // NOTE: mode announce moved 'M' → 'O' — 'M' now carries MIDI notes.
    uint8_t buf[kAlterHeaderSize + 1];
    writeHeader (buf, (uint8_t) 'O');
    buf[kAlterHeaderSize] = (uint8_t) m;
    ringBuffer.push (buf, (int) sizeof (buf));
}

void AlterListenerAudioProcessor::sendBindPacket (juce::uint32 moduleId)
{
    uint8_t buf[kAlterHeaderSize + sizeof (juce::uint32)];
    writeHeader (buf, (uint8_t) 'B');
    std::memcpy (buf + kAlterHeaderSize, &moduleId, sizeof (moduleId));
    ringBuffer.push (buf, (int) sizeof (buf));
}

void AlterListenerAudioProcessor::sendControlPacket (juce::uint32 moduleId, uint8_t paramId, float value)
{
    uint8_t buf[kAlterHeaderSize + sizeof (juce::uint32) + 1 + sizeof (float)];
    writeHeader (buf, (uint8_t) 'C');
    uint8_t* p = buf + kAlterHeaderSize;
    std::memcpy (p, &moduleId, sizeof (moduleId)); p += sizeof (moduleId);
    *p++ = paramId;
    std::memcpy (p, &value, sizeof (float));
    ringBuffer.push (buf, (int) sizeof (buf));
}

// ===================================================================
// Incoming packets from ALTER (called on the sender thread)
// ===================================================================
void AlterListenerAudioProcessor::handleIncomingPacket (const uint8_t* data, int size)
{
    if (size < kAlterHeaderSize) return;
    if (! (data[0]=='A' && data[1]=='L' && data[2]=='T' && data[3]=='2')) return;

    const uint8_t  type    = data[4];
    const uint8_t* payload = data + kAlterHeaderSize;
    const int      bytes   = size - kAlterHeaderSize;

    if (type == 'G')   // module registry
    {
        if (bytes < 2) return;
        int pos = 0;
        themeIndex.store (payload[pos++]);   // ALTER visual theme index
        const int count = payload[pos++];

        std::vector<ModuleEntry> entries;
        entries.reserve ((size_t) count);

        bool complete = true;
        for (int i = 0; i < count; ++i)
        {
            if (pos + 14 > bytes) { complete = false; break; }   // id(4)+colour(4)+type(1)+ownerInstId(4)+nameLen(1)

            ModuleEntry e;
            std::memcpy (&e.id,     payload + pos, 4); pos += 4;
            std::memcpy (&e.colour, payload + pos, 4); pos += 4;
            e.type = payload[pos++];
            std::memcpy (&e.ownerInstId, payload + pos, 4); pos += 4;

            const int nameLen = payload[pos++];
            if (pos + nameLen > bytes) { complete = false; break; }
            e.name = juce::String::fromUTF8 ((const char*) (payload + pos), nameLen);
            pos += nameLen;

            if (pos + 1 > bytes) { complete = false; break; }
            const int ownerLen = payload[pos++];
            if (pos + ownerLen > bytes) { complete = false; break; }
            e.ownerName = juce::String::fromUTF8 ((const char*) (payload + pos), ownerLen);
            pos += ownerLen;

            entries.push_back (std::move (e));
        }

        // Trailer (newer ALTER builds only): the two colours the CUSTOM theme is
        // derived from. Older builds simply do not append them, and `pos` then sits
        // at the end of the payload — hence the length check rather than a version
        // byte. Without these the plugin has nothing to build the custom palette
        // from, which is why a custom theme set in the app never showed up here.
        // (Only when the module list parsed cleanly — on a truncated packet `pos`
        // stops mid-list and those bytes are module data, not colours.)
        if (complete && pos + 8 <= bytes)
        {
            juce::uint32 pri = 0, sec = 0;
            std::memcpy (&pri, payload + pos, 4); pos += 4;
            std::memcpy (&sec, payload + pos, 4); pos += 4;
            themeCustomPrimary.store (pri);
            themeCustomSecondary.store (sec);
        }

        {
            const juce::ScopedLock sl (registryLock);
            registry.swap (entries);
        }
        registryVersion.fetch_add (1);
        updateActiveGroup();   // bound module's type may now be known
    }
    else if (type == 'V')   // value sync for a bound module
    {
        if (bytes < 5) return;
        juce::uint32 moduleId = 0;
        std::memcpy (&moduleId, payload, 4);
        int pos = 4;
        const int count = payload[pos++];

        std::vector<std::pair<uint8_t, float>> values;
        values.reserve ((size_t) count);
        for (int i = 0; i < count; ++i)
        {
            if (pos + 1 + (int) sizeof (float) > bytes) break;
            const uint8_t pid = payload[pos++];
            float v = 0.0f;
            std::memcpy (&v, payload + pos, sizeof (float));
            pos += (int) sizeof (float);
            values.emplace_back (pid, v);
        }

        applyValueSync (moduleId, values);
    }
    else if (type == 'N')   // data-needs mask: which packet types ALTER consumes
    {
        if (bytes >= 1)
            dataNeeds.store (payload[0]);
    }
    else if (type == 'B')   // FFT order (resolution) chosen globally by ALTER
    {
        if (bytes >= 1)
            requestedFftOrder.store ((int) payload[0]);
    }
}

void AlterListenerAudioProcessor::applyValueSync (juce::uint32 moduleId,
                                                  const std::vector<std::pair<uint8_t, float>>& values)
{
    if (moduleId != targetModuleId.load())
        return;   // stale / not for us

    juce::MessageManager::callAsync ([this, values]()
    {
        suppressOutgoing.store (true);
        for (auto& kv : values)
        {
            const char* id = AlterCtrl::idForParam ((AlterCtrl::ParamId) kv.first);
            if (auto* p = apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (kv.second));
        }
        suppressOutgoing.store (false);
    });
}

// ===================================================================
void AlterListenerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    sampleRateHz     = sampleRate;
    samplesPerPacket = (int) juce::jmax (1.0, sampleRate / 200.0);
    sampleCounter    = 0;
    rmsSmooth        = 0.0f;
    blockPeak        = 0.0f;

    truePeak.prepare (2);   // inter-sample (true) peak meter, stereo
    truePeak.reset();
    cqt.prepare (sampleRate);   // constant-Q analyzer

    computeKWeightCoeffs (sampleRate);

    lufsBufferSize = (int) (sampleRate * 0.4);
    lufsBuffer.assign ((size_t) lufsBufferSize, 0.0f);
    lufsWritePos   = 0;
    lufsRunningSum = 0.0;

    waveformL.fill (0.0f);
    waveformR.fill (0.0f);
    waveformWrite = 0;
}

// ---------------------------------------------------------------
// Enqueue funkcie - volane z audio vlakna, BEZ blokovania
// ---------------------------------------------------------------
void AlterListenerAudioProcessor::enqueueFloatPacket (uint8_t type, float value)
{
    // skip if ALTER doesn't consume this type (R=1,P=2,L=4)
    const juce::uint8 bit = (type == 'R') ? 1 : (type == 'P') ? 2 : (type == 'L') ? 4 : 0;
    if (bit != 0 && (dataNeeds.load() & bit) == 0)
        return;

    uint8_t buf[kAlterHeaderSize + sizeof (float)];
    writeHeader (buf, type);
    std::memcpy (buf + kAlterHeaderSize, &value, sizeof (float));
    ringBuffer.push (buf, (int) sizeof (buf));
}

void AlterListenerAudioProcessor::enqueueWaveformPacket()
{
    if ((dataNeeds.load() & 16) == 0)   // W not consumed
        return;

    constexpr int dataBytes  = kWaveformSize * 2 * (int) sizeof (float);
    constexpr int packetSize = kAlterHeaderSize + dataBytes;
    uint8_t packet[packetSize];

    writeHeader (packet, (uint8_t) 'W');

    uint8_t* dst = packet + kAlterHeaderSize;
    for (int i = 0; i < kWaveformSize; ++i)
    {
        const int idx = (waveformWrite + i) % kWaveformSize;
        std::memcpy (dst + (size_t)(i * 2)     * sizeof (float), &waveformL[(size_t) idx], sizeof (float));
        std::memcpy (dst + (size_t)(i * 2 + 1) * sizeof (float), &waveformR[(size_t) idx], sizeof (float));
    }

    ringBuffer.push (packet, packetSize);
}

// Apply a pending FFT-size change (from a 'B' control packet). Audio thread; the
// reassignment of the FFT engine is the only allocation and happens rarely.
void AlterListenerAudioProcessor::applyFftOrderIfChanged()
{
    const int want = juce::jlimit (kFftOrderMin, kFftOrderMax, requestedFftOrder.load());
    if (want == fftOrder) return;

    fftOrder       = want;
    fftSize        = 1 << want;
    binsOut        = fftSize / 2;
    hopSamples     = fftSize / 4;
    fft            = juce::dsp::FFT (want);
    fifoWrite      = 0;
    hopAccumulator = 0;
    fifo.clear();
}

void AlterListenerAudioProcessor::pushSamplesToFifo (const float* samples, int numSamples)
{
    auto* mono = fifo.getWritePointer (0);
    for (int i = 0; i < numSamples; ++i)
    {
        mono[fifoWrite] = samples[i];
        fifoWrite = (fifoWrite + 1) % fftSize;
    }
}

void AlterListenerAudioProcessor::performFftAndEnqueue()
{
    if ((dataNeeds.load() & 8) == 0)   // F (FFT) not consumed
        return;
    auto* rd = fifo.getReadPointer (0);
    for (int i = 0; i < fftSize; ++i)
    {
        const float win = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi
                              * (float) i / (float) (fftSize - 1)));
        const int   idx = (fifoWrite + i) % fftSize;
        fftIn[i].real (rd[idx] * win);
        fftIn[i].imag (0.0f);
    }

    fft.perform (fftIn, fftOut, false);

    const float norm = 4.0f / (float) fftSize;

    const int packetSize = kAlterHeaderSize + binsOut * (int) sizeof (float);
    uint8_t packet[kAlterHeaderSize + kMaxBinsOut * (int) sizeof (float)];
    writeHeader (packet, (uint8_t) 'F');

    float bins[kMaxBinsOut];
    for (int k = 0; k < binsOut; ++k)
    {
        const float re  = fftOut[k].real();
        const float im  = fftOut[k].imag();
        const float mag = std::sqrt (re * re + im * im) * norm;

        float dB = 20.0f * std::log10 (juce::jmax (mag, 1.0e-12f));
        dB = juce::jlimit (kDbFloor, 0.0f, dB);
        bins[k] = juce::jlimit (0.0f, 1.0f, (dB - kDbFloor) / (-kDbFloor));
    }
    std::memcpy (packet + kAlterHeaderSize, bins, (size_t) binsOut * sizeof (float));

    ringBuffer.push (packet, packetSize);
}

// ---- MIDI capture: track held notes, enqueue 'M' ("ALTM") packets ----------
void AlterListenerAudioProcessor::handleMidiAndEnqueue (const juce::MidiBuffer& midi)
{
    bool changed = false;
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn (true))                     // velocity-0 note-on == note-off
        {
            const int n = m.getNoteNumber();
            if (midiVel[(size_t) n] == 0) ++midiActiveCount;
            midiVel[(size_t) n] = (juce::uint8) juce::jmax (1, (int) m.getVelocity());
            changed = true;
        }
        else if (m.isNoteOff())
        {
            const int n = m.getNoteNumber();
            if (midiVel[(size_t) n] != 0) { midiVel[(size_t) n] = 0; --midiActiveCount; changed = true; }
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            if (midiActiveCount > 0) { midiVel.fill (0); midiActiveCount = 0; changed = true; }
        }
    }

    const auto now = juce::Time::getMillisecondCounter();
    const bool heartbeat = midiActiveCount > 0 && now - lastMidiSendMs >= 100;
    if (! changed && ! heartbeat)
        return;
    lastMidiSendMs = now;

    // 'M' packet: count + (note, vel) pairs — held notes only, ascending, max 32
    uint8_t buf[kAlterHeaderSize + 1 + 32 * 2];
    writeHeader (buf, (uint8_t) 'M');
    int cnt = 0;
    uint8_t* p = buf + kAlterHeaderSize + 1;
    for (int n = 0; n < 128 && cnt < 32; ++n)
        if (midiVel[(size_t) n] != 0)
        {
            *p++ = (uint8_t) n;
            *p++ = midiVel[(size_t) n];
            ++cnt;
        }
    buf[kAlterHeaderSize] = (uint8_t) cnt;
    ringBuffer.push (buf, kAlterHeaderSize + 1 + cnt * 2);
}

void AlterListenerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                 juce::MidiBuffer& midi)
{
    handleMidiAndEnqueue (midi);   // exact notes from the DAW → 'M' packets

    const int numSamples = buffer.getNumSamples();
    const int numCh      = buffer.getNumChannels();

    // ---- RMS + True Peak ----
    {
        double sum2 = 0.0;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* d = buffer.getReadPointer (ch);
            const int tpCh = juce::jmin (ch, 1);   // meter has 2 channel states
            for (int i = 0; i < numSamples; ++i)
            {
                sum2 += (double) d[i] * (double) d[i];
                // TRUE (inter-sample) peak — 4x oversampled, not just max |sample|
                blockPeak = juce::jmax (blockPeak, truePeak.processSample (tpCh, d[i]));
            }
        }
        const float rms = (float) std::sqrt (sum2 / (double) (numSamples * juce::jmax (1, numCh)));
        rmsSmooth += 0.1f * (rms - rmsSmooth);

        sampleCounter += numSamples;
        if (sampleCounter >= samplesPerPacket)
        {
            sampleCounter = 0;
            enqueueFloatPacket ('R', juce::jlimit (0.0f, 1.0f, rmsSmooth));
            enqueueFloatPacket ('P', blockPeak);
            blockPeak = 0.0f;

            const double meanSq = (lufsBufferSize > 0)
                                ? lufsRunningSum / (double) lufsBufferSize : 0.0;
            const float lufs = (meanSq > 1.0e-10)
                             ? (float) (-0.691 + 10.0 * std::log10 (meanSq))
                             : -100.0f;
            enqueueFloatPacket ('L', lufs);
        }
    }

    // ---- LUFS K-weighted accumulation (ITU-R BS.1770: kanaly sa scitaju) ----
    {
        const int chCount = juce::jmin (numCh, 2);
        for (int i = 0; i < numSamples; ++i)
        {
            float kSqSum = 0.0f;
            for (int ch = 0; ch < chCount; ++ch)
            {
                float s = buffer.getReadPointer (ch)[i];
                s = kWeightStage1[ch].process (s);
                s = kWeightStage2[ch].process (s);
                kSqSum += s * s;
            }

            if (lufsBufferSize > 0)
            {
                lufsRunningSum -= (double) lufsBuffer[(size_t) lufsWritePos];
                lufsBuffer[(size_t) lufsWritePos] = kSqSum;
                lufsRunningSum += (double) kSqSum;
                if (lufsRunningSum < 0.0) lufsRunningSum = 0.0;
                lufsWritePos = (lufsWritePos + 1) % lufsBufferSize;
            }
        }
    }

    // ---- Stereo waveform buffer ----
    {
        const float* chL = buffer.getReadPointer (0);
        const float* chR = (numCh >= 2) ? buffer.getReadPointer (1) : chL;
        for (int i = 0; i < numSamples; ++i)
        {
            waveformL[(size_t) waveformWrite] = chL[i];
            waveformR[(size_t) waveformWrite] = chR[i];
            waveformWrite = (waveformWrite + 1) % kWaveformSize;
        }
    }

    // Apply any pending FFT-size change requested by ALTER ('B' packet)
    applyFftOrderIfChanged();

    const bool needCqt = (dataNeeds.load() & 32) != 0;

    // ---- Mono mix pre FFT ----
    for (int i = 0; i < numSamples; ++i)
    {
        float s = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            s += buffer.getReadPointer (ch)[i];
        s *= (1.0f / juce::jmax (1, numCh));
        pushSamplesToFifo (&s, 1);
        if (needCqt) cqt.pushSamples (&s, 1);
    }

    // ---- FFT + waveform na hop ----
    hopAccumulator += numSamples;
    while (hopAccumulator >= hopSamples)
    {
        performFftAndEnqueue();
        enqueueWaveformPacket();
        hopAccumulator -= hopSamples;
    }
    // CQT itself is computed on the SenderThread (worker), off the audio thread.

    // ---- Host transport ('T', ~23/s): tempo + PPQ beat position + playing ----
    // ALTER phase-locks its BPM-synced visuals to this grid, so beats land
    // exactly on the DAW's beats (play from 1.1.1 → guaranteed alignment).
    if (auto* phd = getPlayHead())
    {
        if (auto pos = phd->getPosition())
        {
            // Hosts that only report tempo while rolling (FL Studio, Pro Tools)
            // would otherwise blank the BPM every time the user hits stop.
            const float   rawBpm   = (float) pos->getBpm().orFallback (0.0);
            if (rawBpm > 0.0f && std::isfinite (rawBpm))
                lastGoodBpm = rawBpm;
            const float   tBpm     = rawBpm > 0.0f ? rawBpm : lastGoodBpm;
            const double  tPpq     = pos->getPpqPosition().orFallback (-1.0);
            const bool    isPlay   = pos->getIsPlaying();
            const uint8_t playing  = isPlay ? 1 : 0;

            // ---- Sync IMPULSE ('S'): fire on transport (re)start ----
            // A single impulse whenever playback begins, or the play position jumps
            // backwards (loop / manual relocate). ALTER resets its beat phase to this
            // instant, then free-runs at the MANUAL BPM/division. This is the only thing
            // the DAW must send to lock the downbeat — tempo stays manual on the visual.
            const bool startEdge  = isPlay && ! prevPlaying;
            const bool jumpedBack = isPlay && prevPlaying && tPpq >= 0.0 && tPpq < prevPpq - 1.0e-6;
            if (startEdge || jumpedBack)
            {
                uint8_t sbuf[kAlterHeaderSize + 8];
                writeHeader (sbuf, (uint8_t) 'S');
                std::memcpy (sbuf + kAlterHeaderSize, &tPpq, 8);   // ppq at the impulse (for reference)
                ringBuffer.push (sbuf, (int) sizeof (sbuf));
            }
            prevPlaying = isPlay;
            prevPpq     = tPpq;

            // ---- Host transport ('T', ~23/s): tempo + PPQ + playing (throttled) ----
            transportAccumulator += numSamples;
            if (transportAccumulator >= 2048)
            {
                transportAccumulator = 0;
                uint8_t buf[kAlterHeaderSize + 13];
                writeHeader (buf, (uint8_t) 'T');
                std::memcpy (buf + kAlterHeaderSize,     &tBpm, 4);
                std::memcpy (buf + kAlterHeaderSize + 4, &tPpq, 8);
                buf[kAlterHeaderSize + 12] = playing;
                ringBuffer.push (buf, (int) sizeof (buf));
            }
        }
    }
}

// Worker thread: read the analyzer ring (audio-filled; benign race), write a 'Q'.
int AlterListenerAudioProcessor::buildCqtPacket (uint8_t* out)
{
    cqt.compute (cqtScratch);
    const int n = juce::jmin ((int) cqtScratch.size(), 300);
    if (n <= 0) return 0;
    writeHeader (out, (uint8_t) 'Q');
    std::memcpy (out + kAlterHeaderSize, cqtScratch.data(), (size_t) n * sizeof (float));
    return kAlterHeaderSize + n * (int) sizeof (float);
}

void AlterListenerAudioProcessor::computeKWeightCoeffs (double fs)
{
    const double pi = juce::MathConstants<double>::pi;

    // Stage 1: High-shelf
    {
        const double f0    = 1681.974450955533;
        const double G     = 3.999843853973347;
        const double Q     = 0.7071752369554196;
        const double A     = std::pow (10.0, G / 40.0);
        const double w0    = 2.0 * pi * f0 / fs;
        const double sinw  = std::sin (w0), cosw = std::cos (w0);
        const double alpha = sinw / (2.0 * Q);
        const double sqA   = std::sqrt (A);
        const double a0 = (A+1.0) - (A-1.0)*cosw + 2.0*sqA*alpha;
        const double b0 = A*((A+1.0) + (A-1.0)*cosw + 2.0*sqA*alpha);
        const double b1 = -2.0*A*((A-1.0) + (A+1.0)*cosw);
        const double b2 = A*((A+1.0) + (A-1.0)*cosw - 2.0*sqA*alpha);
        const double a1 = 2.0*((A-1.0) - (A+1.0)*cosw);
        const double a2 = (A+1.0) - (A-1.0)*cosw - 2.0*sqA*alpha;
        for (int ch = 0; ch < 2; ++ch)
        {
            kWeightStage1[ch].b0=(float)(b0/a0); kWeightStage1[ch].b1=(float)(b1/a0);
            kWeightStage1[ch].b2=(float)(b2/a0); kWeightStage1[ch].a1=(float)(a1/a0);
            kWeightStage1[ch].a2=(float)(a2/a0); kWeightStage1[ch].reset();
        }
    }

    // Stage 2: High-pass
    {
        const double f0    = 38.13547087602444;
        const double Q     = 0.5003270373238773;
        const double w0    = 2.0 * pi * f0 / fs;
        const double sinw  = std::sin (w0), cosw = std::cos (w0);
        const double alpha = sinw / (2.0 * Q);
        const double a0 = 1.0 + alpha;
        const double b0 = (1.0+cosw)/2.0, b1 = -(1.0+cosw), b2 = (1.0+cosw)/2.0;
        const double a1 = -2.0*cosw, a2 = 1.0 - alpha;
        for (int ch = 0; ch < 2; ++ch)
        {
            kWeightStage2[ch].b0=(float)(b0/a0); kWeightStage2[ch].b1=(float)(b1/a0);
            kWeightStage2[ch].b2=(float)(b2/a0); kWeightStage2[ch].a1=(float)(a1/a0);
            kWeightStage2[ch].a2=(float)(a2/a0); kWeightStage2[ch].reset();
        }
    }
}

// ===================================================================
// State persistence (instanceId + target + APVTS parameters)
// ===================================================================
void AlterListenerAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree root ("ALTERLISTENER_STATE");
    root.setProperty ("instanceId",     (int) instanceId,             nullptr);
    root.setProperty ("targetModuleId", (int) targetModuleId.load(),  nullptr);
    root.addChild (apvts.copyState(), -1, nullptr);

    juce::MemoryOutputStream mos (dest, false);
    root.writeToStream (mos);
}

void AlterListenerAudioProcessor::setStateInformation (const void* data, int size)
{
    auto root = juce::ValueTree::readFromData (data, (size_t) size);

    if (root.isValid() && root.hasType ("ALTERLISTENER_STATE"))
    {
        const auto rid = (juce::uint32) (int) root.getProperty ("instanceId", 0);
        if (rid != 0)
        {
            instanceId = rid;
            rebuildInstanceName();
            if (senderThread != nullptr)
                senderThread->setIdentity (instanceId, instanceName);
        }

        targetModuleId.store ((juce::uint32) (int) root.getProperty ("targetModuleId", 0));

        auto child = root.getChildWithName (apvts.state.getType());
        if (child.isValid())
            apvts.replaceState (child);

        if (auto* mv = apvts.getRawParameterValue ("mode"))
            mode.store ((int) mv->load());

        // re-announce our restored mode/binding to ALTER
        sendModePacket (mode.load());
        if (mode.load() == 1 && targetModuleId.load() != 0)
            sendBindPacket (targetModuleId.load());
        updateActiveGroup();
    }
    else if (size >= (int) sizeof (juce::uint32) && data != nullptr)
    {
        // legacy format: raw instanceId only
        juce::uint32 restored = 0;
        std::memcpy (&restored, data, sizeof (restored));
        if (restored != 0)
        {
            instanceId = restored;
            rebuildInstanceName();
            if (senderThread != nullptr)
                senderThread->setIdentity (instanceId, instanceName);
        }
    }
}

// ===================================================================
// Editor
// ===================================================================
// ===================================================================
// Futuristic widget styling for the plugin editor — pill toggles, capsule
// sliders with a neon core, glowing buttons/combos. Mirrors the ALTER
// controller's look so the plugin feels like part of the same instrument.
// ===================================================================
class CreatorLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void setPalette (juce::Colour panel_, juce::Colour raised_, juce::Colour edge_,
                     juce::Colour tBright_, juce::Colour tNormal_, juce::Colour accent_)
    {
        panel = panel_; raised = raised_; edge = edge_;
        tBright = tBright_; tNormal = tNormal_; accent = accent_;

        setColour (juce::PopupMenu::backgroundColourId,            panel);
        setColour (juce::PopupMenu::textColourId,                  tNormal);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.5f));
        setColour (juce::PopupMenu::highlightedTextColourId,       tBright);
        setColour (juce::TextButton::buttonColourId,               raised);
        setColour (juce::TextButton::textColourOffId,              tNormal);
        setColour (juce::TextButton::textColourOnId,               tBright);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b,
                               const juce::Colour& bgCol, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.0f);
        const float corner = juce::jmin (8.0f, r.getHeight() * 0.3f);

        auto top    = bgCol.brighter (down ? 0.00f : over ? 0.25f : 0.12f);
        auto bottom = bgCol.darker   (down ? 0.10f : 0.35f);
        g.setGradientFill ({ top, r.getX(), r.getY(), bottom, r.getX(), r.getBottom(), false });
        g.fillRoundedRectangle (r, corner);

        g.setColour (top.brighter (0.8f).withAlpha (0.20f));       // lit top edge
        g.fillRoundedRectangle (r.withHeight (2.0f).reduced (corner * 0.6f, 0.0f), 1.0f);

        g.setColour ((over || down ? accent : edge.brighter (0.5f)).withAlpha (over ? 0.9f : 0.55f));
        g.drawRoundedRectangle (r, corner, 1.0f);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool isDown,
                       int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0, 0, (float) width, (float) height).reduced (0.5f);
        const float corner = 6.0f;

        const auto bgc = box.findColour (juce::ComboBox::backgroundColourId);
        g.setGradientFill ({ bgc.brighter (0.10f), r.getX(), r.getY(),
                             bgc.darker (0.25f),   r.getX(), r.getBottom(), false });
        g.fillRoundedRectangle (r, corner);

        g.setColour (edge.brighter (0.5f).withAlpha (0.55f));      // chevron "port" divider
        g.fillRect ((float) width - 24.0f, r.getY() + 4.0f, 1.0f, r.getHeight() - 8.0f);

        g.setColour ((isDown ? accent : edge.brighter (0.5f)).withAlpha (0.8f));
        g.drawRoundedRectangle (r, corner, 1.0f);

        juce::Path chevron;
        const float cx = (float) width - 13.0f, cy = (float) height * 0.5f;
        chevron.startNewSubPath (cx - 4.0f, cy - 2.0f);
        chevron.lineTo (cx, cy + 2.5f);
        chevron.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId));
        g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b,
                           bool over, bool) override
    {
        const float h = juce::jmin (18.0f, (float) b.getHeight() - 4.0f);
        const float w = h * 1.9f;
        auto track = juce::Rectangle<float> (2.0f, ((float) b.getHeight() - h) * 0.5f, w, h);
        const bool on = b.getToggleState();

        if (on)
            g.setGradientFill ({ accent.darker (0.5f), track.getX(), track.getY(),
                                 accent, track.getRight(), track.getBottom(), false });
        else
            g.setColour (raised);
        g.fillRoundedRectangle (track, h * 0.5f);

        g.setColour ((on ? accent.brighter (0.4f) : edge.brighter (0.6f))
                         .withAlpha (over ? 0.95f : 0.6f));
        g.drawRoundedRectangle (track, h * 0.5f, 1.0f);

        const float knobR = h - 5.0f;
        const float kx = on ? track.getRight() - knobR - 2.5f : track.getX() + 2.5f;
        if (on)
        {
            g.setColour (accent.withAlpha (0.20f));                // energised halo
            g.fillEllipse (kx - 3.5f, track.getY() - 1.0f, knobR + 7.0f, knobR + 7.0f);
        }
        g.setColour (on ? tBright : tNormal.withAlpha (0.6f));
        g.fillEllipse (kx, track.getY() + 2.5f, knobR, knobR);
        if (on)
        {
            g.setColour (accent.brighter (0.4f).withAlpha (0.9f));
            g.drawEllipse (kx - 1.0f, track.getY() + 1.5f, knobR + 2.0f, knobR + 2.0f, 1.1f);
        }

        if (b.getButtonText().isNotEmpty())
        {
            g.setColour (b.findColour (juce::ToggleButton::textColourId)
                           .withAlpha (b.isEnabled() ? 1.0f : 0.4f));
            g.setFont (juce::Font (juce::FontOptions (13.0f)));
            g.drawFittedText (b.getButtonText(),
                              b.getLocalBounds().withTrimmedLeft ((int) w + 8),
                              juce::Justification::centredLeft, 2);
        }
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minPos, float maxPos,
                           juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        if (style != juce::Slider::LinearHorizontal)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height,
                                              sliderPos, minPos, maxPos, style, s);
            return;
        }

        const float trackT = 5.0f;
        juce::Rectangle<float> track ((float) x, (float) y + (float) height * 0.5f - trackT * 0.5f,
                                      (float) width, trackT);

        g.setColour (raised);
        g.fillRoundedRectangle (track, trackT * 0.5f);

        auto filled = track.withWidth (juce::jmax (trackT, sliderPos - (float) x));
        g.setGradientFill ({ accent.darker (0.55f), track.getX(), track.getY(),
                             accent, track.getRight(), track.getY(), false });
        g.fillRoundedRectangle (filled, trackT * 0.5f);

        if (filled.getWidth() > 8.0f)                              // neon core
        {
            auto core = filled.reduced (3.0f, 1.8f);
            g.setColour (tBright.withAlpha (0.30f));
            g.fillRoundedRectangle (core, core.getHeight() * 0.5f);
        }

        const float r = 7.0f, cx = sliderPos, cy = track.getCentreY();
        g.setColour (accent.withAlpha (0.30f));
        g.fillEllipse (cx - r - 3, cy - r - 3, (r + 3) * 2, (r + 3) * 2);
        g.setGradientFill ({ tBright, cx, cy - r, accent, cx, cy + r, false });
        g.fillEllipse (cx - r, cy - r, r * 2, r * 2);
        g.setColour (accent.brighter (0.35f).withAlpha (0.9f));
        g.drawEllipse (cx - r, cy - r, r * 2, r * 2, 1.1f);
    }

private:
    juce::Colour panel   { 0xff0A1126 }, raised { 0xff101A38 }, edge { 0xff022D68 },
                 tBright { 0xffEAF3FF }, tNormal { 0xff96C6F2 }, accent { 0xff3D96E7 };
};

class AlterListenerEditor : public juce::AudioProcessorEditor,
                            private juce::ListBoxModel,
                            private juce::Timer
{
public:
    explicit AlterListenerEditor (AlterListenerAudioProcessor& p)
        : juce::AudioProcessorEditor (&p), proc (p)
    {
        moduleLabel.setText ("Target module", juce::dontSendNotification);
        moduleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (moduleLabel);

        moduleList.setModel (this);
        moduleList.setRowHeight (38);   // taller: fits the "in use on ..." second line
        moduleList.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff202028));
        addAndMakeVisible (moduleList);

        // scrollable parameter area (the rows live inside paramHolder)
        addAndMakeVisible (paramView);
        paramView.setViewedComponent (&paramHolder, false);
        paramView.setScrollBarsShown (true, false);

        // ── Synesthesia controls (same order as the ALTER controller editor) ──
        addSlider (AlterCtrl::TypeSynesthesia, "synCurveSmooth", "Ghost");
        addSlider (AlterCtrl::TypeSynesthesia, "synZoom",        "Zoom");
        addSlider (AlterCtrl::TypeSynesthesia, "synRotation",    "Rotation");
        addSlider (AlterCtrl::TypeSynesthesia, "synSymmetry",    "Symmetry");
        addToggle (AlterCtrl::TypeSynesthesia, "synMirror",      "Mirror");
        addSlider (AlterCtrl::TypeSynesthesia, "synSaturation",  "Saturation");
        addSlider (AlterCtrl::TypeSynesthesia, "synBrightness",  "Brightness");
        addSlider (AlterCtrl::TypeSynesthesia, "synBloom",       "Bloom");
        addSlider (AlterCtrl::TypeSynesthesia, "synSpeed",       "Speed");
        addToggle (AlterCtrl::TypeSynesthesia, "synBpmSync",     "BPM sync");
        addSlider (AlterCtrl::TypeSynesthesia, "synBpm",         "BPM");
        addCombo  (AlterCtrl::TypeSynesthesia, "synBeatDiv",     "Beat div",
                   AlterCtrl::beatDivNames());
        addSlider (AlterCtrl::TypeSynesthesia, "synFragment",    "Change");
        addSlider (AlterCtrl::TypeSynesthesia, "synTransmute",   "Transmute");
        addSlider (AlterCtrl::TypeSynesthesia, "synClear",       "Clear");
        addSlider (AlterCtrl::TypeSynesthesia, "synDenoise",     "Denoise");
        addSlider (AlterCtrl::TypeSynesthesia, "synTunnel",      "Tunnel");
        addSlider (AlterCtrl::TypeSynesthesia, "synVortex",      "Vortex");
        addSlider (AlterCtrl::TypeSynesthesia, "synReact",       "React");
        addCombo  (AlterCtrl::TypeSynesthesia, "synTone",        "Colour",
                   { "Base", "Tone" });
        addSlider (AlterCtrl::TypeSynesthesia, "synSmooth",      "Tone smooth");
        addSlider (AlterCtrl::TypeSynesthesia, "synColHue",      "Colour hue");
        addSlider (AlterCtrl::TypeSynesthesia, "synColSat",      "Colour sat");
        addSlider (AlterCtrl::TypeSynesthesia, "synColBri",      "Colour bright");

        // ── Chladni controls (same order as the ALTER controller editor) ──
        addToggle (AlterCtrl::TypeChladni, "chReactive",   "Audio reactive");
        {
            juce::StringArray presetNames { "Custom" };
            for (auto& pr : AlterCtrl::kChladniPresets)
                presetNames.add ("(" + juce::String (pr[0]) + "," + juce::String (pr[1]) + ")");
            addCombo (AlterCtrl::TypeChladni, "chPreset", "Preset", presetNames);
        }
        addSlider (AlterCtrl::TypeChladni, "chM",          "Mode m");
        addSlider (AlterCtrl::TypeChladni, "chN",          "Mode n");
        addSlider (AlterCtrl::TypeChladni, "chShift",      "Shift");
        addSlider (AlterCtrl::TypeChladni, "chAspect",     "Aspect ratio");
        addSlider (AlterCtrl::TypeChladni, "chSandSmooth", "Sand smooth");
        addSlider (AlterCtrl::TypeChladni, "chParticles",  "Particles");
        addCombo  (AlterCtrl::TypeChladni, "chMaterial",   "Material",
                   { "Aluminium", "Steel", "Glass", "Acrylic" });
        addCombo  (AlterCtrl::TypeChladni, "chTone",       "Colour",
                   { "Base", "Tone" });
        addSlider (AlterCtrl::TypeChladni, "chColHue",     "Colour hue");
        addSlider (AlterCtrl::TypeChladni, "chColSat",     "Colour sat");
        addSlider (AlterCtrl::TypeChladni, "chColBri",     "Colour bright");

        // ── Geometry controls (same order as the ALTER controller editor) ──
        addSlider (AlterCtrl::TypeGeometry, "geoSpeed",      "Speed");
        addToggle (AlterCtrl::TypeGeometry, "geoBpmSync",    "BPM sync");
        addSlider (AlterCtrl::TypeGeometry, "geoBpm",        "BPM");
        addCombo  (AlterCtrl::TypeGeometry, "geoBeatDiv",    "Beat div",
                   AlterCtrl::beatDivNames());
        addSlider (AlterCtrl::TypeGeometry, "geoComplexity", "Complexity");
        addSlider (AlterCtrl::TypeGeometry, "geoRandom",     "Random");
        addSlider (AlterCtrl::TypeGeometry, "geoReact",      "React");
        addSlider (AlterCtrl::TypeGeometry, "geoTri",        "Triangle");
        addSlider (AlterCtrl::TypeGeometry, "geoSquare",     "Square");
        addSlider (AlterCtrl::TypeGeometry, "geoCircle",     "Circle");
        addSlider (AlterCtrl::TypeGeometry, "geoSymmetry",   "Symmetry");
        addToggle (AlterCtrl::TypeGeometry, "geoMirror",     "Mirror");
        addSlider (AlterCtrl::TypeGeometry, "geoZoom",       "Zoom");
        addSlider (AlterCtrl::TypeGeometry, "geoTunnel",     "Tunnel");
        addSlider (AlterCtrl::TypeGeometry, "geoDepth",      "Depth");
        addSlider (AlterCtrl::TypeGeometry, "geoAperture",   "Aperture");
        addSlider (AlterCtrl::TypeGeometry, "geoRotation",   "Rotation");
        addSlider (AlterCtrl::TypeGeometry, "geoGlobalRot",  "Module rot");
        addSlider (AlterCtrl::TypeGeometry, "geoSaturation", "Saturation");
        addSlider (AlterCtrl::TypeGeometry, "geoBrightness", "Brightness");
        addSlider (AlterCtrl::TypeGeometry, "geoBloom",      "Bloom");
        addCombo  (AlterCtrl::TypeGeometry, "geoTone",       "Colour",
                   { "Base", "Tone" });
        addSlider (AlterCtrl::TypeGeometry, "geoSmooth",     "Tone smooth");
        addSlider (AlterCtrl::TypeGeometry, "geoColHue",     "Colour hue");
        addSlider (AlterCtrl::TypeGeometry, "geoColSat",     "Colour sat");
        addSlider (AlterCtrl::TypeGeometry, "geoColBri",     "Colour bright");

        setSize (560, 430);
        setLookAndFeel (&lnf);   // futuristic widgets (pill toggles, capsule sliders)
        rebuildModuleList();
        updateEnablement();
        applyTheme (proc.getThemeIndex());
        startTimerHz (8);
    }

    ~AlterListenerEditor() override
    {
        setLookAndFeel (nullptr);
        moduleList.setModel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (pal.bg);

        auto area = getLocalBounds().reduced (12, 0);

        g.setColour (pal.textBright);
        g.setFont (juce::Font (18.0f, juce::Font::bold));
        g.drawText ("ALTER Listener Controller",
                    area.removeFromTop (30).withTrimmedTop (6),
                    juce::Justification::centredLeft, false);

        g.setColour (pal.textNormal.withAlpha (0.9f));
        g.setFont (12.0f);
        const juce::String hint = proc.getMode() == 1
            ? "Control mode: automate the selected module from your DAW."
            : "Listener mode: sending audio data to ALTER.";
        g.drawText (hint, area.removeFromTop (16), juce::Justification::centredLeft, false);

        g.setColour (pal.textNormal.withAlpha (0.55f));
        g.setFont (11.0f);
        // Configure-on-touch is a Live concept; other hosts just show the full
        // automation list, so promising a "Configure panel" there is misleading.
        g.drawText (alterHostIsAbletonLive()
                        ? "Double-click a module to touch ALL its parameters (fills Ableton's Configure panel at once)."
                        : "Every parameter is in your DAW's automation list. Double-click a module to touch them all.",
                    area.removeFromTop (15), juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        area.removeFromTop (62); // title + 2 note lines

        // left: module list, right: scrollable params
        auto left  = area.removeFromLeft (200);
        moduleLabel.setBounds (left.removeFromTop (20));
        moduleList.setBounds (left);

        area.removeFromLeft (14);
        paramView.setBounds (area);

        // lay the visible rows out inside paramHolder (which the viewport scrolls)
        const int rowH = 26, gap = 4;
        const int vw   = juce::jmax (40, paramView.getWidth() - paramView.getScrollBarThickness());
        int y = 0;
        for (auto& r : rows)
        {
            if (! r.visible) { r.setBounds ({}); continue; }
            juce::Rectangle<int> row (0, y, vw, rowH);
            r.label->setBounds (row.removeFromLeft (110));
            if (r.slider) r.slider->setBounds (row);
            if (r.combo)  r.combo->setBounds (row);
            if (r.toggle) r.toggle->setBounds (row.removeFromLeft (60));
            y += rowH + gap;
        }
        paramHolder.setSize (vw, juce::jmax (y, paramView.getHeight()));
    }

private:
    struct Row
    {
        juce::uint8 group = 0;
        bool visible = false;
        std::unique_ptr<juce::Label>  label;
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::ComboBox> combo;
        std::unique_ptr<juce::ToggleButton> toggle;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   sAttach;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> cAttach;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   bAttach;

        void setBounds (juce::Rectangle<int> b)
        {
            if (label)  label->setBounds (b);
        }
    };

    void addSlider (juce::uint8 group, const juce::String& id, const juce::String& name)
    {
        Row r;
        r.group = group;
        r.label = std::make_unique<juce::Label> (juce::String(), name);
        r.label->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        paramHolder.addAndMakeVisible (*r.label);

        r.slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal,
                                                   juce::Slider::TextBoxRight);
        r.slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
        paramHolder.addAndMakeVisible (*r.slider);
        r.sAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            proc.getApvts(), id, *r.slider);

        // Two ways to hit an exact value, and neither of them is dragging:
        //   • double-click the TRACK  → back to the parameter's default
        //   • click the NUMBER        → type it (the text box is not read-only)
        // The attachment has already copied the parameter's range onto the slider
        // by this point, so we only have to look the default up.
        if (auto* rp = proc.getApvts().getParameter (id))
        {
            const auto& range = rp->getNormalisableRange();
            r.slider->setDoubleClickReturnValue (true, (double) range.convertFrom0to1 (rp->getDefaultValue()));

            // Show as many decimals as the step actually carries — 1 for BPM,
            // 2 for the 0..1 knobs — instead of JUCE's seven-digit default.
            int places = 0;
            for (double step = (double) range.interval; step > 0.0 && step < 1.0 && places < 4; ++places)
                step *= 10.0;
            r.slider->setNumDecimalPlacesToDisplay (places);
        }

        r.slider->setTooltip (name + " — double-click the slider for the default, "
                                     "click the number to type an exact value");

        rows.push_back (std::move (r));
    }

    void addToggle (juce::uint8 group, const juce::String& id, const juce::String& name)
    {
        Row r;
        r.group = group;
        r.label = std::make_unique<juce::Label> (juce::String(), name);
        r.label->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        paramHolder.addAndMakeVisible (*r.label);

        r.toggle = std::make_unique<juce::ToggleButton> (juce::String());
        paramHolder.addAndMakeVisible (*r.toggle);
        r.bAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            proc.getApvts(), id, *r.toggle);

        rows.push_back (std::move (r));
    }

    void addCombo (juce::uint8 group, const juce::String& id, const juce::String& name,
                   const juce::StringArray& items)
    {
        Row r;
        r.group = group;
        r.label = std::make_unique<juce::Label> (juce::String(), name);
        r.label->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        paramHolder.addAndMakeVisible (*r.label);

        r.combo = std::make_unique<juce::ComboBox>();
        for (int i = 0; i < items.size(); ++i)
            r.combo->addItem (items[i], i + 1);
        paramHolder.addAndMakeVisible (*r.combo);
        r.cAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            proc.getApvts(), id, *r.combo);

        rows.push_back (std::move (r));
    }

    // The type of the module this plugin is BOUND to (drives which knobs show).
    juce::uint8 boundModuleType() const
    {
        const auto t = proc.getTargetModule();
        if (t != 0)
            for (auto& e : entries)
                if (e.id == t)
                    return e.type;
        return AlterCtrl::TypeOther;
    }

    void updateEnablement()
    {
        const bool control = proc.getMode() == 1;
        moduleList.setEnabled (control);

        const auto selType = boundModuleType();
        for (auto& r : rows)
        {
            r.visible = control && (r.group == selType);
            if (r.label)  r.label->setVisible (r.visible);
            if (r.slider) { r.slider->setVisible (r.visible); r.slider->setEnabled (r.visible); }
            if (r.combo)  { r.combo->setVisible (r.visible);  r.combo->setEnabled (r.visible); }
            if (r.toggle) { r.toggle->setVisible (r.visible); r.toggle->setEnabled (r.visible); }
        }
        resized();
        repaint();
    }

    void rebuildModuleList()
    {
        const auto sel = proc.getTargetModule();
        entries = proc.getRegistry();
        moduleList.updateContent();

        // selection must always mirror the bound module (or nothing)
        int selRow = -1;
        for (int i = 0; i < (int) entries.size(); ++i)
            if (entries[(size_t) i].id == sel) { selRow = i; break; }

        if (selRow >= 0) moduleList.selectRow (selRow, juce::dontSendNotification);
        else             moduleList.deselectAllRows();

        updateEnablement();
    }

    // ===== Timer: poll for registry / mode changes =====
    void timerCallback() override
    {
        const int v = proc.getRegistryVersion();
        if (v != lastRegistryVersion)
        {
            lastRegistryVersion = v;
            rebuildModuleList();
        }

        if (proc.getMode() != lastMode)
        {
            lastMode = proc.getMode();
            updateEnablement();
        }

        if (proc.getTargetModule() != lastTarget)
        {
            lastTarget = proc.getTargetModule();
            rebuildModuleList();    // re-selects the bound row + refreshes knob visibility
        }

        // The CUSTOM theme keeps index 2 while its two source colours change, so
        // watching the index alone would leave the plugin stuck on whatever palette
        // it built the first time. Track the colours as well.
        if (proc.getThemeIndex() != lastTheme
            || proc.getThemeCustomPrimary()  .getARGB() != lastCustomPrimary
            || proc.getThemeCustomSecondary().getARGB() != lastCustomSecondary)
            applyTheme (proc.getThemeIndex());
    }

    // ===== ListBoxModel =====
    int getNumRows() override { return (int) entries.size(); }

    // A module is locked if a DIFFERENT live instance owns it.
    bool inUseByOther (const AlterListenerAudioProcessor::ModuleEntry& e) const
    {
        return e.ownerInstId != 0 && e.ownerInstId != proc.getInstanceId();
    }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= (int) entries.size()) return;
        const auto& e = entries[(size_t) row];
        const bool locked = inUseByOther (e);

        if (selected && ! locked)
            g.fillAll (juce::Colour (0xff3a3a48));

        // colour swatch (dimmed when locked)
        g.setColour (juce::Colour (e.colour).withAlpha (locked ? 0.35f : 1.0f));
        g.fillRect (juce::Rectangle<int> (6, height/2 - 6, 12, 12));

        if (locked)
        {
            // two lines: module name, then "in use on <track>" (so nothing is cut off)
            g.setColour (juce::Colours::grey);
            g.setFont (13.0f);
            g.drawText (e.name, 26, 2, width - 30, 17, juce::Justification::centredLeft, true);

            g.setColour (juce::Colours::grey.withAlpha (0.7f));
            g.setFont (11.0f);
            const juce::String owner = "in use on " + (e.ownerName.isNotEmpty() ? e.ownerName
                                                                                 : juce::String ("another track"));
            g.drawText (owner, 26, 19, width - 30, 16, juce::Justification::centredLeft, true);
        }
        else
        {
            g.setColour (juce::Colours::white);
            g.setFont (13.0f);
            g.drawText (e.name, 26, 0, width - 30, height, juce::Justification::centredLeft, true);
        }
    }

    void selectedRowsChanged (int lastRow) override
    {
        if (lastRow >= 0 && lastRow < (int) entries.size())
        {
            const auto& e = entries[(size_t) lastRow];
            if (inUseByOther (e))
            {
                rebuildModuleList();   // refuse: re-select the currently bound module
                return;
            }
            proc.setTargetModule (e.id);
        }
        updateEnablement();
    }

    // ── Double-click = "configure ALL" ────────────────────────────────────────
    // Ableton's Configure mode adds a parameter to the device panel when it is
    // touched. Double-clicking a module touches EVERY parameter of that module's
    // group, so one double-click maps the whole module — no per-knob clicking.
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (row < 0 || row >= (int) entries.size()) return;
        const auto& e = entries[(size_t) row];
        if (inUseByOther (e)) return;

        proc.setTargetModule (e.id);   // make sure the module is bound first
        touchAllParams (e.type);
    }

    void touchAllParams (juce::uint8 type)
    {
        for (auto pid : AlterCtrl::paramsForType (type))
        {
            if (auto* p = proc.getApvts().getParameter (AlterCtrl::idForParam (pid)))
            {
                const float v = p->getValue();   // normalised 0..1
                p->beginChangeGesture();
                // a tiny real change first — some hosts ignore same-value sets
                p->setValueNotifyingHost (v <= 0.5f ? juce::jmin (1.0f, v + 0.005f)
                                                    : juce::jmax (0.0f, v - 0.005f));
                p->setValueNotifyingHost (v);    // restore the exact value
                p->endChangeGesture();
            }
        }
    }

    AlterListenerAudioProcessor& proc;

    juce::Label   moduleLabel;
    juce::ListBox moduleList;
    std::vector<AlterListenerAudioProcessor::ModuleEntry> entries;

    juce::Viewport  paramView;     // scrolls the parameter rows
    juce::Component paramHolder;   // holds all rows; sized to total content height
    std::vector<Row> rows;

    int lastRegistryVersion = -1;
    int lastMode = -1;
    int lastTheme = -2;
    juce::uint32 lastCustomPrimary   = 0;
    juce::uint32 lastCustomSecondary = 0;
    juce::uint32 lastTarget = 0;

    // ===== Theme palette (mirrors AlterTheme in the ALTER app) =====
    struct Palette { juce::Colour bg, panel, raised, edge, textBright, textNormal, accent; };
    Palette pal { juce::Colour (0xff060A18), juce::Colour (0xff0A1126), juce::Colour (0xff101A38),
                  juce::Colour (0xff022D68), juce::Colour (0xffEAF3FF), juce::Colour (0xff96C6F2),
                  juce::Colour (0xff3D96E7) };

    // Mirrors AlterTheme::setTheme() in the ALTER app. The index mapping MUST match
    // it: 0 = Cyber, 1 = Dark, 2 = CUSTOM, 3 = White.
    //
    // Index 2 used to return a hardcoded "Solar" palette here — a theme the app does
    // not have. So picking the app's Custom theme switched the plugin to an unrelated
    // cream/green look instead of the user's colours, which is the second half of why
    // the custom theme "did nothing" in the plugin.
    static Palette paletteFor (int theme, juce::Colour customPrimary, juce::Colour customSecondary)
    {
        switch (theme)
        {
            case 1: // Dark
                return { juce::Colour (0xff0B0B0B), juce::Colour (0xff161616), juce::Colour (0xff222222),
                         juce::Colour (0xff333333), juce::Colour (0xffFFFFFF), juce::Colour (0xffC9C9C9),
                         juce::Colour (0xffEDEDED) };

            case 2: // CUSTOM — derived exactly as AlterTheme::setTheme(2) does
            {
                // Secondary drives the whole background family; primary drives the
                // accent AND the text hue, so changing the accent visibly recolours
                // the chrome instead of only tinting a few thin lines.
                const juce::Colour bg = customSecondary;
                const bool darkBg = bg.getPerceivedBrightness() < 0.5f;

                const float ah = customPrimary.getHue();
                const float as = juce::jlimit (0.0f, 1.0f, customPrimary.getSaturation());

                return {
                    bg.darker (0.12f),                                          // window background (bgDeep)
                    darkBg ? bg.brighter (0.14f) : bg.darker (0.06f),           // panels
                    darkBg ? bg.brighter (0.26f) : bg.darker (0.12f),           // raised
                    darkBg ? bg.brighter (0.42f) : bg.darker (0.22f),           // edges
                    darkBg ? juce::Colour (ah, as * 0.35f, 0.98f, 1.0f)
                           : juce::Colour (ah, as * 0.80f, 0.12f, 1.0f),        // textBright
                    darkBg ? juce::Colour (ah, as * 0.55f, 0.86f, 1.0f)
                           : juce::Colour (ah, as * 0.85f, 0.26f, 1.0f),        // textNormal
                    customPrimary                                               // accent
                };
            }

            case 3: // White
                return { juce::Colour (0xffF2F5F8), juce::Colour (0xffE9EEF3), juce::Colour (0xffDCE4EC),
                         juce::Colour (0xffB9C6D2), juce::Colour (0xff0B0F14), juce::Colour (0xff2A3440),
                         juce::Colour (0xff14181D) };
            default: // Cyber
                return { juce::Colour (0xff060A18), juce::Colour (0xff0A1126), juce::Colour (0xff101A38),
                         juce::Colour (0xff022D68), juce::Colour (0xffEAF3FF), juce::Colour (0xff96C6F2),
                         juce::Colour (0xff3D96E7) };
        }
    }

    void applyTheme (int theme)
    {
        pal = paletteFor (theme, proc.getThemeCustomPrimary(), proc.getThemeCustomSecondary());
        lnf.setPalette (pal.panel, pal.raised, pal.edge,
                        pal.textBright, pal.textNormal, pal.accent);

        moduleLabel.setColour (juce::Label::textColourId, pal.textBright);

        auto styleCombo = [this] (juce::ComboBox& c)
        {
            c.setColour (juce::ComboBox::backgroundColourId, pal.panel);
            c.setColour (juce::ComboBox::textColourId,       pal.textNormal);
            c.setColour (juce::ComboBox::outlineColourId,    pal.edge);
            c.setColour (juce::ComboBox::arrowColourId,      pal.accent);
        };

        moduleList.setColour (juce::ListBox::backgroundColourId, pal.panel);
        moduleList.setColour (juce::ListBox::outlineColourId,    pal.edge);

        for (auto& r : rows)
        {
            if (r.label) r.label->setColour (juce::Label::textColourId, pal.textNormal);
            if (r.slider)
            {
                r.slider->setColour (juce::Slider::trackColourId,           pal.accent.withAlpha (0.7f));
                r.slider->setColour (juce::Slider::thumbColourId,           pal.accent);
                r.slider->setColour (juce::Slider::backgroundColourId,      pal.raised);
                r.slider->setColour (juce::Slider::textBoxTextColourId,     pal.textNormal);
                r.slider->setColour (juce::Slider::textBoxBackgroundColourId, pal.panel);
                r.slider->setColour (juce::Slider::textBoxOutlineColourId,  pal.edge);
            }
            if (r.combo) styleCombo (*r.combo);
            if (r.toggle)
            {
                r.toggle->setColour (juce::ToggleButton::tickColourId,         pal.accent);
                r.toggle->setColour (juce::ToggleButton::tickDisabledColourId, pal.edge);
            }
        }

        lastTheme           = theme;
        lastCustomPrimary   = proc.getThemeCustomPrimary()  .getARGB();
        lastCustomSecondary = proc.getThemeCustomSecondary().getARGB();
        sendLookAndFeelChange();
        repaint();
    }

    CreatorLookAndFeel lnf;   // futuristic widget styling (declared before use)

    // Nothing in this editor was discoverable by looking at it: that the number
    // is editable, or that a double-click restores the default. One tooltip
    // window makes the per-slider hints actually show up.
    juce::TooltipWindow tooltips { this, 700 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AlterListenerEditor)
};

juce::AudioProcessorEditor* AlterListenerAudioProcessor::createEditor()
{
   #ifdef ALTER_PURE_LISTENER
    // Pure listener has no editor (hasEditor() == false); a host that ignores that
    // gets a harmless generic editor that can't touch missing parameters.
    return new juce::GenericAudioProcessorEditor (*this);
   #else
    return new AlterListenerEditor (*this);
   #endif
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AlterListenerAudioProcessor();
}
