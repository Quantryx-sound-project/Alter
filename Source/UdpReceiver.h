#pragma once
#include <JuceHeader.h>
#include "AudioSourceInterface.h"
#include <vector>
#include <map>
#include <atomic>
#include <cstring>
#include <cmath>
#include <functional>
#include <utility>

// Info o jednej bezuacej instancii ALTER Listener pluginu
struct PluginInstanceInfo
{
    juce::uint32 id = 0;
    juce::String name;
    bool         active = false;   // packet prijaty v poslednych ~2.5 s
};

// ─────────────────────────────────────────────────────────────────────────────
// Control channel (ALTER -> plugin) shared definitions
// Param ids MUST match AlterListener/Source/PluginProcessor.h (namespace AlterCtrl)
// ─────────────────────────────────────────────────────────────────────────────
namespace AlterCtrl
{
    enum ParamId : juce::uint8
    {
        None        = 0,
        SynSmooth   = 1, SynZoom = 2, SynRotation = 3, SynSymmetry = 4,
        SynSaturation = 5, SynBloom = 6, SynSpeed = 7,
        ChShift     = 8, ChAspect = 9, ChSandSmooth = 10, ChParticles = 11, ChMaterial = 12,
        GeoSmooth = 13, GeoZoom = 14, GeoRotation = 15, GeoSymmetry = 16, GeoSaturation = 17,
        GeoBloom = 18, GeoSpeed = 19, GeoTri = 20, GeoSquare = 21, GeoCircle = 22,
        GeoComplexity = 23, GeoRandom = 24, GeoReact = 25, GeoTone = 26,
        GeoDepth = 27, SynReact = 28,
        // ── new controller params (Synesthesia) ──
        SynFragment = 29, SynTransmute = 30, SynMirror = 31, SynClear = 32,
        SynDenoise = 33, SynCurveSmooth = 34, SynBpmSync = 35, SynBpm = 36,
        SynBeatDiv = 37, SynTone = 38,
        // ── new controller params (Geometry) ──
        GeoTunnel = 39, GeoAperture = 40, GeoGlobalRot = 41,
        GeoBpmSync = 42, GeoBpm = 43, GeoBeatDiv = 44,
        // ── new controller params (Chladni) ──
        ChReactive = 45, ChM = 46, ChN = 47, ChTone = 48,
        SynTunnel = 49, SynVortex = 50,
        SynBrightness = 51,
        GeoBrightness = 52,
        GeoMirror     = 53,  // geometry mirror fold (shared toggle in the controller)
        // base colour ("Color..." picker) exposed as H/S/B so it is automatable
        SynColorHue = 54, SynColorSat = 55, SynColorBri = 56,
        ChColorHue  = 57, ChColorSat  = 58, ChColorBri  = 59,
        GeoColorHue = 60, GeoColorSat = 61, GeoColorBri = 62,
        ChPreset    = 63,    // Chladni "Preset" combo (classic (m,n) figures)
        // 'Mirror tone color' — reverses the tone→hue wheel direction. APPENDED at the
        // end on purpose: adding ids anywhere else would renumber the existing ones and
        // silently repoint every automation lane a user has already written. A plugin
        // build that predates these three simply never sends them.
        SynTwist = 64, ChTwist = 65, GeoTwist = 66
    };

    /** Classic Chladni figures behind the controller's Preset combo.
        Index 0 = "Custom", 1..12 = the pairs below.
        MUST match kChladniPresets in AlterCreator/Source/PluginProcessor.h. */
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

    enum ModuleType : juce::uint8 { TypeSynesthesia = 0, TypeChladni = 1, TypeGeometry = 2, TypeOther = 255 };
}

// Snapshot of one controllable ALTER module, pushed to the receiver from the
// message thread (MainComponent) so the socket thread can answer plugin queries
// without touching the (non-thread-safe) ValueTree.
struct ControlModuleDesc
{
    juce::uint32 id     = 0;
    juce::uint8  type   = AlterCtrl::TypeOther;
    juce::uint32 colour = 0xff808080;
    juce::String name;
    std::vector<std::pair<juce::uint8, float>> params;  // paramId -> current value
};

// ─────────────────────────────────────────────────────────────────────────────
// UdpReceiver
//
// Protocol v2: 'ALT2' + uint8 type ('R','P','L','F','W','I') + uint32 instanceId + payload
// Protocol v1 (legacy): 'ALTR'/'ALTP'/'ALTL'/'ALTF'/'ALTW' + payload (id = kLegacyId)
//
// Viacero instancii pluginu moze posielat naraz – receiver si vedie registry
// a data pusta dalej len z VYBRANEJ instancie (0 = auto: prva aktivna).
// ─────────────────────────────────────────────────────────────────────────────
class UdpReceiver : public IAudioSource,
                    private juce::Thread
{
public:
    static constexpr juce::uint32 kLegacyId = 1;

    explicit UdpReceiver (int listenPort)
        : juce::Thread ("UDP Receiver"), port (listenPort)
    {
        socket.setEnablePortReuse (true);
        socket.setMulticastLoopbackEnabled (false);
        const bool ok = socket.bindToPort (port);
        jassertquiet (ok);
        startThread();
    }

    ~UdpReceiver() override { stopThread (500); }

    // ===== Instance API =====

    /** 0 = auto (prva aktivna instancia), inak konkretne instance id. */
    void setSelectedInstance (juce::uint32 id) noexcept { selectedInstance.store (id); }
    juce::uint32 getSelectedInstance() const noexcept   { return selectedInstance.load(); }

    std::vector<PluginInstanceInfo> getInstances() const
    {
        const juce::ScopedLock sl (instanceLock);
        const auto now = juce::Time::getMillisecondCounter();

        std::vector<PluginInstanceInfo> out;
        out.reserve (instances.size());
        for (auto& kv : instances)
            out.push_back ({ kv.first, kv.second.name, now - kv.second.lastSeenMs < 2500 });
        return out;
    }

    // The instance whose data feeds "auto"/global consumers (selected, else auto).
    juce::uint32 effectiveInstance() const noexcept
    {
        const auto sel = selectedInstance.load();
        return sel != 0 ? sel : autoInstance.load();
    }

    // ===== IAudioSource (global = the effective instance) =====
    float getLastRms()  const noexcept override { return getInstanceRms  (effectiveInstance()); }
    float getLastPeak() const noexcept override { return getInstancePeak (effectiveInstance()); }
    float getLastLufs() const noexcept override { return getInstanceLufs (effectiveInstance()); }
    float getBPM()      const noexcept override { return getInstanceBpm  (effectiveInstance()); }

    int getLastFft (std::vector<float>& out) const override
    {
        return getInstanceFft (effectiveInstance(), out);
    }

    int getLastCqt (std::vector<float>& out) const override
    {
        return getInstanceCqt (effectiveInstance(), out);
    }

    int getLastWaveform (std::vector<float>& left, std::vector<float>& right) const override
    {
        return getInstanceWaveform (effectiveInstance(), left, right);
    }

    bool getMonoStream (std::uint64_t& ioTotal, std::vector<float>& out) const override
    {
        return getInstanceStream (effectiveInstance(), ioTotal, out);
    }

    int getFftPacketsPerSecond() const noexcept override { return getInstanceFftPps (effectiveInstance()); }

    // ===== Per-instance getters (for per-module routing / ModuleAudioSource) =====
    float getInstanceRms  (juce::uint32 id) const noexcept
    {
        const juce::ScopedLock sl (audioLock);
        auto it = audioByInstance.find (id);
        return it != audioByInstance.end() ? it->second.rms : 0.0f;
    }
    float getInstancePeak (juce::uint32 id) const noexcept
    {
        const juce::ScopedLock sl (audioLock);
        auto it = audioByInstance.find (id);
        return it != audioByInstance.end() ? it->second.peak : 0.0f;
    }
    float getInstanceLufs (juce::uint32 id) const noexcept
    {
        const juce::ScopedLock sl (audioLock);
        auto it = audioByInstance.find (id);
        return it != audioByInstance.end() ? it->second.lufs : -100.0f;
    }
    float getInstanceBpm (juce::uint32 id) const noexcept
    {
        const juce::ScopedLock sl (audioLock);
        auto it = audioByInstance.find (id);
        return it != audioByInstance.end() ? it->second.bpm : 120.0f;
    }
    int getInstanceFft (juce::uint32 id, std::vector<float>& out) const
    {
        const juce::ScopedLock sl (audioLock);
        auto it = audioByInstance.find (id);
        if (it == audioByInstance.end() || ! it->second.hasFft) { out.clear(); return 0; }
        out = it->second.fft;
        return (int) out.size();
    }
    int getInstanceCqt (juce::uint32 id, std::vector<float>& out) const
    {
        const juce::ScopedLock sl (audioLock);
        auto it = audioByInstance.find (id);
        if (it == audioByInstance.end() || ! it->second.hasCqt) { out.clear(); return 0; }
        out = it->second.cqt;
        return (int) out.size();
    }
    int getInstanceWaveform (juce::uint32 id, std::vector<float>& left, std::vector<float>& right) const
    {
        const juce::ScopedLock sl (audioLock);
        auto it = audioByInstance.find (id);
        if (it == audioByInstance.end() || ! it->second.hasWave) { left.clear(); right.clear(); return 0; }
        left  = it->second.waveL;
        right = it->second.waveR;
        return (int) left.size();
    }
    int getInstanceFftPps (juce::uint32 id) const noexcept
    {
        const juce::ScopedLock sl (audioLock);
        auto it = audioByInstance.find (id);
        return it != audioByInstance.end() ? it->second.fftPacketsPerSecond : 0;
    }
    /** Held MIDI notes of one instance. Returns true while MIDI is PRESENT on
        that track (an 'M' packet arrived within ~2 s) — even with 0 held notes. */
    bool getInstanceMidi (juce::uint32 id, std::vector<IAudioSource::MidiNote>& out) const
    {
        out.clear();
        const juce::ScopedLock sl (audioLock);
        auto it = audioByInstance.find (id);
        if (it == audioByInstance.end() || it->second.midiLastMs == 0)
            return false;
        if (juce::Time::getMillisecondCounter() - it->second.midiLastMs > 2000)
            return false;   // stale → fall back to audio detection

        const auto& a = it->second;
        out.reserve ((size_t) a.midiCount);
        for (int i = 0; i < a.midiCount; ++i)
            out.push_back ({ a.midiNotes[i][0], a.midiNotes[i][1] });
        return true;
    }

    bool getMidiNotes (std::vector<MidiNote>& out) const override
    {
        return getInstanceMidi (effectiveInstance(), out);
    }

    bool getInstanceHostSync (juce::uint32 id, HostSyncInfo& out) const
    {
        const juce::ScopedLock sl (audioLock);
        auto it = audioByInstance.find (id);
        if (it == audioByInstance.end() || (it->second.hostAtMs == 0 && it->second.syncPulseAtMs == 0))
            return false;

        const auto& a = it->second;
        const juce::uint32 now = juce::Time::getMillisecondCounter();
        out.bpm     = a.hostBpm;
        out.ppq     = a.hostPpq;
        out.playing = a.hostPlaying;
        out.ageMs   = a.hostAtMs != 0 ? (now - a.hostAtMs) : 0xFFFFFFFFu;
        out.pulseId    = a.syncPulseCount;
        out.pulseAgeMs = a.syncPulseAtMs != 0 ? (now - a.syncPulseAtMs) : 0xFFFFFFFFu;
        return true;
    }

    /** Transport from ANY instance that reports one (freshest wins). Only the
        CREATOR plugin sends 'T'/'S' — a module whose audio comes from a Listener
        instance (or the auto pick) would otherwise never see the DAW transport,
        which is why BPM sync impulses "didn't work" in Synesthesia/Geometry. */
    bool getAnyHostSync (HostSyncInfo& out) const
    {
        const juce::ScopedLock sl (audioLock);
        const InstanceAudio* best = nullptr;
        juce::uint32 bestAt = 0;
        for (const auto& kv : audioByInstance)
        {
            const juce::uint32 at = juce::jmax (kv.second.hostAtMs, kv.second.syncPulseAtMs);
            if (at > bestAt) { bestAt = at; best = &kv.second; }
        }
        if (best == nullptr) return false;

        const juce::uint32 now = juce::Time::getMillisecondCounter();
        out.bpm        = best->hostBpm;
        out.ppq        = best->hostPpq;
        out.playing    = best->hostPlaying;
        out.ageMs      = best->hostAtMs      != 0 ? (now - best->hostAtMs)      : 0xFFFFFFFFu;
        out.pulseId    = best->syncPulseCount;
        out.pulseAgeMs = best->syncPulseAtMs != 0 ? (now - best->syncPulseAtMs) : 0xFFFFFFFFu;
        return true;
    }

    bool getHostSync (HostSyncInfo& out) const override
    {
        if (getInstanceHostSync (effectiveInstance(), out)) return true;
        return getAnyHostSync (out);
    }

    bool getInstanceStream (juce::uint32 id, std::uint64_t& ioTotal, std::vector<float>& out) const
    {
        const juce::ScopedLock sl (audioLock);
        out.clear();
        auto it = audioByInstance.find (id);
        if (it == audioByInstance.end() || it->second.streamTotal == 0)
            return false;   // no 'W' stream yet → caller falls back to snapshots

        const auto& a = it->second;
        const std::uint64_t total = a.streamTotal;
        std::uint64_t start = juce::jmin (ioTotal, total);
        if (total - start > (std::uint64_t) kStreamRingSize)
            start = total - (std::uint64_t) kStreamRingSize;

        out.resize ((size_t) (total - start));
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = a.streamRing[(size_t) ((start + i) % kStreamRingSize)];

        ioTotal = total;
        return true;
    }

    /** Gap-free STEREO version of getInstanceStream: the same 'W' reconstruction,
        but with the L/R channels kept separate (used by the HUD recorder so an
        exported .mp4 can carry the plugin's real stereo signal, not a downmix).
        Shares the sample counter with getInstanceStream. */
    bool getInstanceStreamStereo (juce::uint32 id, std::uint64_t& ioTotal,
                                  std::vector<float>& outL, std::vector<float>& outR) const
    {
        const juce::ScopedLock sl (audioLock);
        outL.clear(); outR.clear();
        auto it = audioByInstance.find (id);
        if (it == audioByInstance.end() || it->second.streamTotal == 0
            || it->second.streamRingL.size() != kStreamRingSize)
            return false;

        const auto& a = it->second;
        const std::uint64_t total = a.streamTotal;
        std::uint64_t start = juce::jmin (ioTotal, total);
        if (total - start > (std::uint64_t) kStreamRingSize)
            start = total - (std::uint64_t) kStreamRingSize;

        const size_t n = (size_t) (total - start);
        outL.resize (n);
        outR.resize (n);
        for (size_t i = 0; i < n; ++i)
        {
            const size_t k = (size_t) ((start + i) % kStreamRingSize);
            outL[i] = a.streamRingL[k];
            outR[i] = a.streamRingR[k];
        }

        ioTotal = total;
        return true;
    }

    /** Measured sample rate of an instance's 'W' stream, in Hz (0 = not measured
        yet). The protocol carries no sample-rate field, so it is derived from how
        fast samples actually arrive (averaged over ~1 s windows). */
    double getInstanceStreamRate (juce::uint32 id) const noexcept
    {
        const juce::ScopedLock sl (audioLock);
        auto it = audioByInstance.find (id);
        return it != audioByInstance.end() ? it->second.streamRateHz : 0.0;
    }

    // ===== Control channel API =====

    /** Called from the message thread (MainComponent) whenever the set of
        controllable modules or their values changes. Thread-safe snapshot. */
    void setControlRegistry (std::vector<ControlModuleDesc> reg)
    {
        const juce::ScopedLock sl (controlLock);
        controlRegistry = std::move (reg);
    }

    /** Current ALTER visual theme index (0=Cyber,1=Dark,2=Solar,3=White),
        forwarded to control-mode plugins so their editor matches the app. */
    void setControlTheme (int t) noexcept { controlTheme.store (t); }

    /** The two colours the CUSTOM theme (index 2) is derived from. The theme index
        alone cannot describe that theme — every colour in it comes from this pair —
        so it has to travel with the registry or a Control-mode plugin has no way to
        match the app's appearance. */
    void setControlThemeColours (juce::uint32 primaryARGB, juce::uint32 secondaryARGB) noexcept
    {
        controlThemePrimary.store (primaryARGB);
        controlThemeSecondary.store (secondaryARGB);
    }

    // Data-needs bitmask bits (R=1,P=2,L=4,F=8,W=16). Tells each plugin instance
    // which packet types are actually consumed so it can skip the rest.
    enum NeedBits { NeedRms = 1, NeedPeak = 2, NeedLufs = 4, NeedFft = 8, NeedWave = 16, NeedCqt = 32 };

    /** globalMask = union of modules on "auto" (fed by the effective instance);
        overrideMask = per-instance needs for modules pinned to a specific instance. */
    void setInstanceNeeds (juce::uint8 globalMask, std::map<juce::uint32, juce::uint8> overrideMask)
    {
        const juce::ScopedLock sl (needsLock);
        globalNeeds.store (globalMask);
        overrideNeeds = std::move (overrideMask);
    }

    /** The HUD recorder's claim on an instance's data.

        The needs mask above is derived purely from what the VISIBLE MODULES draw,
        which is right for visuals and wrong for recording: exporting a plugin's
        audio track needs the 'W' (waveform) packet, and unless some oscilloscope
        or stereoscope happens to be on screen nobody asks for it — so the plugin
        stops sending it and the export comes out silent. The recorder registers
        its own need here; it is OR-ed into that instance's mask and survives any
        recomputeAndPushNeeds() triggered by the user rearranging modules
        mid-take. Pass id 0 to clear when the recording stops. */
    void setRecordingNeeds (const std::map<juce::uint32, juce::uint8>& perInstance)
    {
        const juce::ScopedLock sl (needsLock);
        recordingNeeds = perInstance;
    }

    /** Convenience for the single-file case. Pass id 0 to clear. */
    void setRecordingNeeds (juce::uint32 instId, juce::uint8 mask)
    {
        std::map<juce::uint32, juce::uint8> m;
        if (instId != 0 && mask != 0) m[instId] = mask;
        setRecordingNeeds (m);
    }

    /** Global FFT order (12..14) that plugins should compute/send. */
    void setFftOrder (int order) noexcept { fftOrder.store (juce::jlimit (10, 14, order)); }

    /** Invoked from the socket thread when a plugin sends a 'C' control packet.
        The handler is responsible for marshalling to the message thread. */
    std::function<void (juce::uint32 moduleId, juce::uint8 paramId, float value)> onControlValue;

private:
    struct InstanceRecord
    {
        juce::String name;
        juce::uint32 lastSeenMs = 0;
        juce::String senderIp;
        int          senderPort = 0;
        int          mode       = 0;   // 0=Listener, 1=Control (from 'O' packets; was 'M')
    };

    void run() override
    {
        uint8_t buf[65536];
       #if JUCE_DEBUG
        int dbgTypeCounts[128] = {};      // packets per type, rolled once a second
       #endif

        juce::uint32 lastTickMs     = juce::Time::getMillisecondCounter();
        juce::uint32 lastRegistryMs = lastTickMs;

        while (! threadShouldExit())
        {
            // periodically push the module registry to every active plugin
            const auto loopNow = juce::Time::getMillisecondCounter();
            if (loopNow - lastRegistryMs >= 500)
            {
                lastRegistryMs = loopNow;
                broadcastRegistry (loopNow);
            }

            // once per second: roll per-instance FFT/s + prune dead instances.
            // MUST run every iteration (NOT gated behind packet arrival) so that
            // a removed plugin is pruned even after the LAST instance stops sending
            // — otherwise the loop just `continue`s below and the dead entry lingers
            // forever in the instance picker.
            if (loopNow - lastTickMs >= 1000)
            {
                lastTickMs = loopNow;
                {
                    const juce::ScopedLock sl (audioLock);
                    for (auto& kv : audioByInstance)
                    {
                        kv.second.fftPacketsPerSecond = kv.second.fftPacketsThisSec;
                        kv.second.fftPacketsThisSec = 0;
                    }
                }
                pruneInstances (loopNow);

               #if JUCE_DEBUG
                // Packet-type census. The recorder's plugin audio source lives or
                // dies on the 'W' (waveform) packet — every other packet type can
                // be flowing, the visuals looking perfectly alive, and the export
                // still be silent, because ONLY 'W' feeds the gap-free stream.
                // So: say once a second what actually arrived.
                {
                    juce::String census;
                    for (int t = 0; t < 128; ++t)
                        if (dbgTypeCounts[t] > 0)
                            census << (char) t << ":" << dbgTypeCounts[t] << " ";
                    DBG ("ALTER UDP: " << (census.isEmpty() ? juce::String ("no packets") : census));
                    std::memset (dbgTypeCounts, 0, sizeof (dbgTypeCounts));
                }
               #endif
            }

            if (socket.waitUntilReady (true, 100) <= 0)
                continue;

            juce::String senderIp;
            int          senderPort = 0;
            const int n = socket.read (buf, (int) sizeof (buf), false, senderIp, senderPort);
            if (n < 4) continue;

            uint8_t      type = 0;
            juce::uint32 instId = 0;
            const uint8_t* payload = nullptr;
            int payloadBytes = 0;

            if (buf[0]=='A' && buf[1]=='L' && buf[2]=='T' && buf[3]=='2' && n >= 9)
            {
                // v2: magic + type + instanceId
                type = buf[4];
                std::memcpy (&instId, buf + 5, sizeof (instId));
                payload      = buf + 9;
                payloadBytes = n - 9;
            }
            else if (buf[0]=='A' && buf[1]=='L' && buf[2]=='T')
            {
                // v1 legacy: 4th magic byte is the type
                type = buf[3];
                instId = kLegacyId;
                payload      = buf + 4;
                payloadBytes = n - 4;
            }
            else
                continue;

           #if JUCE_DEBUG
            if (type < 128) ++dbgTypeCounts[type];
            // A 'W' that gets REJECTED downstream looks identical to one that never
            // came, so flag the shape here: the payload must be a whole number of
            // floats, an even count, and 2..32768 of them.
            if (type == 'W' && (payloadBytes % 4 != 0
                                || (payloadBytes / 4) % 2 != 0
                                || payloadBytes / 4 < 2 || payloadBytes / 4 > 32768))
            {
                DBG ("ALTER UDP: malformed 'W' payload — " << payloadBytes
                     << " bytes (" << (payloadBytes / 4) << " floats), rejected");
            }
           #endif

            touchInstance (instId,
                           type == 'I'
                               ? juce::String::fromUTF8 ((const char*) payload, juce::jmin (payloadBytes, 64))
                               : juce::String(),
                           senderIp, senderPort);

            // ── Control channel (independent of which instance feeds audio) ──
            // NOTE: mode announce moved 'M' → 'O'; 'M' now carries MIDI notes
            // (plugins must be rebuilt together with this app).
            if (type == 'O') { setInstanceMode (instId, payloadBytes > 0 ? payload[0] : 0); continue; }
            if (type == 'B') { handleBind (instId, payload, payloadBytes);   continue; }
            if (type == 'C') { handleControl (instId, payload, payloadBytes); continue; }

            if (type == 'I')
                continue;   // every instance's audio is stored now (no accepted-gate)

            {
                const juce::ScopedLock sl (audioLock);
                auto& a = audioByInstance[instId];

                switch (type)
                {
                    case 'R':
                        if (float v; readFloat (payload, payloadBytes, v))
                        {
                            a.rms = v;
                            detectOnsetFor (a, v);
                        }
                        break;

                    case 'P':
                        if (float v; readFloat (payload, payloadBytes, v))
                            a.peak = v;
                        break;

                    case 'L':
                        if (float v; readFloat (payload, payloadBytes, v))
                            a.lufs = v;
                        break;

                    case 'F':
                        if (readFloatVector (payload, payloadBytes, 64, 16384, scratch))
                        {
                            for (auto& v : scratch)
                                v = std::isfinite (v) ? juce::jlimit (0.0f, 1.0f, v) : 0.0f;
                            a.fft = scratch;
                            a.hasFft = true;
                            ++a.fftPacketsThisSec;
                        }
                        break;

                    case 'T':   // host transport: float bpm + double ppq + uint8 playing
                        if (payloadBytes >= 13)
                        {
                            float  tb = 0.0f; double tp = -1.0;
                            std::memcpy (&tb, payload, 4);
                            std::memcpy (&tp, payload + 4, 8);
                            a.hostBpm     = std::isfinite (tb) ? tb : 0.0f;
                            a.hostPpq     = std::isfinite (tp) ? tp : -1.0;
                            a.hostPlaying = payload[12] != 0;
                            a.hostAtMs    = juce::Time::getMillisecondCounter();
                        }
                        break;

                    case 'S':   // sync impulse: DAW transport (re)start marker (+ ppq)
                        ++a.syncPulseCount;
                        a.syncPulseAtMs = juce::Time::getMillisecondCounter();
                        break;

                    case 'M':   // held MIDI notes ("ALTM"): uint8 count + count×(note, vel)
                        if (payloadBytes >= 1)
                        {
                            const int cnt = juce::jmin ((int) payload[0], 32);
                            if (payloadBytes >= 1 + cnt * 2)
                            {
                                for (int i = 0; i < cnt; ++i)
                                {
                                    a.midiNotes[i][0] = payload[1 + i * 2];
                                    a.midiNotes[i][1] = payload[2 + i * 2];
                                }
                                a.midiCount  = cnt;
                                a.midiLastMs = juce::Time::getMillisecondCounter();
                            }
                        }
                        break;

                    case 'Q':   // constant-Q (multi-resolution) magnitudes, log-spaced
                        if (readFloatVector (payload, payloadBytes, 16, 4096, scratch))
                        {
                            for (auto& v : scratch)
                                v = std::isfinite (v) ? juce::jlimit (0.0f, 1.0f, v) : 0.0f;
                            a.cqt = scratch;
                            a.hasCqt = true;
                        }
                        break;

                    case 'W':
                        if (readFloatVector (payload, payloadBytes, 2, 32768, scratch)
                            && scratch.size() % 2 == 0)
                        {
                            const int samplesPerCh = (int) scratch.size() / 2;
                            a.waveL.resize ((size_t) samplesPerCh);
                            a.waveR.resize ((size_t) samplesPerCh);
                            for (int i = 0; i < samplesPerCh; ++i)
                            {
                                const float l = scratch[(size_t)(i*2)], r = scratch[(size_t)(i*2+1)];
                                a.waveL[(size_t) i] = std::isfinite (l) ? l : 0.0f;
                                a.waveR[(size_t) i] = std::isfinite (r) ? r : 0.0f;
                            }
                            a.hasWave = true;

                            // ── gap-free mono stream ─────────────────────────
                            // The plugin sends one 'W' snapshot of its ring per
                            // FFT hop (hop = fftSize/4), so the NEWEST `hop`
                            // samples of each packet are the fresh ones. Append
                            // just those → a continuous stream (a lost packet
                            // costs one hop, which is fine for a visualiser).
                            {
                                if (a.streamRing.size() != kStreamRingSize)
                                    a.streamRing.assign (kStreamRingSize, 0.0f);
                                if (a.streamRingL.size() != kStreamRingSize)
                                {
                                    a.streamRingL.assign (kStreamRingSize, 0.0f);
                                    a.streamRingR.assign (kStreamRingSize, 0.0f);
                                }

                                const int hop   = juce::jlimit (64, samplesPerCh,
                                                                (1 << fftOrder.load()) / 4);
                                const int fresh = (a.streamTotal == 0) ? samplesPerCh : hop;
                                for (int i = samplesPerCh - fresh; i < samplesPerCh; ++i)
                                {
                                    const float l = a.waveL[(size_t) i];
                                    const float r = a.waveR[(size_t) i];
                                    const size_t k = (size_t) (a.streamTotal % kStreamRingSize);
                                    a.streamRing [k] = 0.5f * (l + r);
                                    a.streamRingL[k] = l;
                                    a.streamRingR[k] = r;
                                    ++a.streamTotal;
                                }

                                // ── stream rate estimate (samples / second) ──
                                // Needed by the recorder: the exported .mp4 must be
                                // told the real rate or the plugin track plays back
                                // at the wrong speed.
                                const juce::uint32 nowMs = juce::Time::getMillisecondCounter();

                                // A GAP INVALIDATES THE WINDOW.
                                //
                                // This stream is switched off whenever no module on
                                // screen draws a waveform, and back on when a
                                // recording asks for it — so a pause of minutes is
                                // normal, not a fault. The samples do not accumulate
                                // while it is off, so a window straddling the pause
                                // divides a handful of samples by the length of the
                                // pause and reports a rate near zero. Smoothed into
                                // the previously correct value that lands somewhere
                                // plausible-looking and wrong, the export is told the
                                // wrong sample rate, and the plugin's track comes out
                                // playing at the wrong speed.
                                //
                                // So a gap restarts the measurement from scratch
                                // rather than dragging the old average across it.
                                if (a.waveLastMs != 0 && nowMs - a.waveLastMs > 300)
                                {
                                    a.rateWindowStartMs = 0;
                                    a.streamRateHz      = 0.0;
                                }
                                a.waveLastMs = nowMs;

                                if (a.rateWindowStartMs == 0)
                                {
                                    a.rateWindowStartMs    = nowMs;
                                    a.rateWindowStartTotal = a.streamTotal;
                                }
                                else if (nowMs - a.rateWindowStartMs >= 1000)
                                {
                                    const double secs = (double) (nowMs - a.rateWindowStartMs) / 1000.0;
                                    const double hz   = (double) (a.streamTotal - a.rateWindowStartTotal) / secs;
                                    // light smoothing so one late packet burst cannot skew it
                                    a.streamRateHz = (a.streamRateHz > 0.0) ? (0.7 * a.streamRateHz + 0.3 * hz) : hz;
                                    a.rateWindowStartMs    = nowMs;
                                    a.rateWindowStartTotal = a.streamTotal;
                                }
                            }
                        }
                        break;

                    default: break;
                }
            }

        }
    }

    // ===== control channel helpers =====

    int writeOutHeader (uint8_t* dst, uint8_t type, juce::uint32 instId) const noexcept
    {
        dst[0]='A'; dst[1]='L'; dst[2]='T'; dst[3]='2';
        dst[4]=type;
        std::memcpy (dst + 5, &instId, sizeof (instId));
        return 9;
    }

    void setInstanceMode (juce::uint32 id, juce::uint8 m)
    {
        const juce::ScopedLock sl (instanceLock);
        auto it = instances.find (id);
        if (it != instances.end())
            it->second.mode = (int) m;
    }

    // Plugin -> 'C': apply a single param to a module (marshalled by the handler).
    // Enforces exclusive ownership: a module can only be driven by the instance
    // that owns it (claims it if free, steals only from a dead owner).
    void handleControl (juce::uint32 instId, const uint8_t* payload, int bytes)
    {
        if (bytes < (int) sizeof (juce::uint32) + 1 + (int) sizeof (float)) return;
        juce::uint32 moduleId = 0;
        std::memcpy (&moduleId, payload, sizeof (moduleId));
        const uint8_t paramId = payload[sizeof (juce::uint32)];
        float value = 0.0f;
        std::memcpy (&value, payload + sizeof (juce::uint32) + 1, sizeof (float));

        if (! claimOwnership (moduleId, instId))
            return;   // owned by another live instance -> ignore

        if (onControlValue)
            onControlValue (moduleId, paramId, value);
    }

    // Returns true if `instId` owns (or may take) `moduleId`. Claims free modules
    // and steals from dead owners; refuses if a different LIVE instance owns it.
    bool claimOwnership (juce::uint32 moduleId, juce::uint32 instId)
    {
        const auto now = juce::Time::getMillisecondCounter();
        const juce::ScopedLock sl (instanceLock);

        auto own = moduleOwner.find (moduleId);
        if (own != moduleOwner.end())
        {
            if (own->second == instId)
                return true;                       // already ours: nothing to do

            auto oit = instances.find (own->second);
            const bool ownerAlive = (oit != instances.end()) && (now - oit->second.lastSeenMs < 3000);
            if (ownerAlive)
                return false;                      // owned by another live instance
        }
        // release any previous module this instance held, then take this one
        for (auto it = moduleOwner.begin(); it != moduleOwner.end();)
            it = (it->second == instId && it->first != moduleId) ? moduleOwner.erase (it) : std::next (it);
        moduleOwner[moduleId] = instId;
        return true;
    }

    // Plugin -> 'B': bind to a module; reply with a 'V' value-sync packet
    void handleBind (juce::uint32 instId, const uint8_t* payload, int bytes)
    {
        if (bytes < (int) sizeof (juce::uint32)) return;
        juce::uint32 moduleId = 0;
        std::memcpy (&moduleId, payload, sizeof (moduleId));

        // find the module + its current values
        ControlModuleDesc desc;
        bool found = false;
        {
            const juce::ScopedLock sl (controlLock);
            for (auto& m : controlRegistry)
                if (m.id == moduleId) { desc = m; found = true; break; }
        }
        if (! found) return;

        // exclusive ownership: refuse if another live instance already owns it
        if (! claimOwnership (moduleId, instId)) return;

        // resolve sender address
        juce::String ip; int port = 0;
        {
            const juce::ScopedLock sl (instanceLock);
            auto it = instances.find (instId);
            if (it == instances.end() || it->second.senderIp.isEmpty()) return;
            ip   = it->second.senderIp;
            port = it->second.senderPort;
        }

        // build 'V' : moduleId + count + (paramId,float)*
        std::vector<uint8_t> pkt (9 + 4 + 1 + desc.params.size() * (1 + sizeof (float)));
        int pos = writeOutHeader (pkt.data(), (uint8_t) 'V', instId);
        std::memcpy (pkt.data() + pos, &moduleId, sizeof (moduleId)); pos += 4;
        pkt[pos++] = (uint8_t) desc.params.size();
        for (auto& kv : desc.params)
        {
            pkt[pos++] = kv.first;
            std::memcpy (pkt.data() + pos, &kv.second, sizeof (float)); pos += (int) sizeof (float);
        }
        socket.write (ip, port, pkt.data(), pos);
    }

    // Push the current module registry ('G') to every active controlling-capable plugin
    void broadcastRegistry (juce::uint32 now)
    {
        // snapshot the registry
        std::vector<ControlModuleDesc> reg;
        {
            const juce::ScopedLock sl (controlLock);
            reg = controlRegistry;
        }

        // snapshot active instance addresses + current module ownership (id + owner name)
        struct Target { juce::uint32 id; juce::String ip; int port; };
        std::vector<Target> targets;
        struct OwnerInfo { juce::uint32 instId; juce::String name; };
        std::map<juce::uint32, OwnerInfo> owners;   // moduleId -> live owner
        {
            const juce::ScopedLock sl (instanceLock);
            for (auto& kv : instances)
                if (kv.second.senderIp.isNotEmpty() && now - kv.second.lastSeenMs < 3000)
                    targets.push_back ({ kv.first, kv.second.senderIp, kv.second.senderPort });

            for (auto& kv : moduleOwner)
            {
                auto oit = instances.find (kv.second);
                if (oit != instances.end() && now - oit->second.lastSeenMs < 3000)
                    owners[kv.first] = { kv.second, oit->second.name };
            }
        }
        if (targets.empty()) return;

        // build 'G' : themeIndex + count + per module
        //   (id, colour, type, ownerInstId, nameLen, name, ownerLen, ownerName)
        std::vector<uint8_t> body;
        body.push_back ((uint8_t) juce::jlimit (0, 255, controlTheme.load()));
        body.push_back ((uint8_t) juce::jmin (255, (int) reg.size()));
        for (auto& m : reg)
        {
            juce::uint32 ownerId = 0;
            juce::String ownerName;
            if (auto it = owners.find (m.id); it != owners.end())
            {
                ownerId   = it->second.instId;
                ownerName = it->second.name;
            }

            const auto nameUtf8  = m.name.toRawUTF8();
            const int  nameLen   = juce::jmin (200, (int) std::strlen (nameUtf8));
            const auto ownerUtf8 = ownerName.toRawUTF8();
            const int  ownerLen  = juce::jmin (200, (int) std::strlen (ownerUtf8));

            const size_t base = body.size();
            body.resize (base + 15 + (size_t) nameLen + (size_t) ownerLen);
            uint8_t* d = body.data() + base;
            std::memcpy (d, &m.id, 4);      d += 4;
            std::memcpy (d, &m.colour, 4);  d += 4;
            *d++ = m.type;
            std::memcpy (d, &ownerId, 4);   d += 4;
            *d++ = (uint8_t) nameLen;
            std::memcpy (d, nameUtf8, (size_t) nameLen);  d += nameLen;
            *d++ = (uint8_t) ownerLen;
            std::memcpy (d, ownerUtf8, (size_t) ownerLen);
        }

        // ── trailer: the CUSTOM theme's two source colours ────────────────────
        // Appended AFTER the module list on purpose. A reader that only knows the
        // old layout stops once it has consumed `count` modules and never looks at
        // these bytes, so an older plugin keeps working unchanged; a newer one
        // checks for the extra 8 bytes and picks them up. Putting them next to the
        // theme byte at the front would have shifted every field behind it and
        // broken every existing build.
        {
            const juce::uint32 pri = controlThemePrimary.load();
            const juce::uint32 sec = controlThemeSecondary.load();
            const size_t base = body.size();
            body.resize (base + 8);
            std::memcpy (body.data() + base,     &pri, 4);
            std::memcpy (body.data() + base + 4, &sec, 4);
        }

        for (auto& t : targets)
        {
            std::vector<uint8_t> pkt (9 + body.size());
            writeOutHeader (pkt.data(), (uint8_t) 'G', t.id);
            std::memcpy (pkt.data() + 9, body.data(), body.size());
            socket.write (t.ip, t.port, pkt.data(), (int) pkt.size());
        }

        // 'N' : per-instance data-needs mask (skip packet types nobody consumes)
        const juce::uint8  gNeeds = globalNeeds.load();
        const juce::uint32 effId  = effectiveInstance();
        for (auto& t : targets)
        {
            juce::uint8 mask = 0;
            {
                const juce::ScopedLock sl (needsLock);
                auto it = overrideNeeds.find (t.id);
                if (it != overrideNeeds.end()) mask = it->second;
            }
            if (t.id == effId) mask |= gNeeds;   // auto modules feed from the effective instance

            {   // every recording currently running that reads THIS instance
                const juce::ScopedLock sl (needsLock);
                if (auto it = recordingNeeds.find (t.id); it != recordingNeeds.end())
                    mask |= it->second;
            }

            uint8_t pkt[10];
            writeOutHeader (pkt, (uint8_t) 'N', t.id);
            pkt[9] = mask;
            socket.write (t.ip, t.port, pkt, 10);
        }

        // 'B' : global FFT order (resolution) — every instance computes the same size
        {
            const uint8_t order = (uint8_t) fftOrder.load();
            for (auto& t : targets)
            {
                uint8_t pkt[10];
                writeOutHeader (pkt, (uint8_t) 'B', t.id);
                pkt[9] = order;
                socket.write (t.ip, t.port, pkt, 10);
            }
        }
    }

    // ===== helpers =====

    static bool readFloat (const uint8_t* p, int bytes, float& out)
    {
        if (bytes < (int) sizeof (float)) return false;
        std::memcpy (&out, p, sizeof (float));
        return true;
    }

    static bool readFloatVector (const uint8_t* p, int bytes, int minFloats, int maxFloats,
                                 std::vector<float>& out)
    {
        if (bytes % (int) sizeof (float) != 0) return false;
        const int count = bytes / (int) sizeof (float);
        if (count < minFloats || count > maxFloats) return false;
        out.resize ((size_t) count);
        std::memcpy (out.data(), p, (size_t) bytes);
        return true;
    }

    void touchInstance (juce::uint32 id, const juce::String& announcedName,
                        const juce::String& senderIp = {}, int senderPort = 0)
    {
        const auto now = juce::Time::getMillisecondCounter();
        const juce::ScopedLock sl (instanceLock);

        auto& rec = instances[id];
        rec.lastSeenMs = now;
        if (senderIp.isNotEmpty()) { rec.senderIp = senderIp; rec.senderPort = senderPort; }
        if (announcedName.isNotEmpty())
            rec.name = announcedName;
        else if (rec.name.isEmpty())
            rec.name = (id == kLegacyId)
                     ? "Plugin (legacy)"
                     : "Plugin #" + juce::String::toHexString ((int)(id & 0xFFFF)).toUpperCase();

        // auto-pick: ak nie je nic zvolene a auto kandidat zomrel/neexistuje
        const auto cur = autoInstance.load();
        if (cur == 0 || instances.find (cur) == instances.end()
            || now - instances[cur].lastSeenMs > 2500)
            autoInstance.store (id);
    }

    void pruneInstances (juce::uint32 now)
    {
        std::vector<juce::uint32> dead;
        {
            const juce::ScopedLock sl (instanceLock);
            for (auto it = instances.begin(); it != instances.end();)
            {
                // 4 s: a live plugin sends an 'I' announce every ~500 ms (even when
                // silent or bypassed), so no traffic for 4 s means it was removed /
                // moved → drop it from the picker promptly instead of after 15 s.
                if (now - it->second.lastSeenMs > 4000)
                {
                    const juce::uint32 deadId = it->first;
                    dead.push_back (deadId);
                    // free any modules this dead instance owned
                    for (auto b = moduleOwner.begin(); b != moduleOwner.end();)
                        b = (b->second == deadId) ? moduleOwner.erase (b) : std::next (b);
                    it = instances.erase (it);
                }
                else
                    ++it;
            }
        }

        if (! dead.empty())
        {
            const juce::ScopedLock sl (audioLock);
            for (auto id : dead)
                audioByInstance.erase (id);
        }
    }

    bool isAcceptedInstance (juce::uint32 id) const noexcept
    {
        const auto sel = selectedInstance.load();
        return sel != 0 ? id == sel : id == autoInstance.load();
    }

    // Per-instance audio: every plugin instance's stream is stored so any module
    // can read any instance (per-module routing). Guarded by audioLock.
    static constexpr size_t kStreamRingSize = 32768;   // ~680 ms @ 48 kHz
    struct InstanceAudio
    {
        float rms = 0.0f, peak = 0.0f, lufs = -100.0f, bpm = 120.0f;
        std::vector<float> fft;            bool hasFft  = false;
        std::vector<float> cqt;            bool hasCqt  = false;
        std::vector<float> waveL, waveR;   bool hasWave = false;
        // gap-free mono stream reconstructed from 'W' packets (spectrogram STFT)
        std::vector<float> streamRing;
        std::uint64_t      streamTotal = 0;
        // When the last 'W' arrived. Used to spot the stream being switched off and
        // on again, which would otherwise poison the rate estimate (see the 'W' case).
        juce::uint32       waveLastMs = 0;
        // same reconstruction, channels kept apart (HUD recorder: stereo export)
        std::vector<float> streamRingL, streamRingR;
        // measured stream rate: the protocol has no sample-rate field, so it is
        // derived from how fast samples actually arrive (~1 s averaging window)
        double        streamRateHz     = 0.0;
        juce::uint32  rateWindowStartMs = 0;
        std::uint64_t rateWindowStartTotal = 0;
        // host transport ('T' packets): tempo + PPQ beat position + playing flag
        float        hostBpm     = 0.0f;
        double       hostPpq     = -1.0;
        bool         hostPlaying = false;
        juce::uint32 hostAtMs    = 0;      // 0 = never received
        // sync IMPULSE ('S' packets): a transport (re)start marker
        juce::uint32 syncPulseCount = 0;   // increments per 'S' received
        juce::uint32 syncPulseAtMs  = 0;   // 0 = never received
        int fftPacketsThisSec = 0, fftPacketsPerSecond = 0;
        // held MIDI notes ('M' packets — "ALTM"): exact notes from the DAW track
        juce::uint8  midiNotes[32][2] = {};   // (note, velocity)
        int          midiCount  = 0;
        juce::uint32 midiLastMs = 0;          // 0 = never received
        // onset/BPM detection state
        std::vector<juce::int64> onsetTimes;
        juce::int64 lastOnsetTime = 0;
        float lastRmsForOnset = 0.0f;
    };

    // ===== state =====
    mutable juce::CriticalSection audioLock;
    std::map<juce::uint32, InstanceAudio> audioByInstance;

    std::vector<float> scratch;   // reused parse buffer (receiver thread only)

    mutable juce::CriticalSection instanceLock;
    std::map<juce::uint32, InstanceRecord> instances;
    std::map<juce::uint32, juce::uint32>   moduleOwner;   // moduleId -> owning instanceId (exclusive bind)
    std::atomic<juce::uint32> selectedInstance { 0 };  // 0 = auto
    std::atomic<juce::uint32> autoInstance     { 0 };

    // control registry snapshot (written by message thread, read by socket thread)
    mutable juce::CriticalSection  controlLock;
    std::vector<ControlModuleDesc> controlRegistry;
    std::atomic<int>               controlTheme { 0 };
    std::atomic<juce::uint32>      controlThemePrimary   { 0xFF3D96E7 };
    std::atomic<juce::uint32>      controlThemeSecondary { 0xFF6902D6 };

    // data-needs masks (written by message thread, read by socket thread)
    mutable juce::CriticalSection            needsLock;
    std::atomic<juce::uint8>                 globalNeeds { 0x1F };   // default: send everything
    std::map<juce::uint32, juce::uint8>      overrideNeeds;
    // Per-instance, because "export each module separately" records several at
    // once and each one may be fed by a different plugin. A single id here meant
    // the other takes silently got no waveform stream at all.
    std::map<juce::uint32, juce::uint8>      recordingNeeds;
    std::atomic<int>                         fftOrder { 12 };        // global FFT order pushed via 'B'

    juce::DatagramSocket socket;
    const int port;

    // onset/BPM detection for one instance (called under audioLock)
    void detectOnsetFor (InstanceAudio& a, float rms)
    {
        const auto now = (juce::int64) juce::Time::getMillisecondCounter();

        if (rms > a.lastRmsForOnset * 1.5f && rms > 0.1f && (now - a.lastOnsetTime) > 200)
        {
            a.onsetTimes.push_back (now);
            a.lastOnsetTime = now;

            if (a.onsetTimes.size() > 8)
                a.onsetTimes.erase (a.onsetTimes.begin());

            if (a.onsetTimes.size() >= 4)
            {
                float avgInterval = 0.0f;
                for (size_t i = 1; i < a.onsetTimes.size(); ++i)
                    avgInterval += (float) (a.onsetTimes[i] - a.onsetTimes[i - 1]);
                avgInterval /= (float) (a.onsetTimes.size() - 1);

                if (avgInterval > 200.0f && avgInterval < 2000.0f)
                    a.bpm = juce::jlimit (60.0f, 200.0f, 60000.0f / avgInterval);
            }
        }

        a.lastRmsForOnset = rms * 0.9f + a.lastRmsForOnset * 0.1f;
    }
};
