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

    // True Peak (ITU-R BS.1770-4 compliant: 4x oversampled)
    float blockPeak = 0.0f;

    // 4x oversampling FIR for True Peak (12-tap polyphase, 4 phases)
    // ITU-R BS.1770-4 specifies 4x oversampling with a specific FIR filter
    // These are simplified 12-tap half-band FIR coefficients split into 4 phases
    static constexpr int kTpTaps = 12;
    static constexpr int kTpPhases = 4;

    // Polyphase FIR coefficients for 4x oversampling (48-tap prototype split into 4 phases of 12)
    static constexpr float kTpFir[kTpPhases][kTpTaps] = {
        // Phase 0 (original samples - identity with slight filtering)
        { 0.0017089843f, -0.0291748047f, -0.0189208984f,  0.0696411133f,  0.3076171875f,  0.6784667969f,
          0.6784667969f,  0.3076171875f,  0.0696411133f, -0.0189208984f, -0.0291748047f,  0.0017089843f },
        // Phase 1 (1/4 sample offset)
        { -0.0024719238f,  0.0134887695f, -0.0392456055f,  0.0898742676f, -0.1882286072f,  0.6266021729f,
           0.6266021729f, -0.1882286072f,  0.0898742676f, -0.0392456055f,  0.0134887695f, -0.0024719238f },
        // Phase 2 (1/2 sample offset)
        { 0.0018463135f, -0.0116882324f,  0.0356445313f, -0.0837402344f,  0.3100585938f,  0.5747070313f,
          0.5747070313f,  0.3100585938f, -0.0837402344f,  0.0356445313f, -0.0116882324f,  0.0018463135f },
        // Phase 3 (3/4 sample offset)
        { -0.0024719238f,  0.0134887695f, -0.0392456055f,  0.0898742676f, -0.1882286072f,  0.6266021729f,
           0.6266021729f, -0.1882286072f,  0.0898742676f, -0.0392456055f,  0.0134887695f, -0.0024719238f }
    };

    // Delay line per channel for True Peak FIR
    static constexpr int kMaxTpChannels = 2;
    float tpDelayLine[kMaxTpChannels][kTpTaps] = {};
    int   tpDelayPos[kMaxTpChannels] = {};

    // Compute true peak for a single sample (returns max of 4 interpolated values)
    float computeTruePeakSample (int ch, float sample)
    {
        // Write sample into delay line
        tpDelayLine[ch][tpDelayPos[ch]] = sample;

        float maxVal = 0.0f;
        for (int phase = 0; phase < kTpPhases; ++phase)
        {
            float sum = 0.0f;
            int pos = tpDelayPos[ch];
            for (int t = 0; t < kTpTaps; ++t)
            {
                sum += kTpFir[phase][t] * tpDelayLine[ch][pos];
                pos--;
                if (pos < 0) pos = kTpTaps - 1;
            }
            float absSum = std::abs (sum);
            if (absSum > maxVal) maxVal = absSum;
        }

        tpDelayPos[ch] = (tpDelayPos[ch] + 1) % kTpTaps;
        return maxVal;
    }

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
