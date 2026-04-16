/*
  ==============================================================================

    WasapiLoopbackCapture.h
    WASAPI Loopback Mode - Windows-only System Audio Capture

    ✅ Funguje BEZ Stereo Mix!
    ✅ Funguje s USB/HDMI/Bluetooth audio!
    ✅ Zachytáva VŠETKO čo hrá na PC!

    ⚠️ Windows Only - This file will NOT compile on macOS/Linux

  ==============================================================================
*/

#pragma once

#if JUCE_WINDOWS  // JUCE macro for Windows detection

#include <JuceHeader.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <comdef.h>
#include <atomic>
#include <vector>

#pragma comment(lib, "ole32.lib")

class WasapiLoopbackCapture
{
public:
    WasapiLoopbackCapture()
    {
        DBG ("=== WASAPI Loopback: Initializing ===");
        
        HRESULT hr = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED (hr) && hr != RPC_E_CHANGED_MODE)
        {
            DBG ("WASAPI: CoInitialize FAILED!");
            return;
        }
        
        if (!openDefaultSpeakers())
        {
            DBG ("WASAPI: Failed to open speakers for loopback!");
            return;
        }
        
        startCaptureThread();
        DBG ("=== WASAPI Loopback: Started successfully! ===");
    }
    
    ~WasapiLoopbackCapture()
    {
        stopCaptureThread();
        
        if (captureClient)
            captureClient->Release();
        if (audioClient)
            audioClient->Release();
        if (device)
            device->Release();
        if (enumerator)
            enumerator->Release();
            
        CoUninitialize();
    }
    
    // Get latest audio samples
    bool getAudioData (std::vector<float>& samples)
    {
        const juce::ScopedLock sl (audioLock);
        if (audioData.empty())
            return false;
            
        samples = audioData;
        return true;
    }
    
    bool isActive() const { return isCapturing.load(); }

private:
    bool openDefaultSpeakers()
    {
        HRESULT hr;
        
        // Create device enumerator
        hr = CoCreateInstance (__uuidof(MMDeviceEnumerator),
                              nullptr,
                              CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              (void**)&enumerator);
        if (FAILED (hr))
        {
            DBG ("WASAPI: Failed to create device enumerator!");
            return false;
        }
        
        // Get DEFAULT SPEAKERS (eRender, NOT eCapture!)
        hr = enumerator->GetDefaultAudioEndpoint (eRender,    // ← OUTPUT device!
                                                   eConsole,
                                                   &device);
        if (FAILED (hr))
        {
            DBG ("WASAPI: No default audio output device!");
            return false;
        }
        
        // Log device name
        IPropertyStore* props = nullptr;
        if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &props)))
        {
            PROPVARIANT varName;
            PropVariantInit (&varName);
            
            PROPERTYKEY key = {
                {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14
            };
            
            if (SUCCEEDED (props->GetValue (key, &varName)))
            {
                DBG ("WASAPI: Opened device: " + juce::String (varName.pwszVal));
                PropVariantClear (&varName);
            }
            props->Release();
        }
        
        // Activate audio client
        hr = device->Activate (__uuidof(IAudioClient),
                              CLSCTX_ALL,
                              nullptr,
                              (void**)&audioClient);
        if (FAILED (hr))
        {
            DBG ("WASAPI: Failed to activate audio client!");
            return false;
        }
        
        // Get mix format
        WAVEFORMATEX* waveFormat = nullptr;
        hr = audioClient->GetMixFormat (&waveFormat);
        if (FAILED (hr))
        {
            DBG ("WASAPI: Failed to get mix format!");
            return false;
        }
        
        sampleRate = waveFormat->nSamplesPerSec;
        numChannels = waveFormat->nChannels;
        
        DBG ("WASAPI: Format: " + juce::String (sampleRate) + " Hz, " + 
             juce::String (numChannels) + " channels");
        
        // Initialize in LOOPBACK mode!
        hr = audioClient->Initialize (
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,  // ← LOOPBACK!
            10000000,  // 1 second buffer
            0,
            waveFormat,
            nullptr
        );
        
        CoTaskMemFree (waveFormat);
        
        if (FAILED (hr))
        {
            DBG ("WASAPI: Failed to initialize audio client! HR: " + juce::String::toHexString ((int)hr));
            return false;
        }
        
        // Get buffer size
        UINT32 bufferFrameCount;
        hr = audioClient->GetBufferSize (&bufferFrameCount);
        if (FAILED (hr))
        {
            DBG ("WASAPI: Failed to get buffer size!");
            return false;
        }
        
        DBG ("WASAPI: Buffer size: " + juce::String (bufferFrameCount) + " frames");
        
        // Create event for notifications
        captureEvent = CreateEvent (nullptr, FALSE, FALSE, nullptr);
        if (!captureEvent)
        {
            DBG ("WASAPI: Failed to create event!");
            return false;
        }
        
        hr = audioClient->SetEventHandle (captureEvent);
        if (FAILED (hr))
        {
            DBG ("WASAPI: Failed to set event handle!");
            return false;
        }
        
        // Get capture client
        hr = audioClient->GetService (__uuidof(IAudioCaptureClient),
                                      (void**)&captureClient);
        if (FAILED (hr))
        {
            DBG ("WASAPI: Failed to get capture client!");
            return false;
        }
        
        // Start audio client
        hr = audioClient->Start();
        if (FAILED (hr))
        {
            DBG ("WASAPI: Failed to start audio client!");
            return false;
        }
        
        DBG ("WASAPI: ✅ Loopback capture started successfully!");
        return true;
    }
    
    void startCaptureThread()
    {
        isCapturing.store (true);
        captureThread = std::thread ([this] { captureLoop(); });
    }
    
    void stopCaptureThread()
    {
        isCapturing.store (false);
        
        if (captureEvent)
            SetEvent (captureEvent);  // Wake up thread
            
        if (captureThread.joinable())
            captureThread.join();
            
        if (captureEvent)
        {
            CloseHandle (captureEvent);
            captureEvent = nullptr;
        }
    }
    
    void captureLoop()
    {
        juce::Thread::setCurrentThreadName ("WASAPI Loopback Capture");
        
        while (isCapturing.load())
        {
            DWORD waitResult = WaitForSingleObject (captureEvent, 1000);
            
            if (waitResult != WAIT_OBJECT_0)
                continue;
                
            if (!isCapturing.load())
                break;
                
            processAudioBuffer();
        }
    }
    
    void processAudioBuffer()
    {
        HRESULT hr;
        BYTE* data = nullptr;
        UINT32 numFramesAvailable = 0;
        DWORD flags = 0;

        static int debugCounter = 0;

        while (true)
        {
            hr = captureClient->GetBuffer (&data,
                                          &numFramesAvailable,
                                          &flags,
                                          nullptr,
                                          nullptr);

            if (hr == AUDCLNT_S_BUFFER_EMPTY)
                break;

            if (FAILED (hr))
            {
                DBG ("WASAPI: GetBuffer failed!");
                break;
            }

            // DEBUG: Log every 100 buffers
            if (++debugCounter % 100 == 0)
            {
                DBG ("WASAPI: Got " + juce::String (numFramesAvailable) + " frames, flags=" + 
                     juce::String ((int)flags) + ", silent=" + juce::String ((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? "YES" : "NO"));
            }

            if (numFramesAvailable == 0)
            {
                captureClient->ReleaseBuffer (0);
                break;
            }

            // Convert to float
            std::vector<float> samples;
            samples.reserve (numFramesAvailable);

            float* floatData = (float*)data;
            bool isSilent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

            for (UINT32 i = 0; i < numFramesAvailable; ++i)
            {
                float sample = 0.0f;

                if (!isSilent)
                {
                    // Mix to mono
                    for (int ch = 0; ch < numChannels; ++ch)
                        sample += floatData[i * numChannels + ch];
                    sample /= (float)numChannels;
                }

                samples.push_back (sample);
            }

            // Store samples
            {
                const juce::ScopedLock sl (audioLock);
                audioData = std::move (samples);
            }

            hr = captureClient->ReleaseBuffer (numFramesAvailable);
            if (FAILED (hr))
            {
                DBG ("WASAPI: ReleaseBuffer failed!");
                break;
            }
        }
    }

private:
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    
    HANDLE captureEvent = nullptr;
    std::thread captureThread;
    std::atomic<bool> isCapturing {false};
    
    int sampleRate = 48000;
    int numChannels = 2;
    
    juce::CriticalSection audioLock;
    std::vector<float> audioData;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WasapiLoopbackCapture)
};

#endif  // JUCE_WINDOWS
