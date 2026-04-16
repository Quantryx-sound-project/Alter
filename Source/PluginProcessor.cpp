#include "PluginProcessor.h"
#include <cmath>
#include <vector>

AlterListenerAudioProcessor::AlterListenerAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor (BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
#endif
{
    // UDP sender
    socket = std::make_unique<juce::DatagramSocket>();
    socket->setEnablePortReuse (true);
    socket->setMulticastLoopbackEnabled (false);
    socket->bindToPort (0);

    // FFT buffers
    fftIn .allocate (kFftSize, true);
    fftOut.allocate (kFftSize, true);

    fifo.setSize (1, kFftSize);
    fifo.clear();
}

void AlterListenerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    sampleRateHz   = sampleRate;
    samplesPerPacket = (int) juce::jmax (1.0, sampleRate / 200.0);
    sampleCounter  = 0;
    rmsSmooth      = 0.0f;
    blockPeak      = 0.0f;

    // K-weighting coefficients (ITU-R BS.1770)
    computeKWeightCoeffs (sampleRate);

    // LUFS 400 ms sliding window
    lufsBufferSize = (int) (sampleRate * 0.4);
    lufsBuffer.assign ((size_t) lufsBufferSize, 0.0f);
    lufsWritePos   = 0;
    lufsRunningSum = 0.0;

    // Clear waveform FIFO
    waveformL.fill (0.0f);
    waveformR.fill (0.0f);
    waveformWrite = 0;
}

void AlterListenerAudioProcessor::sendRmsPacket (float rms)
{
    juce::MemoryOutputStream mo;
    mo.write ("ALTR", 4);
    mo.writeFloat (rms);
    socket->write ("127.0.0.1", kPort, mo.getData(), (int) mo.getDataSize());
}

void AlterListenerAudioProcessor::sendPeakPacket (float peak)
{
    juce::MemoryOutputStream mo;
    mo.write ("ALTP", 4);
    mo.writeFloat (peak);
    socket->write ("127.0.0.1", kPort, mo.getData(), (int) mo.getDataSize());
}

void AlterListenerAudioProcessor::sendLufsPacket (float lufs)
{
    juce::MemoryOutputStream mo;
    mo.write ("ALTL", 4);
    mo.writeFloat (lufs);
    socket->write ("127.0.0.1", kPort, mo.getData(), (int) mo.getDataSize());
}

void AlterListenerAudioProcessor::sendWaveformPacket()
{
    // ALTW: interleaved stereo samples L0,R0, L1,R1, ... (oldest -> newest)
    constexpr int dataBytes  = kWaveformSize * 2 * (int) sizeof (float);
    constexpr int packetSize = 4 + dataBytes;
    alignas(4) uint8_t packet[packetSize];

    packet[0] = 'A'; packet[1] = 'L'; packet[2] = 'T'; packet[3] = 'W';

    float* dst = reinterpret_cast<float*> (packet + 4);
    for (int i = 0; i < kWaveformSize; ++i)
    {
        const int idx = (waveformWrite + i) % kWaveformSize;
        dst[i * 2 + 0] = waveformL[(size_t) idx];
        dst[i * 2 + 1] = waveformR[(size_t) idx];
    }

    socket->write ("127.0.0.1", kPort, packet, packetSize);
}

void AlterListenerAudioProcessor::pushSamplesToFifo (const float* samples, int numSamples)
{
    auto* mono = fifo.getWritePointer (0);
    for (int i = 0; i < numSamples; ++i)
    {
        mono[fifoWrite] = samples[i];
        fifoWrite = (fifoWrite + 1) % kFftSize;
    }
}

void AlterListenerAudioProcessor::performFftAndSend()
{
    // copy last kFftSize samples from FIFO to fftIn and apply Hann
    auto* rd = fifo.getReadPointer (0);
    for (int i = 0; i < kFftSize; ++i)
    {
        const float win = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi
                              * (float) i / (float) (kFftSize - 1)));
        const int idx = (fifoWrite + i) % kFftSize; // oldest -> newest
        const float s = rd[idx];
        fftIn[i].real (s * win);
        fftIn[i].imag (0.0f);
    }

    // FFT -> fftOut
    fft.perform (fftIn, fftOut, false);

    // magnitude (0..Nyquist) -> map to 0..1 via dB floor
    // Phases removed - oscillator uses direct waveform (ALTW) now
    const float norm = 4.0f / (float) kFftSize; // Hann coherent gain ~0.5 -> 4/N

    juce::MemoryOutputStream mo;
    mo.write ("ALTF", 4);

    for (int k = 0; k < kBinsOut; ++k)
    {
        const float re = fftOut[k].real();
        const float im = fftOut[k].imag();
        const float mag = std::sqrt (re * re + im * im) * norm;

        float dB = 20.0f * std::log10 (juce::jmax (mag, 1.0e-12f));
        dB = juce::jlimit (kDbFloor, 0.0f, dB);
        const float n = (dB - kDbFloor) / (-kDbFloor);

        mo.writeFloat (juce::jlimit (0.0f, 1.0f, n));
    }

    socket->write ("127.0.0.1", kPort, mo.getData(), (int) mo.getDataSize());
}

void AlterListenerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused (midi);

    const int numSamples = buffer.getNumSamples();
    const int numCh      = buffer.getNumChannels();

    // ---- RMS + True Peak ----
    {
        double sum2 = 0.0;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* d = buffer.getReadPointer (ch);
            for (int i = 0; i < numSamples; ++i)
            {
                sum2 += (double) d[i] * (double) d[i];
                const float absVal = std::abs (d[i]);
                if (absVal > blockPeak)
                    blockPeak = absVal;
            }
        }
        const double mean2 = sum2 / (double) (numSamples * juce::jmax (1, numCh));
        const float  rms   = (float) std::sqrt (mean2);

        // light RMS smoothing
        const float A = 0.1f, B = 1.0f - A;
        rmsSmooth = B * rmsSmooth + A * rms;

        sampleCounter += numSamples;
        if (sampleCounter >= samplesPerPacket)
        {
            sampleCounter = 0;
            sendRmsPacket (juce::jlimit (0.0f, 1.0f, rmsSmooth));

            // True Peak
            sendPeakPacket (blockPeak);
            blockPeak = 0.0f;

            // LUFS momentary: -0.691 + 10*log10(mean_square)
            const double meanSq = (lufsBufferSize > 0)
                                ? lufsRunningSum / (double) lufsBufferSize
                                : 0.0;
            float lufs = -100.0f;
            if (meanSq > 1.0e-10)
                lufs = (float) (-0.691 + 10.0 * std::log10 (meanSq));
            sendLufsPacket (lufs);
        }
    }

    // ---- LUFS: K-weighted mean-square accumulation ----
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
            if (chCount > 0)
                kSqSum /= (float) chCount;

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

    // ---- Push stereo samples to waveform FIFO (before mono mix) ----
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

    // mix to mono into FIFO (spectrum analyzer still needs this)
    for (int i = 0; i < numSamples; ++i)
    {
        float s = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            s += buffer.getReadPointer (ch)[i];
        s *= (1.0f / juce::jmax (1, numCh));
        pushSamplesToFifo (&s, 1);
    }

    // run FFT + waveform on hop
    hopAccumulator += numSamples;
    while (hopAccumulator >= hopSamples)
    {
        performFftAndSend();
        sendWaveformPacket();
        hopAccumulator -= hopSamples;
    }
}

// K-weighting filter coefficients (ITU-R BS.1770-4)
// Stage 1: High-shelf pre-filter (~+4 dB above 1.5 kHz)
// Stage 2: RLB high-pass (~38 Hz)
void AlterListenerAudioProcessor::computeKWeightCoeffs (double fs)
{
    const double pi = juce::MathConstants<double>::pi;

    // Stage 1: High-shelf (head-related transfer function)
    {
        const double f0 = 1681.974450955533;
        const double G  = 3.999843853973347; // dB
        const double Q  = 0.7071752369554196;

        const double A     = std::pow (10.0, G / 40.0);
        const double w0    = 2.0 * pi * f0 / fs;
        const double sinw  = std::sin (w0);
        const double cosw  = std::cos (w0);
        const double alpha = sinw / (2.0 * Q);
        const double sqA   = std::sqrt (A);

        const double a0 =        (A + 1.0) - (A - 1.0) * cosw + 2.0 * sqA * alpha;
        const double b0 =    A * ((A + 1.0) + (A - 1.0) * cosw + 2.0 * sqA * alpha);
        const double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw);
        const double b2 =    A * ((A + 1.0) + (A - 1.0) * cosw - 2.0 * sqA * alpha);
        const double a1 =    2.0 * ((A - 1.0) - (A + 1.0) * cosw);
        const double a2 =        (A + 1.0) - (A - 1.0) * cosw - 2.0 * sqA * alpha;

        for (int ch = 0; ch < 2; ++ch)
        {
            kWeightStage1[ch].b0 = (float) (b0 / a0);
            kWeightStage1[ch].b1 = (float) (b1 / a0);
            kWeightStage1[ch].b2 = (float) (b2 / a0);
            kWeightStage1[ch].a1 = (float) (a1 / a0);
            kWeightStage1[ch].a2 = (float) (a2 / a0);
            kWeightStage1[ch].reset();
        }
    }

    // Stage 2: High-pass (revised low-frequency weighting)
    {
        const double f0 = 38.13547087602444;
        const double Q  = 0.5003270373238773;

        const double w0    = 2.0 * pi * f0 / fs;
        const double sinw  = std::sin (w0);
        const double cosw  = std::cos (w0);
        const double alpha = sinw / (2.0 * Q);

        const double a0 =  1.0 + alpha;
        const double b0 =  (1.0 + cosw) / 2.0;
        const double b1 = -(1.0 + cosw);
        const double b2 =  (1.0 + cosw) / 2.0;
        const double a1 = -2.0 * cosw;
        const double a2 =  1.0 - alpha;

        for (int ch = 0; ch < 2; ++ch)
        {
            kWeightStage2[ch].b0 = (float) (b0 / a0);
            kWeightStage2[ch].b1 = (float) (b1 / a0);
            kWeightStage2[ch].b2 = (float) (b2 / a0);
            kWeightStage2[ch].a1 = (float) (a1 / a0);
            kWeightStage2[ch].a2 = (float) (a2 / a0);
            kWeightStage2[ch].reset();
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AlterListenerAudioProcessor();
}