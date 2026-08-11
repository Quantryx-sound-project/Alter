/*
  ==============================================================================

    ModuleAudioSource.h
    Per-module audio routing. Each module reads through one of these instead of
    the global source directly:
      - override == 0  -> "auto": forwards to the current GLOBAL source
                          (UDP effective instance, or System Audio),
      - override == id -> reads that specific plugin instance's stream from the
                          UdpReceiver (higher priority than the global pick).

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <atomic>
#include "AudioSourceInterface.h"
#include "UdpReceiver.h"

class ModuleAudioSource : public IAudioSource
{
public:
    explicit ModuleAudioSource (UdpReceiver& udp) : udpSource (udp) {}

    /** The current global source (UDP receiver or System Audio). Updated by
        MainComponent whenever the global audio mode changes. */
    void setGlobalSource (IAudioSource* g) noexcept { global = g; }

    /** 0 = auto (use the global source); otherwise a specific plugin instance. */
    void setOverrideInstance (juce::uint32 id) noexcept { overrideId.store (id); }
    juce::uint32 getOverrideInstance() const noexcept   { return overrideId.load(); }

    // ===== IAudioSource =====
    float getLastRms() const noexcept override
    {
        const auto id = overrideId.load();
        return id != 0 ? udpSource.getInstanceRms (id) : (global ? global->getLastRms() : 0.0f);
    }
    float getLastPeak() const noexcept override
    {
        const auto id = overrideId.load();
        return id != 0 ? udpSource.getInstancePeak (id) : (global ? global->getLastPeak() : 0.0f);
    }
    float getLastSamplePeak() const noexcept override
    {
        const auto id = overrideId.load();
        // UDP instances transmit one peak value only → fall back to it as SP.
        return id != 0 ? udpSource.getInstancePeak (id)
                       : (global ? global->getLastSamplePeak() : 0.0f);
    }
    float getLastLufs() const noexcept override
    {
        const auto id = overrideId.load();
        return id != 0 ? udpSource.getInstanceLufs (id) : (global ? global->getLastLufs() : -100.0f);
    }
    float getBPM() const noexcept override
    {
        const auto id = overrideId.load();
        return id != 0 ? udpSource.getInstanceBpm (id) : (global ? global->getBPM() : 120.0f);
    }
    int getLastFft (std::vector<float>& out) const override
    {
        const auto id = overrideId.load();
        if (id != 0)            return udpSource.getInstanceFft (id, out);
        if (global != nullptr)  return global->getLastFft (out);
        out.clear(); return 0;
    }
    int getLastWaveform (std::vector<float>& left, std::vector<float>& right) const override
    {
        const auto id = overrideId.load();
        if (id != 0)            return udpSource.getInstanceWaveform (id, left, right);
        if (global != nullptr)  return global->getLastWaveform (left, right);
        left.clear(); right.clear(); return 0;
    }
    bool getMonoStream (std::uint64_t& ioTotal, std::vector<float>& out) const override
    {
        const auto id = overrideId.load();
        if (id != 0)            return udpSource.getInstanceStream (id, ioTotal, out);
        if (global != nullptr)  return global->getMonoStream (ioTotal, out);
        out.clear(); return false;
    }
    bool getHostSync (HostSyncInfo& out) const override
    {
        // Exact source first…
        const auto id = overrideId.load();
        if (id != 0 && udpSource.getInstanceHostSync (id, out)) return true;
        if (id == 0 && global != nullptr && global->getHostSync (out)) return true;

        // …then ANY instance that reports a transport. Only the CREATOR plugin
        // sends 'T'/'S': with the visual audio coming from System Audio or from a
        // Listener instance, the DAW sync impulses were invisible to the module —
        // that is why BPM sync "didn't work" in Synesthesia/Geometry.
        return udpSource.getAnyHostSync (out);
    }
    bool getMidiNotes (std::vector<MidiNote>& out) const override
    {
        const auto id = overrideId.load();
        if (id != 0)            return udpSource.getInstanceMidi (id, out);
        if (global != nullptr)  return global->getMidiNotes (out);
        out.clear(); return false;
    }
    int getLastCqt (std::vector<float>& out) const override
    {
        const auto id = overrideId.load();
        if (id != 0)            return udpSource.getInstanceCqt (id, out);
        if (global != nullptr)  return global->getLastCqt (out);
        out.clear(); return 0;
    }
    void setCqtNeeded (bool on) override
    {
        // Local system-audio source computes on demand; the UDP path is gated by
        // the per-instance 'N' needs mask instead (set via computeAndPushNeeds).
        if (global != nullptr) global->setCqtNeeded (on);
    }
    int getLastCqtStereo (std::vector<float>& outL, std::vector<float>& outR) const override
    {
        const auto id = overrideId.load();
        if (id != 0) { outL.clear(); outR.clear(); return 0; }   // UDP instances: mono CQT only
        return global != nullptr ? global->getLastCqtStereo (outL, outR) : 0;
    }
    void setCqtStereoNeeded (bool on) override
    {
        if (global != nullptr) global->setCqtStereoNeeded (on);
    }
    int getFftPacketsPerSecond() const noexcept override
    {
        const auto id = overrideId.load();
        return id != 0 ? udpSource.getInstanceFftPps (id)
                       : (global ? global->getFftPacketsPerSecond() : 0);
    }

private:
    UdpReceiver& udpSource;
    IAudioSource* global = nullptr;
    std::atomic<juce::uint32> overrideId { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModuleAudioSource)
};
