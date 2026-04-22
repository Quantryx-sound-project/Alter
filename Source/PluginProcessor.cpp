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
    socket = std::make_unique<juce::DatagramSocket>();
    socket->setEnablePortReuse (true);
    socket->setMulticastLoopbackEnabled (false);
    socket->bindToPort (0);

    // Spusti sender thread
    senderThread = std::make_unique<SenderThread> (ringBuffer, *socket, kPort);

    fftIn .allocate (kFftSize, true);
    fftOut.allocate (kFftSize, true);
    fifo.setSize (1, kFftSize);
    fifo.clear();
}

AlterListenerAudioProcessor::~AlterListenerAudioProcessor()
{
    // Sender thread treba zastavit pred zrusenim socketu
    senderThread.reset();
    socket.reset();
}

void AlterListenerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    sampleRateHz     = sampleRate;
    samplesPerPacket = (int) juce::jmax (1.0, sampleRate / 200.0);
    sampleCounter    = 0;
    rmsSmooth        = 0.0f;
    blockPeak        = 0.0f;

    // Reset True Peak delay lines
    for (int ch = 0; ch < (int) kMaxTpChannels; ++ch)
    {
        std::fill (std::begin (tpDelayLine[ch]), std::end (tpDelayLine[ch]), 0.0f);
        tpDelayPos[ch] = 0;
    }

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
void AlterListenerAudioProcessor::enqueueRmsPacket (float rms)
{
    uint8_t buf[8];
    buf[0]='A'; buf[1]='L'; buf[2]='T'; buf[3]='R';
    std::memcpy (buf + 4, &rms, sizeof (float));
    ringBuffer.push (buf, 8);
}

void AlterListenerAudioProcessor::enqueuePeakPacket (float peak)
{
    uint8_t buf[8];
    buf[0]='A'; buf[1]='L'; buf[2]='T'; buf[3]='P';
    std::memcpy (buf + 4, &peak, sizeof (float));
    ringBuffer.push (buf, 8);
}

void AlterListenerAudioProcessor::enqueueLufsPacket (float lufs)
{
    uint8_t buf[8];
    buf[0]='A'; buf[1]='L'; buf[2]='T'; buf[3]='L';
    std::memcpy (buf + 4, &lufs, sizeof (float));
    ringBuffer.push (buf, 8);
}

void AlterListenerAudioProcessor::enqueueWaveformPacket()
{
    constexpr int dataBytes  = kWaveformSize * 2 * (int) sizeof (float);
    constexpr int packetSize = 4 + dataBytes;
    alignas(4) uint8_t packet[packetSize];

    packet[0]='A'; packet[1]='L'; packet[2]='T'; packet[3]='W';

    float* dst = reinterpret_cast<float*> (packet + 4);
    for (int i = 0; i < kWaveformSize; ++i)
    {
        const int idx = (waveformWrite + i) % kWaveformSize;
        dst[i * 2 + 0] = waveformL[(size_t) idx];
        dst[i * 2 + 1] = waveformR[(size_t) idx];
    }

    ringBuffer.push (packet, packetSize);
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

void AlterListenerAudioProcessor::performFftAndEnqueue()
{
    auto* rd = fifo.getReadPointer (0);
    for (int i = 0; i < kFftSize; ++i)
    {
        const float win = 0.5f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi
                              * (float) i / (float) (kFftSize - 1)));
        const int   idx = (fifoWrite + i) % kFftSize;
        fftIn[i].real (rd[idx] * win);
        fftIn[i].imag (0.0f);
    }

    fft.perform (fftIn, fftOut, false);

    const float norm = 4.0f / (float) kFftSize;

    constexpr int packetSize = 4 + kBinsOut * (int) sizeof (float);
    alignas(4) uint8_t packet[packetSize];
    packet[0]='A'; packet[1]='L'; packet[2]='T'; packet[3]='F';

    float* dst = reinterpret_cast<float*> (packet + 4);
    for (int k = 0; k < kBinsOut; ++k)
    {
        const float re  = fftOut[k].real();
        const float im  = fftOut[k].imag();
        const float mag = std::sqrt (re * re + im * im) * norm;

        float dB = 20.0f * std::log10 (juce::jmax (mag, 1.0e-12f));
        dB = juce::jlimit (kDbFloor, 0.0f, dB);
        dst[k] = juce::jlimit (0.0f, 1.0f, (dB - kDbFloor) / (-kDbFloor));
    }

    ringBuffer.push (packet, packetSize);
}

void AlterListenerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                 juce::MidiBuffer& midi)
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
                if (absVal > blockPeak) blockPeak = absVal;
            }
        }
        const float rms = (float) std::sqrt (sum2 / (double) (numSamples * juce::jmax (1, numCh)));
        const float A = 0.1f;
        rmsSmooth = (1.0f - A) * rmsSmooth + A * rms;

        sampleCounter += numSamples;
        if (sampleCounter >= samplesPerPacket)
        {
            sampleCounter = 0;
            enqueueRmsPacket  (juce::jlimit (0.0f, 1.0f, rmsSmooth));
            enqueuePeakPacket (blockPeak);
            blockPeak = 0.0f;

            const double meanSq = (lufsBufferSize > 0)
                                ? lufsRunningSum / (double) lufsBufferSize : 0.0;
            const float lufs = (meanSq > 1.0e-10)
                             ? (float) (-0.691 + 10.0 * std::log10 (meanSq))
                             : -100.0f;
            enqueueLufsPacket (lufs);
        }
    }

    // ---- LUFS K-weighted accumulation (ITU-R BS.1770 - kanaly sa scitaju, nedelime) ----
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
        // ITU-R BS.1770: kanaly sa scitaju priamo, NEpriemuerujeme
        // ODSTRANENY RIADOK: if (chCount > 0) kSqSum /= (float) chCount;

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

    // ---- Mono mix pre FFT ----
    for (int i = 0; i < numSamples; ++i)
    {
        float s = 0.0f;
        for (int ch = 0; ch < numCh; ++ch)
            s += buffer.getReadPointer (ch)[i];
        s *= (1.0f / juce::jmax (1, numCh));
        pushSamplesToFifo (&s, 1);
    }

    // ---- FFT + waveform na hop ----
    hopAccumulator += numSamples;
    while (hopAccumulator >= hopSamples)
    {
        performFftAndEnqueue();
        enqueueWaveformPacket();
        hopAccumulator -= hopSamples;
    }
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

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AlterListenerAudioProcessor();
}
