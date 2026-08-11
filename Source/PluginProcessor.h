#pragma once
#include <JuceHeader.h>
#include "TruePeakMeter.h"
#include "ConstantQ.h"
#include <atomic>
#include <cstring>
#include <array>
#include <map>

// ALTER Listener - VST3 (no UI)
//
// Protocol v2 (multi-instance):
//   Every packet:  'A','L','T','2'  +  uint8 type  +  uint32 instanceId  +  payload
//   Types:
//     'R' : float                      RMS (~200 Hz)
//     'P' : float                      True peak (linear, 4x-oversampled inter-sample peak per window)
//     'L' : float                      Momentary LUFS (ITU-R BS.1770, 400 ms)
//     'F' : N floats in [0..1]         FFT magnitudes (N = binsOut, 2048..8192)
//     'W' : interleaved L,R floats     Stereo waveform (raw samples)
//     'I' : UTF-8 string               Instance announce (name), sent ~2x/s
//     'M' : uint8 count + count×(note,vel)  Held MIDI notes ("ALTM"; only while MIDI present)
//
//   Control packets ALTER -> plugin (received by the sender thread):
//     'N' : uint8                      data-needs mask (which packet types to send)
//     'B' : uint8                      FFT order (12..14 → 4096..16384-pt → 2048..8192 bins)
//
// The legacy v1 protocol ('ALTR'/'ALTF'/...) is still understood by the
// receiver in ALTER, so old plugin builds keep working (shown as "Legacy").

static constexpr int kAlterHeaderSize = 9; // 4 magic + 1 type + 4 instanceId

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
// Pise audio vlakno, cita sender thread
// ---------------------------------------------------------------
class PacketRingBuffer
{
public:
    static constexpr int kCapacity = 64;

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

class AlterListenerAudioProcessor : public juce::AudioProcessor
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

    bool hasEditor() const override                                   { return false; }
    juce::AudioProcessorEditor* createEditor() override               { return nullptr; }

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

    // Persist the instance id so the same plugin slot keeps its identity
    // across DAW project reloads.
    void getStateInformation (juce::MemoryBlock& dest) override
    {
        dest.append (&instanceId, sizeof (instanceId));
    }

    void setStateInformation (const void* data, int size) override
    {
        if (size >= (int) sizeof (juce::uint32) && data != nullptr)
        {
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

private:
    // ---------------------------------------------------------------
    // Sender thread - cita z ring bufferu a posiela UDP packety.
    // Navyse kazdych ~500 ms posiela 'I' announce packet s menom instancie.
    // ---------------------------------------------------------------
    class SenderThread : public juce::Thread
    {
    public:
        SenderThread (PacketRingBuffer& rb, juce::DatagramSocket& sock, int port,
                      juce::uint32 instId, const juce::String& instName,
                      std::atomic<juce::uint8>& needs, std::atomic<int>& fftOrder)
            : juce::Thread ("ALTER Sender"),
              ringBuffer (rb), socket (sock), targetPort (port),
              dataNeeds (needs), reqFftOrder (fftOrder)
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

                // receive the data-needs mask ('N') from ALTER (non-blocking)
                {
                    uint8_t rbuf[64];
                    juce::String ip; int prt = 0;
                    int n = socket.read (rbuf, (int) sizeof (rbuf), false, ip, prt);
                    while (n >= 10)
                    {
                        if (rbuf[0]=='A'&&rbuf[1]=='L'&&rbuf[2]=='T'&&rbuf[3]=='2')
                        {
                            if (rbuf[4]=='N')                        // data-needs mask
                                dataNeeds.store (rbuf[9]);
                            else if (rbuf[4]=='B')                   // FFT order (resolution)
                                reqFftOrder.store ((int) rbuf[9]);
                        }
                        n = socket.read (rbuf, (int) sizeof (rbuf), false, ip, prt);
                    }
                }

                const auto now = juce::Time::getMillisecondCounter();
                if (now - lastAnnounceMs >= 500)
                {
                    lastAnnounceMs = now;
                    sendAnnounce();
                }

                // Constant-Q computed HERE (worker thread), ~30 Hz, only on demand —
                // keeps the expensive 32768-pt FFT off the audio thread.
                if ((dataNeeds.load() & 32) && buildCqt && now - lastCqtMs >= 33)
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
        // Set by the processor: fills a 'Q' packet from the analyzer, returns size.
        std::function<int (uint8_t*)> buildCqt;

    private:
        juce::uint32 lastCqtMs = 0;

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
        std::atomic<juce::uint8>& dataNeeds;
        std::atomic<int>&     reqFftOrder;

        juce::CriticalSection identityLock;
        juce::uint32          id = 0;
        std::string           nameUtf8;
    };

    // UDP
    std::unique_ptr<juce::DatagramSocket> socket;
    PacketRingBuffer                      ringBuffer;
    // data-needs mask from ALTER ('N'): R=1,P=2,L=4,F=8,W=16. Default = all.
    std::atomic<juce::uint8>              dataNeeds { 0x1F };
    std::unique_ptr<SenderThread>         senderThread;
    static constexpr int kPort = 7000;

    // ---------------------------------------------------------------
    // Instance identity
    //
    // Name = track name (from the DAW, when the host reports it via
    // updateTrackProperties) + ordinal for multiple plugins on one track:
    //   "Bass #1", "Bass #2", "Track 3 #1", ...
    // ---------------------------------------------------------------
    juce::uint32 instanceId   = 0;
    juce::String instanceName;
    juce::String trackName;

public:
    void updateTrackProperties (const TrackProperties& props) override
    {
        // JUCE 7: props.name is String;  JUCE 8: std::optional<String>
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
        // ordinal per track name within this process (1,2,3,...)
        static juce::CriticalSection countLock;
        static std::map<juce::String, int> perTrackCount;

        const juce::String track = trackName.isNotEmpty() ? trackName : "Track";

        if (claimedTrack != track)
        {
            const juce::ScopedLock sl (countLock);
            claimedTrack   = track;
            claimedOrdinal = ++perTrackCount[track];
        }

        instanceName = "Listener " + track + " " + juce::String (claimedOrdinal);
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

    // Constant-Q (multi-resolution) — computed on the SenderThread, only on demand.
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

    std::atomic<int> requestedFftOrder { kFftOrderMin };   // set by SenderThread on 'B'
    void applyFftOrderIfChanged();                          // audio thread

    juce::dsp::FFT fft { kFftOrderMin };
    juce::HeapBlock<juce::dsp::Complex<float>> fftIn, fftOut;
    juce::AudioBuffer<float> fifo;
    int fifoWrite      = 0;
    int hopSamples     = (1 << kFftOrderMin) / 4;
    int hopAccumulator = 0;

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

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();
