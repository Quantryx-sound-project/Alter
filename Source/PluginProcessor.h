#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <cstring>
#include <array>

// ALTER Listener - VST3 (no UI)
// - RMS:  'ALTR' + float (~60 Hz)
// - FFT:  'ALTF' + N floats in [0..1], magnitudes only (N = kBinsOut = 2048)
// - Peak: 'ALTP' + float (linear true-peak, max|sample| per block)
// - LUFS: 'ALTL' + float (momentary LUFS, ITU-R BS.1770, 400 ms window)
// - Wave: 'ALTW' + interleaved L,R floats (stereo waveform, raw samples)

class AlterListenerAudioProcessor : public juce::AudioProcessor
{
public:
    AlterListenerAudioProcessor();
    ~AlterListenerAudioProcessor() override = default;

    // AudioProcessor overrides
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

    // prevent abstract
    bool acceptsMidi() const override                                  { return false; }
    bool producesMidi() const override                                 { return false; }
    bool isMidiEffect() const override                                 { return false; }
    double getTailLengthSeconds() const override                        { return 0.0; }
    bool supportsDoublePrecisionProcessing() const override             { return false; }
   #if JUCE_MAJOR_VERSION >= 7
    juce::AudioProcessorParameter* getBypassParameter() const override { return nullptr; }
   #endif

    // Programs
    int getNumPrograms() override                                       { return 1; }
    int getCurrentProgram() override                                    { return 0; }
    void setCurrentProgram (int) override                               {}
    const juce::String getProgramName (int) override                    { return {}; }
    void changeProgramName (int, const juce::String&) override          {}

    // Persist
    void getStateInformation (juce::MemoryBlock&) override              {}
    void setStateInformation (const void*, int) override                {}

private:
    // UDP
    std::unique_ptr<juce::DatagramSocket> socket;
    static constexpr int kPort = 7000;
    void sendRmsPacket (float rmsValue);
    void sendPeakPacket (float peak);
    void sendLufsPacket (float lufs);
    void sendWaveformPacket();

    // RMS
    int    samplesPerPacket = 0;  // ~60 Hz sending
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

    Biquad kWeightStage1[2], kWeightStage2[2];  // per-channel (L, R)

    // LUFS momentary (400 ms sliding window of K-weighted squared samples)
    std::vector<float> lufsBuffer;
    int  lufsBufferSize  = 0;
    int  lufsWritePos    = 0;
    double lufsRunningSum = 0.0;

    void computeKWeightCoeffs (double sampleRate);

    // FFT
    static constexpr int kFftOrder = 12;
    static constexpr int kFftSize  = 1 << kFftOrder;
    static constexpr int kBinsRaw  = kFftSize / 2; // 2048
    static constexpr int kBinsOut  = kBinsRaw;

    juce::dsp::FFT fft { kFftOrder };
    juce::HeapBlock<juce::dsp::Complex<float>> fftIn, fftOut; // complex arrays
    juce::AudioBuffer<float> fifo;            // mono ring kFftSize
    int fifoWrite = 0;
    int hopSamples = kFftSize / 4;            // 75% overlap (hop = 1024 at 48 kHz)
    int hopAccumulator = 0;

    // dB floor for mapping to 0..1 (must match app)
    static constexpr float kDbFloor = -90.0f;

    // Stereo waveform ring buffer (for ALTW packets)
    static constexpr int kWaveformSize = 4096;  // samples per channel
    std::array<float, kWaveformSize> waveformL {};
    std::array<float, kWaveformSize> waveformR {};
    int waveformWrite = 0;

    // helpers
    void pushSamplesToFifo (const float* samples, int numSamples);
    void performFftAndSend();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AlterListenerAudioProcessor)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();