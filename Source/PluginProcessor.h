#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <cstring>
#include <array>

// ALTER Listener - VST3 (no UI)
// - RMS:  'ALTR' + float (~200 Hz)
// - FFT:  'ALTF' + N floats in [0..1], magnitudes only (N = kBinsOut = 2048)
// - Peak: 'ALTP' + float (linear true-peak, max|sample| per block)
// - LUFS: 'ALTL' + float (momentary LUFS, ITU-R BS.1770, 400 ms window)
// - Wave: 'ALTW' + interleaved L,R floats (stereo waveform, raw samples)

// ---------------------------------------------------------------
// Packet struktura pre ring buffer
// ---------------------------------------------------------------
struct Packet
{
    static constexpr int kMaxSize = 4 + 8192 * sizeof (float);
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

    bool acceptsMidi() const override                                  { return false; }
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

    void getStateInformation (juce::MemoryBlock&) override              {}
    void setStateInformation (const void*, int) override                {}

private:
    // ---------------------------------------------------------------
    // Sender thread - cita z ring bufferu a posiela UDP packety
    // ---------------------------------------------------------------
    class SenderThread : public juce::Thread
    {
    public:
        SenderThread (PacketRingBuffer& rb, juce::DatagramSocket& sock, int port)
            : juce::Thread ("ALTER Sender"),
              ringBuffer (rb), socket (sock), targetPort (port)
        {
            startThread (juce::Thread::Priority::low);
        }

        ~SenderThread() override { stopThread (500); }

    private:
        void run() override
        {
            Packet p;
            while (! threadShouldExit())
            {
                if (ringBuffer.pop (p))
                    socket.write ("127.0.0.1", targetPort, p.data, p.size);
                else
                    juce::Thread::sleep (1);
            }
        }

        PacketRingBuffer&     ringBuffer;
        juce::DatagramSocket& socket;
        const int             targetPort;
    };

    // UDP
    std::unique_ptr<juce::DatagramSocket> socket;
    PacketRingBuffer                      ringBuffer;
    std::unique_ptr<SenderThread>         senderThread;
    static constexpr int kPort = 7000;

    // Enqueue funkcie (volane z audio vlakna)
    void enqueueRmsPacket      (float rmsValue);
    void enqueuePeakPacket     (float peak);
    void enqueueLufsPacket     (float lufs);
    void enqueueWaveformPacket ();

    // RMS
    int    samplesPerPacket = 0;
    int    sampleCounter    = 0;
    float  rmsSmooth        = 0.0f;
    double sampleRateHz     = 48000.0;

    // True Peak
    float blockPeak = 0.0f;

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

    // FFT
    static constexpr int kFftOrder = 12;
    static constexpr int kFftSize  = 1 << kFftOrder;
    static constexpr int kBinsRaw  = kFftSize / 2;
    static constexpr int kBinsOut  = kBinsRaw;

    juce::dsp::FFT fft { kFftOrder };
    juce::HeapBlock<juce::dsp::Complex<float>> fftIn, fftOut;
    juce::AudioBuffer<float> fifo;
    int fifoWrite      = 0;
    int hopSamples     = kFftSize / 4;
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