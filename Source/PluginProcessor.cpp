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
    // Unique non-zero instance id (persisted via get/setStateInformation)
    do { instanceId = (juce::uint32) juce::Random::getSystemRandom().nextInt(); }
    while (instanceId == 0);
    rebuildInstanceName();

    socket = std::make_unique<juce::DatagramSocket>();
    socket->setEnablePortReuse (true);
    socket->setMulticastLoopbackEnabled (false);
    socket->bindToPort (0);

    senderThread = std::make_unique<SenderThread> (ringBuffer, *socket, kPort,
                                                   instanceId, instanceName, dataNeeds,
                                                   requestedFftOrder);
    senderThread->buildCqt = [this] (uint8_t* out) { return buildCqtPacket (out); };

    // Allocate to the MAX size once; the active window uses a subset (fftSize).
    fftIn .allocate (kMaxFftSize, true);
    fftOut.allocate (kMaxFftSize, true);
    fifo.setSize (1, kMaxFftSize);
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
    const juce::uint8 bit = (type == 'R') ? 1 : (type == 'P') ? 2 : (type == 'L') ? 4 : 0;
    if (bit != 0 && (dataNeeds.load() & bit) == 0)
        return;   // ALTER doesn't consume this type

    uint8_t buf[kAlterHeaderSize + sizeof (float)];
    writeHeader (buf, type);
    std::memcpy (buf + kAlterHeaderSize, &value, sizeof (float));
    ringBuffer.push (buf, (int) sizeof (buf));
}

void AlterListenerAudioProcessor::enqueueWaveformPacket()
{
    if ((dataNeeds.load() & 16) == 0) return;   // W not consumed

    constexpr int dataBytes  = kWaveformSize * 2 * (int) sizeof (float);
    constexpr int packetSize = kAlterHeaderSize + dataBytes;
    uint8_t packet[packetSize];

    writeHeader (packet, (uint8_t) 'W');

    // write interleaved L,R directly into the (unaligned) payload
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
    if ((dataNeeds.load() & 8) == 0) return;   // F (FFT) not consumed

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
}

// Runs on the SenderThread (worker). Reads the analyzer ring (filled on the audio
// thread; benign race) and writes a 'Q' packet. Returns the packet size.
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

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AlterListenerAudioProcessor();
}
