/*
  ==============================================================================

    MacSystemAudioCapture.mm
    ScreenCaptureKit system-audio backend for macOS (Objective-C++, Apple-only).

  ==============================================================================
*/

#include "MacSystemAudioCapture.h"

#if __APPLE__

#import <Foundation/Foundation.h>
#include <mutex>
#include <atomic>
#include <algorithm>

#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
 #import <ScreenCaptureKit/ScreenCaptureKit.h>
 #import <CoreMedia/CoreMedia.h>
 #import <CoreVideo/CoreVideo.h>     // kCVPixelFormatType_32BGRA
 #define ALTER_HAS_SCK 1
#endif

// Core Audio process taps (macOS 14.2+): PURE system-audio capture with its own
// "System Audio Recording Only" permission — far more reliable than the SCK
// audio stream (which needs a live screen capture to pump audio at all).
#if __has_include(<CoreAudio/AudioHardwareTapping.h>) && __has_include(<CoreAudio/CATapDescription.h>)
 #import <CoreAudio/CoreAudio.h>
 #import <CoreAudio/AudioHardwareTapping.h>
 #import <CoreAudio/CATapDescription.h>
 #define ALTER_HAS_CATAP 1
#endif

#if ALTER_HAS_SCK

// Shared C++ state between the Objective-C tap and the C++ wrapper.
struct AlterSharedAudio
{
    std::mutex          mtx;
    std::vector<float>  pending;          // accumulated mono samples
    std::atomic<bool>   active { false };
};

API_AVAILABLE(macos(13.0))
@interface AlterAudioTap : NSObject <SCStreamOutput, SCStreamDelegate>
{
@public
    AlterSharedAudio* shared;
}
@end

@implementation AlterAudioTap

- (void)stream:(SCStream *)stream
   didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
   ofType:(SCStreamOutputType)type
{
    (void) stream;
    if (type != SCStreamOutputTypeAudio || shared == nullptr) return;
    if (! CMSampleBufferIsValid (sampleBuffer)) return;

    const CMAudioFormatDescriptionRef fmt =
        (CMAudioFormatDescriptionRef) CMSampleBufferGetFormatDescription (sampleBuffer);
    if (fmt == nullptr) return;
    const AudioStreamBasicDescription* asbd =
        CMAudioFormatDescriptionGetStreamBasicDescription (fmt);
    if (asbd == nullptr) return;

    const int  channels       = (int) asbd->mChannelsPerFrame;
    const bool isFloat        = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    const bool nonInterleaved = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    if (! isFloat || channels <= 0) return;

    AudioBufferList abl;
    CMBlockBufferRef block = nullptr;
    const OSStatus st = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer (
        sampleBuffer, nullptr, &abl, sizeof (abl), nullptr, nullptr,
        kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);
    if (st != noErr) { if (block) CFRelease (block); return; }

    std::vector<float> stereo;   // interleaved L,R

    if (nonInterleaved && abl.mNumberBuffers >= 1)
    {
        const int frames = (int) (abl.mBuffers[0].mDataByteSize / sizeof (float));
        const float* L = (const float*) abl.mBuffers[0].mData;
        const float* R = (abl.mNumberBuffers >= 2) ? (const float*) abl.mBuffers[1].mData : L;
        stereo.resize ((size_t) frames * 2);
        for (int i = 0; i < frames; ++i) { stereo[(size_t)(2*i)] = L[i]; stereo[(size_t)(2*i+1)] = R[i]; }
    }
    else if (abl.mNumberBuffers >= 1)
    {
        const float* d  = (const float*) abl.mBuffers[0].mData;
        const int total = (int) (abl.mBuffers[0].mDataByteSize / sizeof (float));
        const int frames = total / channels;
        stereo.resize ((size_t) frames * 2);
        for (int i = 0; i < frames; ++i)
        {
            const float L = d[i * channels + 0];
            const float R = (channels > 1) ? d[i * channels + 1] : L;
            stereo[(size_t)(2*i)] = L; stereo[(size_t)(2*i+1)] = R;
        }
    }

    if (block) CFRelease (block);

    if (! stereo.empty())
    {
        std::lock_guard<std::mutex> lk (shared->mtx);
        if (shared->pending.size() < 48000 * 2 * 4)   // ~4 s safety cap (2 ch)
            shared->pending.insert (shared->pending.end(), stereo.begin(), stereo.end());
    }
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    (void) stream; (void) error;
    if (shared) shared->active = false;
}

@end

struct MacSystemAudioCapture::Impl
{
    AlterSharedAudio shared;
    id stream = nil;          // SCStream*
    id tap    = nil;          // AlterAudioTap*
    dispatch_queue_t queue = nullptr;
    int sampleRate = 48000;   // actual capture rate (CATap follows the device)

   #if ALTER_HAS_CATAP
    // Core Audio process-tap backend (macOS 14.2+)
    AudioObjectID       caTap    = kAudioObjectUnknown;
    AudioObjectID       caAggDev = kAudioObjectUnknown;
    AudioDeviceIOProcID caProcID = nullptr;
    bool                caActive = false;

    bool startCoreAudioTap();
    void stopCoreAudioTap();
   #endif
};

#if ALTER_HAS_CATAP
bool MacSystemAudioCapture::Impl::startCoreAudioTap()
{
    if (@available (macOS 14.2, *))
    {
        // 1) global stereo tap of ALL system audio (mixed), excluding nothing —
        //    Alter itself produces no audio output, so no self-echo is possible.
        CATapDescription* desc =
            [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses: @[]];
        desc.name = @"Alter System Audio Tap";

        OSStatus err = AudioHardwareCreateProcessTap (desc, &caTap);
        if (err != noErr || caTap == kAudioObjectUnknown)
        {
            // Most commonly TCC: System Settings → Privacy & Security →
            // Screen & System Audio Recording → "System Audio Recording Only".
            NSLog (@"[Alter] CATap: AudioHardwareCreateProcessTap failed (%d)", (int) err);
            return false;
        }

        // 2) default output device UID → aggregate main sub-device
        AudioObjectID outDev = kAudioObjectUnknown;
        UInt32 sz = sizeof (outDev);
        AudioObjectPropertyAddress addr { kAudioHardwarePropertyDefaultOutputDevice,
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain };
        err = AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, &sz, &outDev);

        CFStringRef outUid = nullptr;
        if (err == noErr && outDev != kAudioObjectUnknown)
        {
            sz = sizeof (outUid);
            addr.mSelector = kAudioDevicePropertyDeviceUID;
            err = AudioObjectGetPropertyData (outDev, &addr, 0, nullptr, &sz, &outUid);
        }
        if (err != noErr || outUid == nullptr)
        {
            NSLog (@"[Alter] CATap: default output device UID unavailable (%d)", (int) err);
            stopCoreAudioTap();
            return false;
        }

        // 3) private aggregate device wrapping the output device + our tap
        NSDictionary* aggDesc = @{
            @kAudioAggregateDeviceNameKey         : @"Alter System Audio",
            @kAudioAggregateDeviceUIDKey          : [[NSUUID UUID] UUIDString],
            @kAudioAggregateDeviceMainSubDeviceKey: (__bridge NSString*) outUid,
            @kAudioAggregateDeviceIsPrivateKey    : @YES,
            @kAudioAggregateDeviceIsStackedKey    : @NO,
            @kAudioAggregateDeviceTapAutoStartKey : @YES,
            @kAudioAggregateDeviceSubDeviceListKey: @[ @{ @kAudioSubDeviceUIDKey
                                                            : (__bridge NSString*) outUid } ],
            @kAudioAggregateDeviceTapListKey      : @[ @{ @kAudioSubTapUIDKey
                                                            : desc.UUID.UUIDString,
                                                          @kAudioSubTapDriftCompensationKey : @YES } ]
        };
        err = AudioHardwareCreateAggregateDevice ((__bridge CFDictionaryRef) aggDesc, &caAggDev);
        CFRelease (outUid);
        if (err != noErr || caAggDev == kAudioObjectUnknown)
        {
            NSLog (@"[Alter] CATap: AudioHardwareCreateAggregateDevice failed (%d)", (int) err);
            stopCoreAudioTap();
            return false;
        }

        // 4) the tap's real stream format (rate follows the output device)
        AudioStreamBasicDescription asbd {};
        sz = sizeof (asbd);
        addr = { kAudioTapPropertyFormat, kAudioObjectPropertyScopeGlobal,
                 kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData (caTap, &addr, 0, nullptr, &sz, &asbd) == noErr
            && asbd.mSampleRate > 1000.0)
            sampleRate = (int) std::lround (asbd.mSampleRate);

        // 5) IO proc on the aggregate: the tap arrives as INPUT buffers
        AlterSharedAudio* sharedPtr = &shared;
        err = AudioDeviceCreateIOProcIDWithBlock (&caProcID, caAggDev, queue,
            ^(const AudioTimeStamp*, const AudioBufferList* inInput,
              const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*)
        {
            if (inInput == nullptr || inInput->mNumberBuffers == 0) return;

            std::vector<float> stereo;   // interleaved L,R
            const AudioBuffer& b0 = inInput->mBuffers[0];
            const float* d = (const float*) b0.mData;
            if (d == nullptr) return;

            if (inInput->mNumberBuffers >= 2 && b0.mNumberChannels == 1)
            {
                // non-interleaved: buffer per channel
                const int frames = (int) (b0.mDataByteSize / sizeof (float));
                const float* L = d;
                const float* R = (const float*) inInput->mBuffers[1].mData;
                if (R == nullptr) R = L;
                stereo.resize ((size_t) frames * 2);
                for (int i = 0; i < frames; ++i)
                { stereo[(size_t)(2*i)] = L[i]; stereo[(size_t)(2*i+1)] = R[i]; }
            }
            else
            {
                // interleaved (mono, stereo or more: take the first two channels)
                const int ch     = std::max (1, (int) b0.mNumberChannels);
                const int frames = (int) (b0.mDataByteSize / sizeof (float)) / ch;
                stereo.resize ((size_t) frames * 2);
                for (int i = 0; i < frames; ++i)
                {
                    const float L = d[i * ch];
                    const float R = (ch > 1) ? d[i * ch + 1] : L;
                    stereo[(size_t)(2*i)] = L; stereo[(size_t)(2*i+1)] = R;
                }
            }

            if (! stereo.empty())
            {
                std::lock_guard<std::mutex> lk (sharedPtr->mtx);
                if (sharedPtr->pending.size() < 48000 * 2 * 4)   // ~4 s safety cap
                    sharedPtr->pending.insert (sharedPtr->pending.end(),
                                               stereo.begin(), stereo.end());
            }
        });
        if (err != noErr || caProcID == nullptr)
        {
            NSLog (@"[Alter] CATap: AudioDeviceCreateIOProcIDWithBlock failed (%d)", (int) err);
            stopCoreAudioTap();
            return false;
        }

        err = AudioDeviceStart (caAggDev, caProcID);
        if (err != noErr)
        {
            NSLog (@"[Alter] CATap: AudioDeviceStart failed (%d)", (int) err);
            stopCoreAudioTap();
            return false;
        }

        NSLog (@"[Alter] CATap: system audio capture ACTIVE (rate %d Hz)", sampleRate);
        caActive = true;
        shared.active = true;
        return true;
    }
    return false;
}

void MacSystemAudioCapture::Impl::stopCoreAudioTap()
{
    if (@available (macOS 14.2, *))
    {
        if (caAggDev != kAudioObjectUnknown && caProcID != nullptr)
        {
            AudioDeviceStop (caAggDev, caProcID);
            AudioDeviceDestroyIOProcID (caAggDev, caProcID);
        }
        caProcID = nullptr;
        if (caAggDev != kAudioObjectUnknown)
        {
            AudioHardwareDestroyAggregateDevice (caAggDev);
            caAggDev = kAudioObjectUnknown;
        }
        if (caTap != kAudioObjectUnknown)
        {
            AudioHardwareDestroyProcessTap (caTap);
            caTap = kAudioObjectUnknown;
        }
    }
    caActive = false;
}
#endif // ALTER_HAS_CATAP

MacSystemAudioCapture::MacSystemAudioCapture() : impl (std::make_unique<Impl>())
{
    impl->queue = dispatch_queue_create ("com.alter.systemaudio", DISPATCH_QUEUE_SERIAL);

    // AUTOMATIC backend choice by OS version:
    //   macOS 14.2+  → Core Audio process tap (pure system audio, own permission)
    //   macOS 13.0+  → ScreenCaptureKit fallback (needs Screen Recording)
   #if ALTER_HAS_CATAP
    if (@available (macOS 14.2, *))
        if (impl->startCoreAudioTap())
            return;
   #endif

    if (@available (macOS 13.0, *))
    {
        AlterAudioTap* tap = [[AlterAudioTap alloc] init];
        tap->shared = &impl->shared;
        impl->tap = tap;

        Impl* implPtr = impl.get();

        [SCShareableContent getShareableContentWithCompletionHandler:
            ^(SCShareableContent* content, NSError* err)
        {
            if (err != nil)
            {
                // Most commonly: "Screen Recording" permission has not been granted
                // (System Settings → Privacy & Security → Screen Recording → enable Alter,
                // then relaunch). ScreenCaptureKit audio needs this permission.
                NSLog (@"[Alter] System audio capture unavailable: %@", err.localizedDescription);
                return;
            }
            if (content.displays.count == 0)
            {
                NSLog (@"[Alter] System audio capture: no displays available to attach to.");
                return;
            }

            SCDisplay* disp = content.displays.firstObject;
            SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:disp excludingWindows:@[]];

            SCStreamConfiguration* cfg = [[SCStreamConfiguration alloc] init];
            cfg.capturesAudio = YES;
            cfg.excludesCurrentProcessAudio = YES;
            cfg.sampleRate = 48000;
            cfg.channelCount = 2;
            // Minimal video. An audio-only SCStream frequently never starts pumping
            // audio sample buffers; the stream has to be actively capturing the
            // screen for audio to flow (this matches Apple's reference sample, which
            // always registers BOTH a screen and an audio output). We keep the frame
            // tiny and slow so the video side costs almost nothing, then discard the
            // screen frames in the tap (it only consumes SCStreamOutputTypeAudio).
            cfg.width  = 2;
            cfg.height = 2;
            cfg.minimumFrameInterval = CMTimeMake (1, 4);   // ~4 fps: just enough to keep the stream alive
            cfg.pixelFormat = kCVPixelFormatType_32BGRA;
            cfg.queueDepth  = 5;
            cfg.showsCursor = NO;

            SCStream* stream = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:tap];

            NSError* eScreen = nil;
            [stream addStreamOutput:tap type:SCStreamOutputTypeScreen
                  sampleHandlerQueue:implPtr->queue error:&eScreen];   // frames discarded; keeps audio flowing
            if (eScreen != nil)
                NSLog (@"[Alter] System audio capture: screen output failed: %@", eScreen.localizedDescription);

            NSError* e2 = nil;
            [stream addStreamOutput:tap type:SCStreamOutputTypeAudio
                  sampleHandlerQueue:implPtr->queue error:&e2];

            if (e2 == nil)
            {
                [stream startCaptureWithCompletionHandler:^(NSError* e3)
                {
                    if (e3 == nil) { implPtr->shared.active = true; }
                    else           { NSLog (@"[Alter] System audio capture failed to start: %@", e3.localizedDescription); }
                }];
                implPtr->stream = stream;       // keep the +1 from alloc/init
            }
            else
            {
                NSLog (@"[Alter] System audio capture: audio output failed: %@", e2.localizedDescription);
            }
           #if ! __has_feature(objc_arc)
            if (e2 != nil) { [stream release]; }
            [filter release];
            [cfg release];
           #endif
        }];
    }
}

MacSystemAudioCapture::~MacSystemAudioCapture()
{
    if (impl == nullptr) return;

   #if ALTER_HAS_CATAP
    impl->stopCoreAudioTap();
   #endif

    if (@available (macOS 13.0, *))
    {
        if (impl->stream != nil)
        {
            [(SCStream*) impl->stream stopCaptureWithCompletionHandler:^(NSError*){}];
           #if ! __has_feature(objc_arc)
            [(SCStream*) impl->stream release];
           #endif
            impl->stream = nil;
        }
    }

   #if ! __has_feature(objc_arc)
    if (impl->tap != nil) { [(AlterAudioTap*) impl->tap release]; }
   #endif
    impl->tap = nil;
    // queue left to be reclaimed by the runtime (one per session)
}

bool MacSystemAudioCapture::isActive() const
{
    return impl != nullptr && impl->shared.active.load();
}

int MacSystemAudioCapture::getSampleRate() const
{
    return impl != nullptr ? impl->sampleRate : 48000;
}

int MacSystemAudioCapture::getAudioData (std::vector<float>& mono)
{
    if (impl == nullptr) return 0;
    std::lock_guard<std::mutex> lk (impl->shared.mtx);
    if (impl->shared.pending.empty()) return 0;
    const size_t frames = impl->shared.pending.size() / 2;     // pending is interleaved
    mono.resize (frames);
    for (size_t i = 0; i < frames; ++i)
        mono[i] = 0.5f * (impl->shared.pending[2*i] + impl->shared.pending[2*i+1]);
    impl->shared.pending.clear();
    return (int) frames;
}

int MacSystemAudioCapture::getStereoData (std::vector<float>& interleaved)
{
    if (impl == nullptr) return 0;
    std::lock_guard<std::mutex> lk (impl->shared.mtx);
    if (impl->shared.pending.empty()) return 0;
    interleaved = std::move (impl->shared.pending);
    impl->shared.pending.clear();
    return (int) (interleaved.size() / 2);
}

#else  // ScreenCaptureKit SDK not available

struct MacSystemAudioCapture::Impl {};
MacSystemAudioCapture::MacSystemAudioCapture() : impl (std::make_unique<Impl>()) {}
MacSystemAudioCapture::~MacSystemAudioCapture() {}
bool MacSystemAudioCapture::isActive() const { return false; }
int  MacSystemAudioCapture::getSampleRate() const { return 48000; }
int  MacSystemAudioCapture::getAudioData (std::vector<float>&) { return 0; }
int  MacSystemAudioCapture::getStereoData (std::vector<float>&) { return 0; }

#endif // ALTER_HAS_SCK

#endif // __APPLE__
