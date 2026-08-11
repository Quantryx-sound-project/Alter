#pragma once
#include <JuceHeader.h>
#include "TruePeakMeter.h"
#include "ConstantQ.h"
#include <atomic>
#include <cstring>
#include <array>
#include <map>
#include <vector>
#include <functional>

// ALTER Listener - VST3
//
// Two modes (chosen in the plugin editor / "mode" parameter):
//   0 = Listener  : behaves exactly like before – sends audio analysis data
//                   (RMS/peak/LUFS/FFT/waveform) to ALTER over UDP.
//   1 = Control   : ALSO sends audio data (still a sender), PLUS exposes
//                   automatable parameters that drive a chosen ALTER visual
//                   module. Lets you automate visual params from the DAW
//                   (e.g. Ableton) on the track the plugin sits on.
//
// Protocol v2 (multi-instance):
//   Every packet:  'A','L','T','2'  +  uint8 type  +  uint32 instanceId  +  payload
//
//   Plugin -> ALTER (UDP port 7000):
//     'R' : float                      RMS (~200 Hz)
//     'P' : float                      True peak (linear)
//     'L' : float                      Momentary LUFS (ITU-R BS.1770, 400 ms)
//     'F' : N floats in [0..1]         FFT magnitudes (N = binsOut, 2048..8192)
//     'W' : interleaved L,R floats     Stereo waveform (raw samples)
//     'I' : UTF-8 string               Instance announce (name), sent ~2x/s
//     'O' : uint8 mode                 Mode announce (0=Listener, 1=Control)  [was 'M']
//     'M' : uint8 count + count×(note,vel)   Held MIDI notes ("ALTM"; sent only while MIDI present)
//   ALTER -> plugin control: 'N' (data-needs mask), 'B' (FFT order 12..14)
//     'B' : uint32 targetModuleId      Bind: which ALTER module this plugin drives
//     'C' : uint32 moduleId + uint8 paramId + float value
//                                      Control: set a visual param (real value)
//
//   ALTER -> Plugin (replied to the plugin's source address on the same socket):
//     'G' : module registry            uint8 count, then per module:
//                                        uint32 id, uint32 colourARGB,
//                                        uint8 type, uint8 nameLen, name[nameLen]
//     'V' : value sync                 uint32 moduleId, uint8 count, then per param:
//                                        uint8 paramId, float value (real)
//
// The legacy v1 protocol ('ALTR'/'ALTF'/...) is still understood by the
// receiver in ALTER, so old plugin builds keep working (shown as "Legacy").

static constexpr int kAlterHeaderSize = 9; // 4 magic + 1 type + 4 instanceId

// ---------------------------------------------------------------
// Shared control-parameter ids (MUST match Alter/Source/UdpReceiver.h)
// ---------------------------------------------------------------
namespace AlterCtrl
{
    enum ParamId : uint8_t
    {
        None        = 0,
        // Synesthesia (module type 0)
        SynSmooth   = 1,
        SynZoom     = 2,
        SynRotation = 3,
        SynSymmetry = 4,
        SynSaturation = 5,
        SynBloom    = 6,
        SynSpeed    = 7,
        // Chladni patterns (module type 1)
        ChShift     = 8,
        ChAspect    = 9,
        ChSandSmooth = 10,
        ChParticles = 11,
        ChMaterial  = 12,
        // Geometry (module type 2)
        GeoSmooth     = 13,
        GeoZoom       = 14,
        GeoRotation   = 15,
        GeoSymmetry   = 16,
        GeoSaturation = 17,
        GeoBloom      = 18,
        GeoSpeed      = 19,
        GeoTri        = 20,
        GeoSquare     = 21,
        GeoCircle     = 22,
        GeoComplexity = 23,
        GeoRandom     = 24,
        GeoReact      = 25,
        GeoTone       = 26,
        GeoDepth      = 27,
        SynReact      = 28,
        // ── new controller params (Synesthesia) ──
        SynFragment   = 29,
        SynTransmute  = 30,
        SynMirror     = 31,
        SynClear      = 32,
        SynDenoise    = 33,
        SynCurveSmooth = 34,
        SynBpmSync    = 35,
        SynBpm        = 36,
        SynBeatDiv    = 37,
        SynTone       = 38,
        // ── new controller params (Geometry) ──
        GeoTunnel     = 39,
        GeoAperture   = 40,
        GeoGlobalRot  = 41,
        GeoBpmSync    = 42,
        GeoBpm        = 43,
        GeoBeatDiv    = 44,
        // ── new controller params (Chladni) ──
        ChReactive    = 45,
        ChM           = 46,
        ChN           = 47,
        ChTone        = 48,
        SynTunnel     = 49,
        SynVortex     = 50,
        SynBrightness = 51,
        GeoBrightness = 52,
        // Mirror fold — the ALTER controller shows the SAME toggle for Geometry,
        // but it used to be reachable only through SynMirror (synesthesia-only),
        // so it was not automatable on a Geometry module. Own id now.
        GeoMirror     = 53,
        // ── base colour (the controller's "Color..." picker), as H/S/B so it can
        //    actually be automated on a DAW timeline ──
        SynColorHue   = 54,
        SynColorSat   = 55,
        SynColorBri   = 56,
        ChColorHue    = 57,
        ChColorSat    = 58,
        ChColorBri    = 59,
        GeoColorHue   = 60,
        GeoColorSat   = 61,
        GeoColorBri   = 62,
        // ── Chladni "Preset" combo (classic (m,n) figures) ──
        ChPreset      = 63,

        LastParam     = ChPreset   // highest id (listener registration loops 1..LastParam)
    };

    /** Beat divisions behind the "Beat div" combo, for BOTH Synesthesia and
        Geometry. Index -> label; the index is what travels on the wire.
        MUST match GeometryVisual::labelForDivision / beatsForDivision in ALTER,
        and VisualSynesthesia::beatsForDiv, which all use this same order. */
    inline const char* beatDivName (int idx) noexcept
    {
        static const char* names[12] =
        { "4/1", "2/1", "1/1", "1/2", "1/2T", "1/4", "1/4T",
          "1/8", "1/8T", "1/16", "1/16T", "1/32" };
        return names[idx < 0 ? 0 : (idx > 11 ? 11 : idx)];
    }

    inline juce::StringArray beatDivNames()
    {
        juce::StringArray a;
        for (int i = 0; i < 12; ++i)
            a.add (beatDivName (i));
        return a;
    }

    /** Classic Chladni figures behind the controller's Preset combo.
        Index 0 = "Custom" (no preset), 1..12 = the pairs below.
        MUST match the table in Alter/Source/ControllerWindow.cpp. */
    inline constexpr int kChladniPresets[12][2] =
        { {1,2},{2,3},{1,4},{2,5},{3,4},{3,5},{4,5},{2,7},{3,7},{5,6},{4,9},{6,7} };

    /** 0 = Custom, else the 1-based preset index for an (m,n) pair. */
    inline int chladniPresetIndex (int m, int n) noexcept
    {
        for (int i = 0; i < 12; ++i)
            if (kChladniPresets[i][0] == m && kChladniPresets[i][1] == n)
                return i + 1;
        return 0;
    }

    // module type codes used in the 'G' registry
    enum ModuleType : uint8_t { TypeSynesthesia = 0, TypeChladni = 1, TypeGeometry = 2, TypeOther = 255 };

    // APVTS string id <-> numeric ParamId
    inline const char* idForParam (ParamId p) noexcept
    {
        switch (p)
        {
            case SynSmooth:     return "synSmooth";
            case SynZoom:       return "synZoom";
            case SynRotation:   return "synRotation";
            case SynSymmetry:   return "synSymmetry";
            case SynSaturation: return "synSaturation";
            case SynBloom:      return "synBloom";
            case SynSpeed:      return "synSpeed";
            case ChShift:       return "chShift";
            case ChAspect:      return "chAspect";
            case ChSandSmooth:  return "chSandSmooth";
            case ChParticles:   return "chParticles";
            case ChMaterial:    return "chMaterial";
            case GeoSmooth:     return "geoSmooth";
            case GeoZoom:       return "geoZoom";
            case GeoRotation:   return "geoRotation";
            case GeoSymmetry:   return "geoSymmetry";
            case GeoSaturation: return "geoSaturation";
            case GeoBloom:      return "geoBloom";
            case GeoSpeed:      return "geoSpeed";
            case GeoTri:        return "geoTri";
            case GeoSquare:     return "geoSquare";
            case GeoCircle:     return "geoCircle";
            case GeoComplexity: return "geoComplexity";
            case GeoRandom:     return "geoRandom";
            case GeoReact:      return "geoReact";
            case GeoTone:       return "geoTone";
            case GeoDepth:      return "geoDepth";
            case SynReact:      return "synReact";
            case SynFragment:   return "synFragment";
            case SynTransmute:  return "synTransmute";
            case SynMirror:     return "synMirror";
            case SynClear:      return "synClear";
            case SynDenoise:    return "synDenoise";
            case SynCurveSmooth:return "synCurveSmooth";
            case SynBpmSync:    return "synBpmSync";
            case SynBpm:        return "synBpm";
            case SynBeatDiv:    return "synBeatDiv";
            case SynTone:       return "synTone";
            case GeoTunnel:     return "geoTunnel";
            case GeoAperture:   return "geoAperture";
            case GeoGlobalRot:  return "geoGlobalRot";
            case GeoBpmSync:    return "geoBpmSync";
            case GeoBpm:        return "geoBpm";
            case GeoBeatDiv:    return "geoBeatDiv";
            case ChReactive:    return "chReactive";
            case ChM:           return "chM";
            case ChN:           return "chN";
            case ChTone:        return "chTone";
            case SynTunnel:     return "synTunnel";
            case SynVortex:     return "synVortex";
            case SynBrightness: return "synBrightness";
            case GeoBrightness: return "geoBrightness";
            case GeoMirror:     return "geoMirror";
            case SynColorHue:   return "synColHue";
            case SynColorSat:   return "synColSat";
            case SynColorBri:   return "synColBri";
            case ChColorHue:    return "chColHue";
            case ChColorSat:    return "chColSat";
            case ChColorBri:    return "chColBri";
            case GeoColorHue:   return "geoColHue";
            case GeoColorSat:   return "geoColSat";
            case GeoColorBri:   return "geoColBri";
            case ChPreset:      return "chPreset";
            default:            return "";
        }
    }

    inline ParamId paramForId (const juce::String& id) noexcept
    {
        if (id == "synSmooth")     return SynSmooth;
        if (id == "synZoom")       return SynZoom;
        if (id == "synRotation")   return SynRotation;
        if (id == "synSymmetry")   return SynSymmetry;
        if (id == "synSaturation") return SynSaturation;
        if (id == "synBloom")      return SynBloom;
        if (id == "synSpeed")      return SynSpeed;
        if (id == "chShift")       return ChShift;
        if (id == "chAspect")      return ChAspect;
        if (id == "chSandSmooth")  return ChSandSmooth;
        if (id == "chParticles")   return ChParticles;
        if (id == "chMaterial")    return ChMaterial;
        if (id == "geoSmooth")     return GeoSmooth;
        if (id == "geoZoom")       return GeoZoom;
        if (id == "geoRotation")   return GeoRotation;
        if (id == "geoSymmetry")   return GeoSymmetry;
        if (id == "geoSaturation") return GeoSaturation;
        if (id == "geoBloom")      return GeoBloom;
        if (id == "geoSpeed")      return GeoSpeed;
        if (id == "geoTri")        return GeoTri;
        if (id == "geoSquare")     return GeoSquare;
        if (id == "geoCircle")     return GeoCircle;
        if (id == "geoComplexity") return GeoComplexity;
        if (id == "geoRandom")     return GeoRandom;
        if (id == "geoReact")      return GeoReact;
        if (id == "geoTone")       return GeoTone;
        if (id == "geoDepth")      return GeoDepth;
        if (id == "synReact")      return SynReact;
        if (id == "synFragment")   return SynFragment;
        if (id == "synTransmute")  return SynTransmute;
        if (id == "synMirror")     return SynMirror;
        if (id == "synClear")      return SynClear;
        if (id == "synDenoise")    return SynDenoise;
        if (id == "synCurveSmooth")return SynCurveSmooth;
        if (id == "synBpmSync")    return SynBpmSync;
        if (id == "synBpm")        return SynBpm;
        if (id == "synBeatDiv")    return SynBeatDiv;
        if (id == "synTone")       return SynTone;
        if (id == "geoTunnel")     return GeoTunnel;
        if (id == "geoAperture")   return GeoAperture;
        if (id == "geoGlobalRot")  return GeoGlobalRot;
        if (id == "geoBpmSync")    return GeoBpmSync;
        if (id == "geoBpm")        return GeoBpm;
        if (id == "geoBeatDiv")    return GeoBeatDiv;
        if (id == "chReactive")    return ChReactive;
        if (id == "chM")           return ChM;
        if (id == "chN")           return ChN;
        if (id == "chTone")        return ChTone;
        if (id == "synTunnel")     return SynTunnel;
        if (id == "synVortex")     return SynVortex;
        if (id == "synBrightness") return SynBrightness;
        if (id == "geoBrightness") return GeoBrightness;
        if (id == "geoMirror")     return GeoMirror;
        if (id == "synColHue")     return SynColorHue;
        if (id == "synColSat")     return SynColorSat;
        if (id == "synColBri")     return SynColorBri;
        if (id == "chColHue")      return ChColorHue;
        if (id == "chColSat")      return ChColorSat;
        if (id == "chColBri")      return ChColorBri;
        if (id == "geoColHue")     return GeoColorHue;
        if (id == "geoColSat")     return GeoColorSat;
        if (id == "geoColBri")     return GeoColorBri;
        if (id == "chPreset")      return ChPreset;
        return None;
    }

    /** All automatable params belonging to one module type (for "configure all").
        The order mirrors the ALTER controller editor top-to-bottom, so Ableton's
        Configure panel comes out in the same order as the app. */
    inline std::vector<ParamId> paramsForType (uint8_t type)
    {
        switch (type)
        {
            case TypeSynesthesia:
                return { SynCurveSmooth, SynZoom, SynRotation, SynSymmetry, SynMirror,
                         SynSaturation, SynBrightness, SynBloom, SynSpeed,
                         SynBpmSync, SynBpm, SynBeatDiv,
                         SynFragment, SynTransmute, SynClear, SynDenoise,
                         SynTunnel, SynVortex, SynReact, SynTone, SynSmooth,
                         SynColorHue, SynColorSat, SynColorBri };
            case TypeChladni:
                return { ChReactive, ChPreset, ChM, ChN, ChShift, ChAspect, ChSandSmooth,
                         ChParticles, ChMaterial, ChTone,
                         ChColorHue, ChColorSat, ChColorBri };
            case TypeGeometry:
                return { GeoSpeed, GeoBpmSync, GeoBpm, GeoBeatDiv,
                         GeoComplexity, GeoRandom, GeoReact,
                         GeoTri, GeoSquare, GeoCircle,
                         GeoSymmetry, GeoMirror, GeoZoom,
                         GeoTunnel, GeoDepth, GeoAperture,
                         GeoRotation, GeoGlobalRot,
                         GeoSaturation, GeoBrightness, GeoBloom,
                         GeoTone, GeoSmooth,
                         GeoColorHue, GeoColorSat, GeoColorBri };
            default: return {};
        }
    }
}

// ---------------------------------------------------------------
// Packet struktura pre ring buffer
// ---------------------------------------------------------------
struct Packet
{
    static constexpr int kMaxSize = kAlterHeaderSize + 8192 * (int) sizeof (float);
    uint8_t data[kMaxSize];
    int     size = 0;
};

// ---------------------------------------------------------------
// Lock-free ring buffer pre packety
// Pise audio vlakno (a UI vlakno pre control packety), cita sender thread
// ---------------------------------------------------------------
class PacketRingBuffer
{
public:
    static constexpr int kCapacity = 128;

    bool push (const uint8_t* src, int len)
    {
        const int w = writePos.load (std::memory_order_relaxed);
        const int next = (w + 1) % kCapacity;
        if (next == readPos.load (std::memory_order_acquire))
            return false; // buffer plny, zahodime packet

        auto& slot = slots[w];
        std::memcpy (slot.data, src, (size_t) len);
        slot.size = len;
        writePos.store (next, std::memory_order_release);
        return true;
    }

    bool pop (Packet& out)
    {
        const int r = readPos.load (std::memory_order_relaxed);
        if (r == writePos.load (std::memory_order_acquire))
            return false; // buffer prazdny

        out = slots[r];
        readPos.store ((r + 1) % kCapacity, std::memory_order_release);
        return true;
    }

private:
    Packet slots[kCapacity];
    std::atomic<int> writePos { 0 };
    std::atomic<int> readPos  { 0 };
};

class AlterListenerAudioProcessor : public juce::AudioProcessor,
                                    private juce::AudioProcessorValueTreeState::Listener
{
public:
    AlterListenerAudioProcessor();
    ~AlterListenerAudioProcessor() override;

    const juce::String getName() const override                      { return "ALTER Listener"; }
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override                                  {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    { juce::ignoreUnused (layouts); return true; }

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
   #if JUCE_AUDIOPROCESSOR_HAS_PROCESSBLOCK_DOUBLE
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override { }
   #endif

   #ifdef ALTER_PURE_LISTENER
    bool hasEditor() const override                                   { return false; }  // pure listener: no editor
   #else
    bool hasEditor() const override                                   { return true; }
   #endif
    juce::AudioProcessorEditor* createEditor() override;

    bool acceptsMidi() const override                                  { return true; }   // MIDI capture → 'M' packets
    bool producesMidi() const override                                 { return false; }
    bool isMidiEffect() const override                                 { return false; }
    double getTailLengthSeconds() const override                        { return 0.0; }
    bool supportsDoublePrecisionProcessing() const override             { return false; }
   #if JUCE_MAJOR_VERSION >= 7
    juce::AudioProcessorParameter* getBypassParameter() const override { return nullptr; }
   #endif

    int getNumPrograms() override                                       { return 1; }
    int getCurrentProgram() override                                    { return 0; }
    void setCurrentProgram (int) override                               {}
    const juce::String getProgramName (int) override                    { return {}; }
    void changeProgramName (int, const juce::String&) override          {}

    void getStateInformation (juce::MemoryBlock& dest) override;
    void setStateInformation (const void* data, int size) override;

    // ===== Public API used by the editor =====
    juce::AudioProcessorValueTreeState& getApvts() noexcept { return apvts; }

    int  getMode() const noexcept { return mode.load(); }

    juce::uint32 getTargetModule() const noexcept { return targetModuleId.load(); }
    void setTargetModule (juce::uint32 id);

    // A snapshot of the ALTER module registry received over UDP.
    struct ModuleEntry
    {
        juce::uint32 id     = 0;
        juce::uint8  type   = AlterCtrl::TypeOther;
        juce::uint32 colour = 0xff808080;
        juce::String name;
        juce::uint32 ownerInstId = 0;   // 0 = free; else the instance that owns this module
        juce::String ownerName;         // owner's track/name (for "in use on ...")
    };
    std::vector<ModuleEntry> getRegistry() const
    {
        const juce::ScopedLock sl (registryLock);
        return registry;
    }
    int getRegistryVersion() const noexcept { return registryVersion.load(); }

    // ALTER theme index (0=Cyber,1=Dark,2=Solar,3=White) for the editor look.
    int getThemeIndex() const noexcept { return themeIndex.load(); }
    juce::Colour getThemeCustomPrimary()   const noexcept { return juce::Colour (themeCustomPrimary.load()); }
    juce::Colour getThemeCustomSecondary() const noexcept { return juce::Colour (themeCustomSecondary.load()); }

    juce::uint32 getInstanceId() const noexcept { return instanceId; }

private:
    // ===== APVTS / parameters =====
    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void parameterChanged (const juce::String& paramID, float newValue) override;

    std::atomic<int>          mode { 0 };               // 0=Listener, 1=Control
    std::atomic<juce::uint32> targetModuleId { 0 };     // bound ALTER module id (0=none)
    // Data-needs mask from ALTER ('N' packet): R=1,P=2,L=4,F=8,W=16. Default = all
    // (send everything) until ALTER tells us what it actually consumes.
    std::atomic<juce::uint8>  dataNeeds { 0x1F };
    std::atomic<bool>         suppressOutgoing { false }; // guard against echo loops

    // Active param group for dynamic show/hide: 0=Synesthesia, 1=Chladni, 255=both.
    // Irrelevant params become non-automatable so hosts can drop them from the
    // automation list (Ableton may need a rescan / may ignore this).
    std::atomic<int>          activeGroup { 255 };
    void updateActiveGroup();

    // ===== Incoming packet handling (called from sender thread) =====
    void handleIncomingPacket (const uint8_t* data, int size);
    void applyValueSync (juce::uint32 moduleId,
                         const std::vector<std::pair<uint8_t, float>>& values);

    mutable juce::CriticalSection registryLock;
    std::vector<ModuleEntry>      registry;
    std::atomic<int>              registryVersion { 0 };
    std::atomic<int>              themeIndex { 0 };
    // Theme index 2 is CUSTOM, and every colour in it is derived from this pair —
    // the index alone cannot describe it. ALTER sends them in the 'G' trailer.
    std::atomic<juce::uint32>     themeCustomPrimary   { 0xFF3D96E7 };
    std::atomic<juce::uint32>     themeCustomSecondary { 0xFF6902D6 };

    // ===== Outgoing control packets =====
    void sendModePacket (int m);
    void sendBindPacket (juce::uint32 moduleId);
    void sendControlPacket (juce::uint32 moduleId, uint8_t paramId, float value);

    // ---------------------------------------------------------------
    // Automation coalescing
    // ---------------------------------------------------------------
    // parameterChanged() is called ONCE PER AUDIO BLOCK for every parameter the
    // host is automating — about 94 times a second at a 512-sample block, and
    // multiplied by however many lanes are moving. Sending a packet for each of
    // those put ALTER's message thread through a full state-tree update per
    // block, which is what made an automated Bloom or Zoom stutter the whole HUD.
    //
    // Nothing downstream can USE more than one value per displayed frame, so the
    // audio thread now only parks the latest value per parameter and the sender
    // thread emits them at ~60 Hz. This drops nothing: the final value of a
    // gesture is by definition the last one written, so it is always the one that
    // goes out.
    struct PendingCtrl
    {
        std::atomic<float> value { 0.0f };
        std::atomic<bool>  dirty { false };
    };
    PendingCtrl pendingCtrl[AlterCtrl::LastParam + 1];

    /** AUDIO THREAD (lock-free, allocation-free). */
    void queueControlValue (uint8_t paramId, float value) noexcept
    {
        if (paramId == AlterCtrl::None || paramId > AlterCtrl::LastParam)
            return;
        pendingCtrl[paramId].value.store (value, std::memory_order_relaxed);
        pendingCtrl[paramId].dirty.store (true, std::memory_order_release);
    }

    /** SENDER THREAD. Emit one packet per parameter that moved since last time. */
    void flushPendingControlValues()
    {
        if (mode.load() != 1) return;
        const auto target = targetModuleId.load();
        if (target == 0) return;

        for (int pid = 1; pid <= (int) AlterCtrl::LastParam; ++pid)
        {
            if (! pendingCtrl[pid].dirty.exchange (false, std::memory_order_acquire))
                continue;
            sendControlPacket (target, (uint8_t) pid, pendingCtrl[pid].value.load (std::memory_order_relaxed));
        }
    }

    /** Drop anything still queued — used when the bound module changes, so values
        meant for the old module cannot land on the new one. */
    void discardPendingControlValues() noexcept
    {
        for (auto& s : pendingCtrl)
            s.dirty.store (false, std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------
    // Sender thread - cita z ring bufferu a posiela UDP packety.
    // Navyse posiela 'I' announce + 'O' mode, a CITA odpovede z ALTERa
    // (registry 'G' a value-sync 'V') na tom istom sockete.
    // ---------------------------------------------------------------
    class SenderThread : public juce::Thread
    {
    public:
        SenderThread (PacketRingBuffer& rb, juce::DatagramSocket& sock, int port,
                      juce::uint32 instId, const juce::String& instName,
                      std::function<void (const uint8_t*, int)> onReceiveFn)
            : juce::Thread ("ALTER Sender"),
              ringBuffer (rb), socket (sock), targetPort (port),
              onReceive (std::move (onReceiveFn))
        {
            setIdentity (instId, instName);
            startThread (juce::Thread::Priority::low);
        }

        ~SenderThread() override { stopThread (500); }

        void setIdentity (juce::uint32 instId, const juce::String& instName)
        {
            const juce::ScopedLock sl (identityLock);
            id = instId;
            nameUtf8 = instName.toRawUTF8();
        }

    private:
        void run() override
        {
            Packet p;
            juce::uint32 lastAnnounceMs = 0;

            while (! threadShouldExit())
            {
                bool didWork = false;

                while (ringBuffer.pop (p))
                {
                    socket.write ("127.0.0.1", targetPort, p.data, p.size);
                    didWork = true;
                    if (threadShouldExit()) return;
                }

                // read replies from ALTER (non-blocking poll)
                if (socket.waitUntilReady (true, 0) > 0)
                {
                    uint8_t rbuf[4096];
                    const int rn = socket.read (rbuf, (int) sizeof (rbuf), false);
                    if (rn > 0 && onReceive)
                        onReceive (rbuf, rn);
                    didWork = true;
                }

                const auto now = juce::Time::getMillisecondCounter();
                if (now - lastAnnounceMs >= 500)
                {
                    lastAnnounceMs = now;
                    sendAnnounce();
                }

                const bool live = hooksLive.load (std::memory_order_acquire);

                // Coalesced automation: at most one packet per parameter per ~60 Hz
                // tick, however fast the host is driving the audio thread.
                if (live && flushControls && now - lastCtrlFlushMs >= 16)
                {
                    lastCtrlFlushMs = now;
                    flushControls();
                }

                // Constant-Q on the worker thread (~30 Hz, on demand) — off audio thread.
                if (live && buildCqt && cqtWanted && cqtWanted() && now - lastCqtMs >= 33)
                {
                    lastCqtMs = now;
                    uint8_t buf[kAlterHeaderSize + 300 * (int) sizeof (float)];
                    const int sz = buildCqt (buf);
                    if (sz > 0) { socket.write ("127.0.0.1", targetPort, buf, sz); didWork = true; }
                }

                if (! didWork)
                    juce::Thread::sleep (1);
            }
        }

    public:
        std::function<int (uint8_t*)> buildCqt;       // fills a 'Q' packet, returns size
        std::function<bool()>         cqtWanted;      // true while CQT is needed
        std::function<void()>         flushControls;  // drains the coalesced automation
        juce::uint32                  lastCqtMs = 0;
        juce::uint32                  lastCtrlFlushMs = 0;

        /** Call once the three callbacks above have been assigned.

            The thread starts inside this class's constructor, so it is already
            running while the owner is still wiring the callbacks up — reading a
            std::function that is concurrently being assigned is a data race, and a
            torn read means calling through a half-written target. Publishing them
            behind one release store closes that window for all three at once. */
        void hooksReady() noexcept { hooksLive.store (true, std::memory_order_release); }

    private:
        void sendAnnounce()
        {
            uint8_t buf[kAlterHeaderSize + 64];
            juce::uint32 instId;
            int nameLen;
            {
                const juce::ScopedLock sl (identityLock);
                instId  = id;
                nameLen = juce::jmin ((int) nameUtf8.length(), 64);
                std::memcpy (buf + kAlterHeaderSize, nameUtf8.c_str(), (size_t) nameLen);
            }

            buf[0]='A'; buf[1]='L'; buf[2]='T'; buf[3]='2';
            buf[4]=(uint8_t) 'I';
            std::memcpy (buf + 5, &instId, sizeof (instId));

            socket.write ("127.0.0.1", targetPort, buf, kAlterHeaderSize + nameLen);
        }

        PacketRingBuffer&     ringBuffer;
        juce::DatagramSocket& socket;
        const int             targetPort;
        std::function<void (const uint8_t*, int)> onReceive;

        juce::CriticalSection identityLock;
        juce::uint32          id = 0;
        std::string           nameUtf8;
        std::atomic<bool>     hooksLive { false };
    };

    // UDP
    std::unique_ptr<juce::DatagramSocket> socket;
    PacketRingBuffer                      ringBuffer;
    std::unique_ptr<SenderThread>         senderThread;
    static constexpr int kPort = 7000;

    // ---------------------------------------------------------------
    // Instance identity
    // ---------------------------------------------------------------
    juce::uint32 instanceId   = 0;
    juce::String instanceName;
    juce::String trackName;

public:
    void updateTrackProperties (const TrackProperties& props) override
    {
        const juce::String newTrack = unwrapName (props.name);
        if (newTrack.isNotEmpty() && newTrack != trackName)
        {
            trackName = newTrack;
            rebuildInstanceName();
            if (senderThread != nullptr)
                senderThread->setIdentity (instanceId, instanceName);
        }
    }

private:
    static juce::String unwrapName (const juce::String& s) { return s; }
    template <typename Opt>
    static juce::String unwrapName (const Opt& s) { return s.has_value() ? *s : juce::String(); }
    void rebuildInstanceName()
    {
        static juce::CriticalSection countLock;
        static std::map<juce::String, int> perTrackCount;

        const juce::String track = trackName.isNotEmpty() ? trackName : "Track";

        if (claimedTrack != track)
        {
            const juce::ScopedLock sl (countLock);
            claimedTrack   = track;
            claimedOrdinal = ++perTrackCount[track];
        }

        instanceName = "Creator " + track + " " + juce::String (claimedOrdinal);
    }

    juce::String claimedTrack;
    int          claimedOrdinal = 0;

    // Header helper: writes 'ALT2' + type + instanceId, returns header size
    int writeHeader (uint8_t* dst, uint8_t type) const noexcept
    {
        dst[0]='A'; dst[1]='L'; dst[2]='T'; dst[3]='2';
        dst[4]=type;
        std::memcpy (dst + 5, &instanceId, sizeof (instanceId));
        return kAlterHeaderSize;
    }

    // Enqueue funkcie (volane z audio vlakna)
    void enqueueFloatPacket    (uint8_t type, float value);
    void enqueueWaveformPacket ();

    // RMS
    int    samplesPerPacket = 0;
    int    sampleCounter    = 0;
    float  rmsSmooth        = 0.0f;
    double sampleRateHz     = 48000.0;

    // True Peak (ITU-R BS.1770: 4x oversampled, windowed-sinc polyphase FIR)
    float blockPeak = 0.0f;
    TruePeakMeter truePeak;

    // ---- MIDI capture ('M' packets, "ALTM") ----
    // Held-note state, audio-thread only. Sent on every change + 100 ms
    // heartbeat while notes are held; nothing is sent when the track has no
    // MIDI (ALTER then falls back to FFT detection).
    std::array<juce::uint8, 128> midiVel {};   // 0 = not held
    int          midiActiveCount = 0;
    juce::uint32 lastMidiSendMs  = 0;
    void handleMidiAndEnqueue (const juce::MidiBuffer& midi);   // audio thread

    // Constant-Q (multi-resolution) — computed/sent only when ALTER needs it (bit 32).
    ConstantQAnalyzer cqt;
    std::vector<float> cqtScratch;
    int buildCqtPacket (uint8_t* out);   // worker thread

    // K-weighting biquad (ITU-R BS.1770)
    struct Biquad
    {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;
        void reset() { z1 = z2 = 0.0f; }
        float process (float x)
        {
            float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    Biquad kWeightStage1[2], kWeightStage2[2];

    // LUFS momentary (400 ms sliding window)
    std::vector<float> lufsBuffer;
    int    lufsBufferSize  = 0;
    int    lufsWritePos    = 0;
    double lufsRunningSum  = 0.0;

    void computeKWeightCoeffs (double sampleRate);

    // FFT — runtime-configurable size. ALTER picks the resolution globally and
    // pushes it via a 'B' control packet; the plugin then computes/sends only that
    // many bins (lower setting = less CPU + smaller UDP packets on weak machines).
    static constexpr int kFftOrderMin = 10;            // 1024 → 512 bins (weak machines)
    static constexpr int kFftOrderMax = 14;            // 16384 → 8192 bins (fits one UDP datagram)
    static constexpr int kMaxFftSize  = 1 << kFftOrderMax;
    static constexpr int kMaxBinsOut  = kMaxFftSize / 2;

    int fftOrder = kFftOrderMin;
    int fftSize  = 1 << kFftOrderMin;
    int binsOut  = fftSize / 2;

    std::atomic<int> requestedFftOrder { kFftOrderMin };   // set on 'B' control packet
    void applyFftOrderIfChanged();                          // audio thread

    juce::dsp::FFT fft { kFftOrderMin };
    juce::HeapBlock<juce::dsp::Complex<float>> fftIn, fftOut;
    juce::AudioBuffer<float> fifo;
    int fifoWrite      = 0;
    int hopSamples     = (1 << kFftOrderMin) / 4;
    int hopAccumulator = 0;
    int transportAccumulator = 0;   // 'T' packet throttle (~23/s @ 48k)
    // Sync-impulse ('S') edge detection: we fire ONE impulse when the DAW transport
    // (re)starts — playback begins, or the position jumps backwards (loop/relocate).
    // ALTER re-anchors its beat phase to that impulse; BPM & division stay manual.
    bool   prevPlaying = false;
    double prevPpq     = -1.0;
    // Last tempo the host actually reported. FL Studio and Pro Tools return no
    // BPM while the transport is stopped, and Reaper drops it during a bounce —
    // sending the 0.0 fallback makes ALTER's BPM-synced visuals stall until play
    // is pressed again. Hold the last good value instead.
    float  lastGoodBpm = 0.0f;

    static constexpr float kDbFloor = -90.0f;

    // Stereo waveform ring buffer
    static constexpr int kWaveformSize = 4096;
    std::array<float, kWaveformSize> waveformL {};
    std::array<float, kWaveformSize> waveformR {};
    int waveformWrite = 0;

    void pushSamplesToFifo (const float* samples, int numSamples);
    void performFftAndEnqueue();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AlterListenerAudioProcessor)
};