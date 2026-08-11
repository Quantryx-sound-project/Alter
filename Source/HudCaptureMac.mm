/*
  ==============================================================================

    HudCaptureMac.mm
    macOS window capture for the HUD recorder, using CGWindowListCreateImage so
    OpenGL / composited content is captured correctly (the equivalent of the
    Windows PrintWindow path).

    NOTE: on macOS 10.15+ this needs the "Screen Recording" permission
    (System Settings → Privacy & Security → Screen Recording). The first capture
    triggers the OS prompt.

    This file is Objective-C++ and is only compiled on Apple platforms.

  ==============================================================================
*/

#include <JuceHeader.h>

#if JUCE_MAC

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <cstring>

bool alterCaptureMacWindow (void* nsViewHandle, juce::Image& out)
{
    if (nsViewHandle == nullptr) return false;

    NSView*   view = (NSView*) nsViewHandle;
    NSWindow* win  = [view window];
    if (win == nil) return false;

    const CGWindowID wid = (CGWindowID) [win windowNumber];
    if (wid == 0) return false;

    CGImageRef cg = CGWindowListCreateImage (CGRectNull,
                                             kCGWindowListOptionIncludingWindow,
                                             wid,
                                             (CGWindowImageOption) (kCGWindowImageBoundsIgnoreFraming
                                                                  | kCGWindowImageNominalResolution));
    if (cg == nullptr) return false;

    const int w = (int) CGImageGetWidth (cg);
    const int h = (int) CGImageGetHeight (cg);
    if (w <= 0 || h <= 0) { CGImageRelease (cg); return false; }

    out = juce::Image (juce::Image::ARGB, w, h, true);
    {
        juce::Image::BitmapData bd (out, juce::Image::BitmapData::writeOnly);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        // juce::Image ARGB is BGRA in memory on little-endian → premultiplied-first + LE
        CGContextRef ctx = CGBitmapContextCreate (bd.data, (size_t) w, (size_t) h, 8,
                                                  (size_t) bd.lineStride, cs,
                                                  kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        if (ctx != nullptr)
        {
            CGContextDrawImage (ctx, CGRectMake (0, 0, w, h), cg);
            CGContextRelease (ctx);
        }
        CGColorSpaceRelease (cs);
    }

    CGImageRelease (cg);
    return true;
}

/** Whole-display grab (the "record the entire screen" mode). Same bitmap layout
    as alterCaptureMacWindow, so the recorder can treat both the same. */
bool alterCaptureMacScreen (juce::Image& out)
{
    CGImageRef cg = CGDisplayCreateImage (CGMainDisplayID());
    if (cg == nullptr) return false;

    const int w = (int) CGImageGetWidth (cg);
    const int h = (int) CGImageGetHeight (cg);
    if (w <= 0 || h <= 0) { CGImageRelease (cg); return false; }

    out = juce::Image (juce::Image::ARGB, w, h, true);
    {
        juce::Image::BitmapData bd (out, juce::Image::BitmapData::writeOnly);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate (bd.data, (size_t) w, (size_t) h, 8,
                                                  (size_t) bd.lineStride, cs,
                                                  kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        if (ctx != nullptr)
        {
            CGContextDrawImage (ctx, CGRectMake (0, 0, w, h), cg);
            CGContextRelease (ctx);
        }
        CGColorSpaceRelease (cs);
    }

    CGImageRelease (cg);
    return true;
}

// ── AVFoundation MP4 (H.264 + AAC) encoder ─────────────────────────────────────
//  C-style interface consumed by HudRecorder's mac Mp4Encoder wrapper. Non-ARC:
//  ObjC objects are alloc/retained here and released in close()/destroy().
namespace
{
    struct MacMp4
    {
        AVAssetWriter* writer = nil;
        AVAssetWriterInput* videoIn = nil;
        AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
        AVAssetWriterInput* audioIn = nil;
        int  width = 0, height = 0, fps = 30, audioRate = 48000, audioChannels = 1;
        bool hasAudio = false, started = false;
    };
}

void* alterMp4Create() { return new MacMp4(); }

void alterMp4Destroy (void* hp)
{
    auto* m = (MacMp4*) hp;
    if (m == nullptr) return;
    if (m->adaptor) [m->adaptor release];
    if (m->videoIn) [m->videoIn release];
    if (m->audioIn) [m->audioIn release];
    if (m->writer)  [m->writer release];
    delete m;
}

bool alterMp4Open (void* hp, const char* utf8Path, int w, int h, int fps,
                   int aRate, int aChannels, bool withAudio)
{
    auto* m = (MacMp4*) hp;
    if (m == nullptr) return false;
    m->width = w & ~1; m->height = h & ~1; m->fps = (fps > 0 ? fps : 30);
    m->audioRate = aRate; m->audioChannels = (aChannels > 0 ? aChannels : 1); m->hasAudio = withAudio;
    if (m->width < 2 || m->height < 2) return false;

    @autoreleasepool
    {
        NSString* p   = [NSString stringWithUTF8String: utf8Path];
        NSURL*    url = [NSURL fileURLWithPath: p];
        NSError*  err = nil;
        m->writer = [[AVAssetWriter alloc] initWithURL: url fileType: AVFileTypeMPEG4 error: &err];
        if (m->writer == nil) return false;

        const double br = fmin (40000000.0, fmax (4000000.0,
                                (double) m->width * (double) m->height * (double) m->fps * 0.18));
        NSDictionary* comp = @{ AVVideoAverageBitRateKey:      @((NSInteger) br),
                                AVVideoMaxKeyFrameIntervalKey: @(m->fps * 2) };
        NSDictionary* vset = @{ AVVideoCodecKey:  AVVideoCodecTypeH264,
                                AVVideoWidthKey:  @(m->width),
                                AVVideoHeightKey: @(m->height),
                                AVVideoCompressionPropertiesKey: comp };
        m->videoIn = [[AVAssetWriterInput alloc] initWithMediaType: AVMediaTypeVideo outputSettings: vset];
        m->videoIn.expectsMediaDataInRealTime = YES;

        NSDictionary* pbAttrs = @{ (id) kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                                   (id) kCVPixelBufferWidthKey:           @(m->width),
                                   (id) kCVPixelBufferHeightKey:          @(m->height) };
        m->adaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc]
                         initWithAssetWriterInput: m->videoIn sourcePixelBufferAttributes: pbAttrs];

        if (! [m->writer canAddInput: m->videoIn]) return false;
        [m->writer addInput: m->videoIn];

        if (withAudio)
        {
            NSDictionary* aset = @{ AVFormatIDKey:         @(kAudioFormatMPEG4AAC),
                                    AVNumberOfChannelsKey: @(m->audioChannels),
                                    AVSampleRateKey:       @(m->audioRate),
                                    // stereo needs the headroom (music, not speech)
                                    AVEncoderBitRateKey:   @(m->audioChannels >= 2 ? 256000 : 128000) };
            m->audioIn = [[AVAssetWriterInput alloc] initWithMediaType: AVMediaTypeAudio outputSettings: aset];
            m->audioIn.expectsMediaDataInRealTime = YES;
            if ([m->writer canAddInput: m->audioIn]) [m->writer addInput: m->audioIn];
            else { [m->audioIn release]; m->audioIn = nil; m->hasAudio = false; }
        }

        if (! [m->writer startWriting]) return false;
        [m->writer startSessionAtSourceTime: kCMTimeZero];
        m->started = true;
    }
    return true;
}

void alterMp4WriteVideoBGRA (void* hp, const void* bgra, int w, int h, long long timeHns)
{
    auto* m = (MacMp4*) hp;
    if (m == nullptr || ! m->started || m->videoIn == nil) return;
    if (! m->videoIn.isReadyForMoreMediaData) return;     // drop if encoder is busy
    if (w != m->width || h != m->height) return;

    CVPixelBufferRef pb = NULL;
    CVPixelBufferPoolRef pool = m->adaptor.pixelBufferPool;
    if (pool != NULL) CVPixelBufferPoolCreatePixelBuffer (NULL, pool, &pb);
    if (pb == NULL)
        if (CVPixelBufferCreate (kCFAllocatorDefault, w, h, kCVPixelFormatType_32BGRA, NULL, &pb) != kCVReturnSuccess)
            return;

    CVPixelBufferLockBaseAddress (pb, 0);
    uint8_t*       dst       = (uint8_t*) CVPixelBufferGetBaseAddress (pb);
    const size_t   dstStride = CVPixelBufferGetBytesPerRow (pb);
    const uint8_t* src       = (const uint8_t*) bgra;
    const size_t   srcStride = (size_t) w * 4;
    for (int y = 0; y < h; ++y)
        std::memcpy (dst + (size_t) y * dstStride, src + (size_t) y * srcStride, srcStride);
    CVPixelBufferUnlockBaseAddress (pb, 0);

    const CMTime t = CMTimeMake (timeHns, 10000000);
    [m->adaptor appendPixelBuffer: pb withPresentationTime: t];
    CVPixelBufferRelease (pb);
}

void alterMp4WriteAudioPCM (void* hp, const short* pcm, int numSamples, int aChannels, long long timeHns)
{
    auto* m = (MacMp4*) hp;
    if (m == nullptr || ! m->started || ! m->hasAudio || m->audioIn == nil) return;
    if (! m->audioIn.isReadyForMoreMediaData) return;
    const int channels = (aChannels > 0 ? aChannels : 1);
    const int frames   = numSamples / channels;
    if (frames <= 0) return;

    AudioStreamBasicDescription asbd; std::memset (&asbd, 0, sizeof (asbd));
    asbd.mSampleRate       = m->audioRate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mFramesPerPacket   = 1;
    asbd.mChannelsPerFrame  = (UInt32) channels;
    asbd.mBitsPerChannel    = 16;
    asbd.mBytesPerFrame     = (UInt32) (2 * channels);
    asbd.mBytesPerPacket    = (UInt32) (2 * channels);

    CMAudioFormatDescriptionRef fmt = NULL;
    if (CMAudioFormatDescriptionCreate (kCFAllocatorDefault, &asbd, 0, NULL, 0, NULL, NULL, &fmt) != noErr)
        return;

    const int dataBytes = numSamples * 2;
    CMBlockBufferRef block = NULL;
    if (CMBlockBufferCreateWithMemoryBlock (kCFAllocatorDefault, NULL, (size_t) dataBytes, kCFAllocatorDefault,
                                            NULL, 0, (size_t) dataBytes, 0, &block) != noErr)
    { CFRelease (fmt); return; }
    CMBlockBufferReplaceDataBytes (pcm, block, 0, (size_t) dataBytes);

    CMSampleTimingInfo timing;
    timing.duration             = CMTimeMake (1, m->audioRate);
    timing.presentationTimeStamp = CMTimeMake (timeHns, 10000000);
    timing.decodeTimeStamp      = kCMTimeInvalid;

    CMSampleBufferRef sbuf = NULL;
    if (CMSampleBufferCreate (kCFAllocatorDefault, block, TRUE, NULL, NULL, fmt,
                              frames, 1, &timing, 0, NULL, &sbuf) == noErr)
    {
        [m->audioIn appendSampleBuffer: sbuf];
        CFRelease (sbuf);
    }
    CFRelease (block);
    CFRelease (fmt);
}

void alterMp4Close (void* hp)
{
    auto* m = (MacMp4*) hp;
    if (m == nullptr || ! m->started || m->writer == nil) return;
    if (m->videoIn) [m->videoIn markAsFinished];
    if (m->audioIn) [m->audioIn markAsFinished];

    dispatch_semaphore_t sem = dispatch_semaphore_create (0);
    [m->writer finishWritingWithCompletionHandler: ^{ dispatch_semaphore_signal (sem); }];
    dispatch_semaphore_wait (sem, dispatch_time (DISPATCH_TIME_NOW, (int64_t) (10 * NSEC_PER_SEC)));
    m->started = false;
}

#endif
