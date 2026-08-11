/*
  ==============================================================================

    HudRecorder.h
    Records the HUD window to a video file, with an optional audio track captured
    from the system mix (so e.g. the DAW output is included). No external tools
    (ffmpeg) required.
      - Windows: real .mp4 (H.264 + AAC) via the built-in Media Foundation encoder
                 (hardware-accelerated where available).
      - macOS / else: self-contained MJPEG .avi fallback.

    Capture:
      - Windows: BitBlt of the composited screen region, on a DEDICATED CAPTURE
                 THREAD, into a cached DIB section (see GrabThread in the .cpp).
                 The message thread does nothing at all during a recording.
      - macOS:   CGWindowListCreateImage (HudCaptureMac.mm), driven by the Timer
      - else:    component snapshot (OpenGL may not appear)
    Audio (interleaved stereo, chosen in the export dialog):
      - system mix — Windows: WASAPI loopback / macOS: ScreenCaptureKit
      - or straight from ONE ALTER plugin instance (its gap-free UDP stream),
        via setExternalAudioSource()
    Frames are rescaled to the HUD window's logical size.

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <deque>
#include <atomic>
#include <vector>
#include <functional>
#include <cstdint>

class HudRecorder : private juce::Timer,
                    private juce::Thread
{
public:
    HudRecorder();
    ~HudRecorder() override;

    /** Records the top-level window of `target` into `outputFile`. */
    bool start (juce::Component* target, juce::File outputFile);
    void stop();

    /** What gets grabbed: the HUD window, the whole display, or one fixed
        rectangle of the screen (used when each module is exported separately). */
    enum class CaptureSource { HudWindow, WholeScreen, ScreenRect };

    /** Whole-screen mode is capped to a sane resolution when exported at native
        size. Set BEFORE start(). */
    void setCaptureSource (CaptureSource s) noexcept { captureSource = s; }

    /** The rectangle for CaptureSource::ScreenRect, in PHYSICAL screen pixels.
        Locked for the whole take: a module that is moved mid-recording keeps being
        filmed where it started, which is the same choice whole-screen makes about
        the display it began on. */
    void setCaptureRect (juce::Rectangle<int> physicalScreenRect) noexcept
    {
        captureRect = physicalScreenRect;
    }

    /** Output size. Three modes, all of them aspect-safe (the HUD is NEVER stretched):

          (0, 0)   native HUD size — no scaling at all.
          (w, 0)   WIDTH-DRIVEN: the frame is exactly `w` px wide and the height
                   follows the HUD's own proportions. This is what the social
                   presets use — the platform width is what matters, and the user
                   shapes the HUD window itself to the aspect they want, so the
                   export has no black bars and no distortion.
          (w, h)   exact frame: scaled to FIT and centred on black (custom sizes,
                   where the requested aspect may not match the HUD).

        Set BEFORE start(). */
    void setTargetResolution (int w, int h) noexcept { tgtW = w; tgtH = h; }

    /** File extension the recorder produces on this platform. */
    static juce::String outputExtension()
    {
       #if JUCE_WINDOWS || JUCE_MAC
        return ".mp4";
       #else
        return ".avi";
       #endif
    }

    /** Ceiling on the long side when recording at ORIGINAL size. A 4K or 5K source
        would otherwise go into the encoder at its full size, which no longer fits in
        a real-time budget. The export dialog applies the same number so the size it
        promises is the size that comes out. */
    static constexpr int maxCaptureDim = 2560;

    // ── transparent (alpha) capture ───────────────────────────────────────────
    /** Record the modules on a transparent background instead of filming the
        screen, and write a lossless .mov that keeps the alpha channel.

        This is a genuinely different pipeline, not a flag on the encoder. A screen
        grab is opaque by construction — by the time the desktop compositor is done,
        every semi-transparent pixel has the background mixed INTO it and the two
        can never be separated again. So alpha frames cannot come from BitBlt at
        all: `grab` is called on the MESSAGE THREAD (it paints components, which no
        other thread may touch) and must return an ARGB image whose alpha is real.

        The recorder asks for the FINISHED size and the grab paints straight into
        it. Painting at full HUD size and resampling afterwards cost twice: a large
        allocation and a full software fill on the message thread, then a
        high-quality resample on the writer thread — for pixels that were about to
        be thrown away.

        Pass an empty function to go back to normal screen recording. Set BEFORE
        start(). */
    using AlphaGrabFn = std::function<juce::Image (int outW, int outH)>;
    void setAlphaSource (AlphaGrabFn grab) { alphaGrab = std::move (grab); }
    bool isAlphaMode() const noexcept { return alphaGrab != nullptr; }

    /** Extension for a transparent export. */
    static juce::String alphaOutputExtension() { return ".mov"; }

    // ── audio source ──────────────────────────────────────────────────────────
    /** Pull callback for an EXTERNAL audio source (e.g. one ALTER plugin instance
        streaming over UDP). Called repeatedly from the recorder's writer thread;
        it must APPEND every sample produced since the previous call to `out` as
        INTERLEAVED STEREO (L,R,L,R...) and return the number of FRAMES appended
        (0 = nothing new yet). It must never block. */
    using AudioPullFn = std::function<int (std::vector<float>& interleavedStereo)>;

    /** Record the system mix (WASAPI loopback / ScreenCaptureKit). The default. */
    void setSystemAudioSource() noexcept
    {
        externalPull = nullptr;
        externalRate = 0;
    }

    /** Record from `pull` instead of the system mix — used to take the audio track
        straight from a chosen VST plugin instance. `sampleRateHz` is the rate the
        callback delivers at (<= 0 → 48 kHz). Set BEFORE start(). */
    void setExternalAudioSource (AudioPullFn pull, int sampleRateHz)
    {
        externalPull = std::move (pull);
        externalRate = sampleRateHz;
    }

    /** True while the current/last recording takes its audio from setExternalAudioSource(). */
    bool usesExternalAudio() const noexcept { return externalPull != nullptr; }

    bool isRecording() const noexcept { return recording.load(); }
    juce::File getOutputFile() const { return outFile; }

private:
    // A grabbed frame. On Windows the message thread stores the raw full-res BGRA
    // bytes (cheap) and the worker does the heavy convert + downscale + encode.
    // On macOS / fallback a ready Image is carried instead.
    struct RawFrame
    {
        std::vector<std::uint8_t> bgra;   // top-down BGRA, size = w*h*4 (Windows path)
        int w = 0, h = 0;
        juce::Image image;                // mac / fallback path
        juce::int64 timeHns = 0;          // grab time since start, in 100-ns units (MP4 timestamps)
        bool valid() const { return (w > 0 && h > 0 && ! bgra.empty()) || image.isValid(); }
    };

    // Windows MP4 (Media Foundation) encoder — defined in the .cpp. Pimpl so the
    // heavy <mfapi.h> machinery stays out of the header.
    struct Mp4Encoder;

   #if JUCE_WINDOWS
    /** Windows screen grabbing runs here, NOT on the message thread. A whole-screen
        BitBlt costs tens of milliseconds; paying that 30x a second on the message
        thread is what made the HUD stutter while recording. (macOS never showed the
        problem — CGWindowListCreateImage is an order of magnitude cheaper — so the
        mac path still uses the Timer.) Defined in the .cpp. */
    struct GrabThread;
    std::unique_ptr<GrabThread> grabber;

    /** Interleaved-stereo sample-rate conversion, needed because the Media Foundation
        AAC encoder accepts 44.1 / 48 kHz ONLY. Defined in the .cpp. */
    struct StereoResampler;
    std::unique_ptr<StereoResampler> resampler;
    std::vector<float> resampledScratch;
   #endif

    AlphaGrabFn alphaGrab;                       // non-null → transparent .mov pipeline
    struct AlphaWriter;                          // .mov muxer, defined in the .cpp
    std::unique_ptr<AlphaWriter> alphaOut;

    /** Frames waiting to be PNG-compressed. Compression is what limits a
        transparent export, so it is done several frames at a time across the pool
        instead of one at a time on this thread. */
    std::vector<RawFrame> alphaPending;
    static constexpr int  maxAlphaBatch = 8;
    void encodeAlphaBatch (bool flushAll);

    /** Workers for the frame scale/letterbox pass. One core cannot rescale a 4K
        frame 30 times a second, and when that pass falls behind the frame queue
        fills and the capture thread starts skipping grabs — a visibly stuttering
        export. Created for the duration of a recording only. */
    std::unique_ptr<juce::ThreadPool> scalePool;

    void timerCallback() override;             // grab video frame (mac / fallback path)
    void run() override;                       // convert + encode + write (worker thread)

    RawFrame capture (juce::Component*);
    RawFrame captureScreen();
    void     processFrame (RawFrame);

    // Called from the capture thread.
    bool queueHasRoom() const;
    void pushFrame (RawFrame&&);
    void wakeWriter() { notify(); }

    // Reusable frame buffers. A 4K frame is 33 MB; allocating and freeing one per
    // frame at 30 Hz is ~1 GB/s of pure allocator churn, which on Windows means
    // repeated large VirtualAlloc/VirtualFree round trips.
    std::vector<std::uint8_t> acquireBuffer (size_t bytes);
    void recycleBuffer (std::vector<std::uint8_t>&& b);
    juce::CriticalSection poolLock;
    std::vector<std::vector<std::uint8_t>> bufferPool;
    void        flushAudioToEncoder();
    void        flushAudioToMov();       // transparent .mov: PCM straight in         // drain PCM → MP4 encoder (Windows)
    void        pollAudio();
    void        writeAviHeader();
    void        writeVideoChunk (const void* jpeg, int len);
    void        writeAudioChunk (const juce::int16* pcm, int numSamples);
    void        finalizeAvi();

    struct AudioTap;
    std::unique_ptr<AudioTap> audioTap;

    std::unique_ptr<Mp4Encoder> mp4;            // Windows: active MP4 encoder (else null)
    bool useMp4 = false;                        // chosen backend
    double startMs = 0.0;                       // recording start (message-thread clock)
    juce::int64 audioWrittenSamples = 0;        // MP4 audio timeline (per-channel samples)
    juce::int64 movAudioWritten = 0;            // same, for the transparent .mov

    juce::Component::SafePointer<juce::Component> target;
    std::atomic<bool> recording { false };

    juce::File outFile;
    std::unique_ptr<juce::FileOutputStream> avi;
    bool headerWritten = false;
    int  frameCount = 0;
    int  recW = 0, recH = 0;
    int  tgtW = 0, tgtH = 0;                     // see setTargetResolution()
    CaptureSource captureSource = CaptureSource::HudWindow;
    juce::Rectangle<int> captureRect;            // CaptureSource::ScreenRect

    /** Stream dimensions for a captured frame, per the setTargetResolution() rules. */
    void resolveOutputSize (int srcW, int srcH, int& outW, int& outH) const noexcept;

    bool hasAudio = false;
    int  audioRate = 48000;                      // rate written to the FILE (post-resample)
    int  captureRate = 48000;                    // rate the audio SOURCE delivers at
    int  audioChannels = 2;                      // recorded PCM is interleaved stereo
    int  audioTotalSamples = 0;                  // AVI: interleaved samples written
    AudioPullFn externalPull;                    // non-null → plugin/external audio
    int         externalRate = 0;                // rate the external source delivers at
    std::vector<float> audioScratch;             // reused float buffer (worker thread)
    int  lastLogicalW = -1, lastLogicalH = -1;   // skip capture while the HUD is resizing
    std::vector<juce::int16> audioPcm;         // drained system audio, queued for the next chunk

    // Diagnostics for the "exported file has no sound" class of bug: what the
    // SOURCE delivered (pollAudio) vs what reached the encoder. Logged once a
    // second in a debug build; costs nothing in release.
    juce::int64 dbgSamples = 0;
    float       dbgPeak = 0.0f;
    double      dbgLastLogSec = 0.0;
    juce::int64 dbgPolls = 0, dbgPollsEmpty = 0;

    // Frame accounting: grabbed vs skipped (queue full) vs encoded, plus the time
    // the encode pass costs. A stuttering export is always one of these three.
    std::atomic<int>    dbgGrabbed { 0 }, dbgSkipped { 0 }, dbgEncoded { 0 };
    std::atomic<double> dbgEncodeMs { 0.0 };
    double dbgFrameLogSec = 0.0;

    juce::int64 riffSizePos = 0, totalFramesPos = 0, vidLengthPos = 0,
                audLengthPos = 0, moviSizePos = 0, moviFourccPos = 0;

    struct IdxEntry { char id[4]; juce::uint32 offset, size; };
    std::vector<IdxEntry> index;

    static constexpr int fps = 30;
    // Raw frames are large, so the queue stays short. On Windows the buffers are
    // recycled through bufferPool rather than reallocated, so the extra depth costs
    // nothing and absorbs encoder hiccups instead of dropping frames. The mac path
    // still queues whole juce::Images, so it keeps the original, tighter bound.
   #if JUCE_WINDOWS
    static constexpr int maxQueued = 6;
   #else
    static constexpr int maxQueued = 4;
   #endif
    // (kept private-ish by convention but declared public above — see the header
    // section marker; the export dialog needs the same number to predict the size)

    juce::CriticalSection qLock;
    std::deque<RawFrame> queue;
};
