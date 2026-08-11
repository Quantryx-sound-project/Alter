#include "HudRecorder.h"
#include "MovWriter.h"
#include <cmath>
#include <cstring>
#include <algorithm>

#if JUCE_WINDOWS
 #include <windows.h>
 #include "WasapiLoopbackCapture.h"
 #include <mfapi.h>
 #include <mfidl.h>
 #include <mfreadwrite.h>
 #include <mferror.h>
 #include <codecapi.h>
 #include <icodecapi.h>
 #pragma comment(lib, "mfplat.lib")
 #pragma comment(lib, "mfreadwrite.lib")
 #pragma comment(lib, "mfuuid.lib")
 #pragma comment(lib, "ole32.lib")
 #pragma comment(lib, "strmiids.lib")   // CODECAPI_* GUIDs (encoder rate control)
#elif JUCE_MAC
 #include "MacSystemAudioCapture.h"
#endif

#if JUCE_MAC
bool alterCaptureMacWindow (void* nsViewHandle, juce::Image& out);   // HudCaptureMac.mm
bool alterCaptureMacScreen (juce::Image& out);                       // HudCaptureMac.mm
#endif

// ── fast BGRA scale + letterbox ───────────────────────────────────────────────
// Scales `src` to FIT inside dstW x dstH (aspect preserved), centres it, and fills
// the remainder with black — the same contract as letterboxInto(), but working on
// raw BGRA bytes so the result can be written STRAIGHT into the encoder's buffer.
//
// The old path cost four full-frame passes per frame: bgra -> juce::Image ->
// high-quality resample -> staging buffer -> encoder buffer. At 4K/30 that is
// ~530 MB/s of copying before the encoder even starts, and it was the single
// biggest reason the Windows export could not keep up.
//
// Shrinking uses an exact box average (every source pixel is read exactly once,
// so a 1-px neon line contributes its energy instead of being point-sampled out
// of existence); enlarging uses bilinear.
//
// The work is split across a thread pool by OUTPUT ROW. Every row is independent
// in both branches, and at 4K it is the single most expensive thing the writer
// thread does: a custom 3840x2160 export from a 2560x1440 capture is 8.3 Mpx of
// bilinear interpolation per frame, which one core cannot do 30 times a second.
// The queue then fills, the capture thread stops grabbing, and the result is a
// video that visibly drops frames — the "it stutters above Full HD" report.
struct ScaleGeom { int w = 0, h = 0, ox = 0, oy = 0; };

static ScaleGeom scaleGeomFor (int srcW, int srcH, int dstW, int dstH)
{
    const double s = juce::jmin ((double) dstW / (double) srcW,
                                 (double) dstH / (double) srcH);
    ScaleGeom g;
    g.w  = juce::jlimit (1, dstW, juce::roundToInt (srcW * s));
    g.h  = juce::jlimit (1, dstH, juce::roundToInt (srcH * s));
    g.ox = (dstW - g.w) / 2;
    g.oy = (dstH - g.h) / 2;
    return g;
}

// ── sRGB <-> linear light, for a physically correct box average ───────────────
// Averaging sRGB-ENCODED bytes is wrong, and it is wrong in the direction that
// hurts this app specifically. A 1-px white stroke next to a black pixel should
// average to mid GREY light, which is byte ~188 in sRGB — averaging the bytes
// gives 128, less than half the light. So every thin stroke and every bloom
// falloff comes out dimmer and muddier than it was on screen, and the loss grows
// with the downscale factor. Averaging in LINEAR light and converting back keeps
// the energy, which is exactly what makes small shapes survive a downscale.
struct SrgbTables
{
    std::uint32_t toLinear[256];        // 0 .. 65535
    std::uint8_t  toSrgb[4097];         // linear 0..4096 -> byte

    SrgbTables()
    {
        for (int i = 0; i < 256; ++i)
        {
            const double c = i / 255.0;
            const double l = (c <= 0.04045) ? (c / 12.92)
                                            : std::pow ((c + 0.055) / 1.055, 2.4);
            toLinear[i] = (std::uint32_t) juce::jlimit<long> (0, 65535, std::lround (l * 65535.0));
        }

        for (int i = 0; i <= 4096; ++i)
        {
            const double l = i / 4096.0;
            const double c = (l <= 0.0031308) ? (l * 12.92)
                                              : (1.055 * std::pow (l, 1.0 / 2.4) - 0.055);
            toSrgb[i] = (std::uint8_t) juce::jlimit (0, 255, (int) std::lround (c * 255.0));
        }
    }
};

static const SrgbTables& srgbTables()
{
    static const SrgbTables t;          // built once, read-only thereafter
    return t;
}

/** Scales the output rows [y0, y1) only. `cx`/`cy` are the box-average edge tables
    (shrinking); pass nullptr for the bilinear enlarging path. */
static void scaleLetterboxRows (const std::uint8_t* src, int srcW, int srcH,
                                std::uint8_t* dst, int dstW,
                                const ScaleGeom& g,
                                const int* cx, const int* cy,
                                int y0, int y1)
{
    const int w = g.w, h = g.h, ox = g.ox, oy = g.oy;
    const size_t srcPitch = (size_t) srcW * 4;
    const size_t dstPitch = (size_t) dstW * 4;
    juce::ignoreUnused (h);

    if (cx != nullptr && cy != nullptr)
    {
        // ---- shrinking: exact box average, in LINEAR LIGHT ----
        const auto& tab = srgbTables();

        for (int y = y0; y < y1; ++y)
        {
            const int sy0 = cy[(size_t) y];
            const int sy1 = juce::jmax (sy0 + 1, cy[(size_t) y + 1]);
            std::uint8_t* d = dst + (size_t) (y + oy) * dstPitch + (size_t) ox * 4;

            for (int x = 0; x < w; ++x, d += 4)
            {
                const int sx0 = cx[(size_t) x];
                const int sx1 = juce::jmax (sx0 + 1, cx[(size_t) x + 1]);
                std::uint64_t accB = 0, accG = 0, accR = 0;

                for (int sy = sy0; sy < sy1; ++sy)
                {
                    const std::uint8_t* sp = src + (size_t) sy * srcPitch + (size_t) sx0 * 4;
                    for (int sx = sx0; sx < sx1; ++sx, sp += 4)
                    {
                        accB += tab.toLinear[sp[0]];
                        accG += tab.toLinear[sp[1]];
                        accR += tab.toLinear[sp[2]];
                    }
                }

                const std::uint64_t n = (std::uint64_t) ((sx1 - sx0) * (sy1 - sy0)) * 16ULL;
                d[0] = tab.toSrgb[(size_t) juce::jmin<std::uint64_t> (4096, accB / n)];
                d[1] = tab.toSrgb[(size_t) juce::jmin<std::uint64_t> (4096, accG / n)];
                d[2] = tab.toSrgb[(size_t) juce::jmin<std::uint64_t> (4096, accR / n)];
                d[3] = 255;
            }
        }
        return;
    }

    // ---- enlarging: bilinear, 8-bit fixed-point weights ----
    // The old version did this in DOUBLES, recomputing the horizontal weight from
    // scratch for every pixel of every row: three multiplies, a floor and a round
    // per channel. At 3840x2160 that is ~25 million double ops per frame. The
    // horizontal weights are the same for every row, so they are built once, and
    // 8-bit fixed point is exact enough for 8-bit output.
    std::vector<int> xo0 ((size_t) w), xo1 ((size_t) w), xwt ((size_t) w);
    for (int x = 0; x < w; ++x)
    {
        const double fx = ((double) x + 0.5) * (double) srcW / (double) w - 0.5;
        const int    ix = (int) std::floor (fx);
        xo0[(size_t) x] = juce::jlimit (0, srcW - 1, ix)     * 4;
        xo1[(size_t) x] = juce::jlimit (0, srcW - 1, ix + 1) * 4;
        xwt[(size_t) x] = juce::jlimit (0, 256, (int) std::lround ((fx - (double) ix) * 256.0));
    }

    for (int y = y0; y < y1; ++y)
    {
        const double fy = ((double) y + 0.5) * (double) srcH / (double) g.h - 0.5;
        const int    iy = (int) std::floor (fy);
        const int    wy = juce::jlimit (0, 256, (int) std::lround ((fy - (double) iy) * 256.0));

        const std::uint8_t* row0 = src + (size_t) juce::jlimit (0, srcH - 1, iy)     * srcPitch;
        const std::uint8_t* row1 = src + (size_t) juce::jlimit (0, srcH - 1, iy + 1) * srcPitch;
        std::uint8_t* d = dst + (size_t) (y + oy) * dstPitch + (size_t) ox * 4;

        for (int x = 0; x < w; ++x, d += 4)
        {
            const int a0 = xo0[(size_t) x], a1 = xo1[(size_t) x], wx = xwt[(size_t) x];

            for (int c = 0; c < 3; ++c)
            {
                const int t = (row0[a0 + c] * (256 - wx) + row0[a1 + c] * wx) >> 8;
                const int b = (row1[a0 + c] * (256 - wx) + row1[a1 + c] * wx) >> 8;
                d[c] = (std::uint8_t) ((t * (256 - wy) + b * wy) >> 8);
            }
            d[3] = 255;
        }
    }
}

/** Whole-frame scale + letterbox, spread over `pool` by output row. */
static void scaleLetterboxBGRA (const std::uint8_t* src, int srcW, int srcH,
                                std::uint8_t* dst, int dstW, int dstH,
                                juce::ThreadPool* pool)
{
    if (src == nullptr || dst == nullptr || srcW < 1 || srcH < 1 || dstW < 1 || dstH < 1)
        return;

    if (srcW == dstW && srcH == dstH)
    {
        std::memcpy (dst, src, (size_t) dstW * (size_t) dstH * 4);   // exact size: no work at all
        return;
    }

    const ScaleGeom g = scaleGeomFor (srcW, srcH, dstW, dstH);

    if (g.w != dstW || g.h != dstH)
        std::memset (dst, 0, (size_t) dstW * (size_t) dstH * 4);     // black bars

    // Box-average edge tables, shared read-only by every worker.
    std::vector<int> cx, cy;
    const bool shrinking = (g.w < srcW || g.h < srcH);
    if (shrinking)
    {
        cx.resize ((size_t) g.w + 1);
        cy.resize ((size_t) g.h + 1);
        for (int x = 0; x <= g.w; ++x) cx[(size_t) x] = (int) ((juce::int64) x * srcW / g.w);
        for (int y = 0; y <= g.h; ++y) cy[(size_t) y] = (int) ((juce::int64) y * srcH / g.h);
    }
    const int* cxp = shrinking ? cx.data() : nullptr;
    const int* cyp = shrinking ? cy.data() : nullptr;

    auto doRows = [&] (int a, int b)
    {
        scaleLetterboxRows (src, srcW, srcH, dst, dstW, g, cxp, cyp, a, b);
    };

    const int nThreads = (pool != nullptr && g.h >= 128)
                             ? juce::jlimit (1, 8, pool->getNumThreads() + 1) : 1;
    if (nThreads <= 1) { doRows (0, g.h); return; }

    // Ranges are decided BEFORE anything is queued: a job must never read a counter
    // the queueing loop is still incrementing.
    const int chunk = (g.h + nThreads - 1) / nThreads;
    std::vector<std::pair<int, int>> ranges;
    for (int t = 1; t < nThreads; ++t)
    {
        const int a = juce::jmin (g.h, t * chunk);
        const int b = juce::jmin (g.h, a + chunk);
        if (a < b) ranges.push_back ({ a, b });
    }

    const int spawned = (int) ranges.size();
    std::atomic<int> finished { 0 };
    juce::WaitableEvent allDone;

    for (int i = 0; i < spawned; ++i)
    {
        const int a = ranges[(size_t) i].first, b = ranges[(size_t) i].second;
        pool->addJob ([&, a, b]
        {
            doRows (a, b);
            if (finished.fetch_add (1) + 1 == spawned) allDone.signal();
        });
    }

    doRows (0, juce::jmin (g.h, chunk));      // this thread takes the first chunk

    // The jobs capture locals by reference, so we must not leave before they finish.
    while (finished.load() < spawned)
        allDone.wait (50);
}

// ── transparent (.mov) output ─────────────────────────────────────────────────
// Pimpl so MovWriter.h stays out of HudRecorder.h.
struct HudRecorder::AlphaWriter
{
    AlterMovWriter mov;
};

// ── platform audio tap (system mix → INTERLEAVED STEREO float, drained) ───────
// Stereo, not a mono downmix: the exported file should carry the same signal the
// user hears. Both backends already capture 2 channels internally.
struct HudRecorder::AudioTap
{
   #if JUCE_WINDOWS
    WasapiLoopbackCapture cap;
    bool pull (std::vector<float>& interleaved) { return cap.getStereoData (interleaved); }
    int  rate() const { return cap.getSampleRate(); }
   #elif JUCE_MAC
    MacSystemAudioCapture cap;
    bool pull (std::vector<float>& interleaved) { return cap.getStereoData (interleaved) > 0; }
    int  rate() const { return cap.getSampleRate(); }
   #else
    bool pull (std::vector<float>&) { return false; }
    int  rate() const { return 48000; }
   #endif
};

// ── Windows MP4 (H.264 + AAC) encoder via Media Foundation ─────────────────────
#if JUCE_WINDOWS
struct HudRecorder::Mp4Encoder
{
    IMFSinkWriter* writer = nullptr;
    DWORD videoIndex = 0, audioIndex = 0;
    bool  hasAudio = false, ok = false, mfStarted = false;
    int   width = 0, height = 0, fps = 30, audioRate = 48000, audioChannels = 1;

    ~Mp4Encoder() { close(); }

    bool open (const juce::File& file, int w, int h, int frameRate,
               int aRate, int aChannels, bool withAudio)
    {
        width = w & ~1; height = h & ~1; fps = juce::jmax (1, frameRate);
        audioRate = aRate; audioChannels = juce::jmax (1, aChannels); hasAudio = withAudio;
        if (width < 2 || height < 2) return false;

        if (FAILED (MFStartup (MF_VERSION))) return false;
        mfStarted = true;

        IMFAttributes* attr = nullptr;
        if (SUCCEEDED (MFCreateAttributes (&attr, 2)))
        {
            attr->SetUINT32 (MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 1);
            // By default the sink writer PACES us: WriteSample() blocks to keep the
            // encoder from running ahead. That is right for transcoding a file, but
            // for live capture it stalls the writer thread, the frame queue backs up
            // and frames get dropped. We already bound memory with maxQueued.
            attr->SetUINT32 (MF_SINK_WRITER_DISABLE_THROTTLING, 1);
        }

        const juce::String path = file.getFullPathName();   // keep alive across the call
        HRESULT hr = MFCreateSinkWriterFromURL (path.toWideCharPointer(), nullptr, attr, &writer);
        if (attr) attr->Release();
        if (FAILED (hr)) { close(); return false; }

        // ---- video: output H.264 ----
        // 0.18 bits/pixel was too lean for what ALTER actually produces. This is
        // thin saturated strokes and glow gradients on near-black — the worst case
        // for H.264: every stroke is high-frequency detail, and the dark background
        // fools the rate controller into thinking the frame is cheap. At 1440p that
        // gave ~20 Mbit/s and the export came back visibly softer and blockier than
        // the HUD it was filming. 0.35 bits/pixel doubles the headroom, and the
        // ceiling is raised so 4K is not silently capped back down to it.
        const UINT32 bitrate = (UINT32) juce::jlimit (8000000.0, 80000000.0,
                                   (double) width * (double) height * (double) fps * 0.35);
        IMFMediaType* vOut = nullptr;
        MFCreateMediaType (&vOut);
        vOut->SetGUID   (MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vOut->SetGUID   (MF_MT_SUBTYPE,    MFVideoFormat_H264);
        vOut->SetUINT32 (MF_MT_AVG_BITRATE, bitrate);
        vOut->SetUINT32 (MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        vOut->SetUINT32 (MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        MFSetAttributeSize  (vOut, MF_MT_FRAME_SIZE, (UINT32) width, (UINT32) height);
        MFSetAttributeRatio (vOut, MF_MT_FRAME_RATE, (UINT32) fps, 1);
        MFSetAttributeRatio (vOut, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->AddStream (vOut, &videoIndex);
        vOut->Release();
        if (FAILED (hr)) { close(); return false; }

        // ---- video: input BGRA (top-down → positive stride) ----
        IMFMediaType* vIn = nullptr;
        MFCreateMediaType (&vIn);
        vIn->SetGUID   (MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vIn->SetGUID   (MF_MT_SUBTYPE,    MFVideoFormat_RGB32);
        vIn->SetUINT32 (MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        vIn->SetUINT32 (MF_MT_DEFAULT_STRIDE, (UINT32) (INT32) (width * 4));
        // GDI hands us FULL-RANGE RGB. Say so explicitly instead of letting the
        // colour converter guess — guessing studio range crushes blacks, which on
        // a HUD that is mostly near-black is exactly where it shows.
        vIn->SetUINT32 (MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255);
        MFSetAttributeSize  (vIn, MF_MT_FRAME_SIZE, (UINT32) width, (UINT32) height);
        MFSetAttributeRatio (vIn, MF_MT_FRAME_RATE, (UINT32) fps, 1);
        MFSetAttributeRatio (vIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->SetInputMediaType (videoIndex, vIn, nullptr);
        vIn->Release();
        if (FAILED (hr)) { close(); return false; }

        // NOTE: the H.264 rate-control block used to sit HERE, between the video and
        // audio streams. GetServiceForStream() instantiates the encoder MFT, and a
        // sink writer that has already handed out an encoder can refuse a later
        // AddStream() with MF_E_INVALIDREQUEST — i.e. the audio track quietly went
        // missing. Both streams are now declared first; the codec tuning happens
        // afterwards, still before BeginWriting(), where it works just as well.

        // ---- audio: output AAC + input PCM ----
        // The Microsoft AAC encoder only accepts a SHORT LIST of output types:
        // 44.1/48 kHz, 1 or 2 channels, and an average byte rate of exactly
        // 12000 / 16000 / 20000 / 24000 B/s. Which of those are actually offered
        // depends on the machine (the encoder enumerates a fixed type list, and
        // 24000 B/s stereo is NOT present on every Windows build). The old code
        // asked for 24000 once, and when AddStream said no it just set
        // hasAudio = false and produced a VIDEO-ONLY file, silently — which is
        // exactly the "export has no sound" bug, regardless of audio source.
        // So: walk the list downwards until one is accepted, and say loudly in
        // the debug log when none is.
        if (hasAudio)
        {
            static const UINT32 kAacByteRates[] = { 24000, 20000, 16000, 12000 };
            bool audioReady = false;
            HRESULT lastAdd = E_FAIL, lastIn = E_FAIL;

            for (UINT32 bps : kAacByteRates)
            {
                if (audioChannels < 2 && bps > 16000) continue;   // mono tops out lower

                IMFMediaType* aOut = nullptr;
                MFCreateMediaType (&aOut);
                aOut->SetGUID   (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                aOut->SetGUID   (MF_MT_SUBTYPE,    MFAudioFormat_AAC);
                aOut->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                aOut->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) audioRate);
                aOut->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, (UINT32) audioChannels);
                aOut->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bps);
                aOut->SetUINT32 (MF_MT_AAC_PAYLOAD_TYPE, 0);
                lastAdd = writer->AddStream (aOut, &audioIndex);
                aOut->Release();

                if (FAILED (lastAdd))
                {
                    DBG ("ALTER REC: AAC AddStream failed at " << (int) bps
                         << " B/s, rate " << audioRate << ", ch " << audioChannels
                         << " -> hr 0x" << juce::String::toHexString ((int) lastAdd));
                    continue;
                }

                IMFMediaType* aIn = nullptr;
                MFCreateMediaType (&aIn);
                aIn->SetGUID   (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                aIn->SetGUID   (MF_MT_SUBTYPE,    MFAudioFormat_PCM);
                aIn->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                aIn->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) audioRate);
                aIn->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, (UINT32) audioChannels);
                aIn->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32) (audioChannels * 2));
                aIn->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) (audioRate * audioChannels * 2));
                aIn->SetUINT32 (MF_MT_ALL_SAMPLES_INDEPENDENT, 1);
                lastIn = writer->SetInputMediaType (audioIndex, aIn, nullptr);
                aIn->Release();

                if (SUCCEEDED (lastIn)) { audioReady = true;
                                          DBG ("ALTER REC: AAC ok at " << (int) bps << " B/s, "
                                               << audioRate << " Hz, " << audioChannels << " ch");
                                          break; }

                DBG ("ALTER REC: PCM SetInputMediaType failed (AAC " << (int) bps
                     << " B/s) -> hr 0x" << juce::String::toHexString ((int) lastIn));
            }

            juce::ignoreUnused (lastAdd, lastIn);   // only read by DBG

            if (! audioReady)
            {
                DBG ("ALTER REC: *** NO AUDIO TRACK *** the MF AAC encoder rejected every "
                     "configuration (rate " << audioRate << " Hz, " << audioChannels
                     << " ch). Export will be video only.");
                hasAudio = false;
            }
        }

        // ---- encoder rate control (best effort; ignored if the codec says no) ----
        // The MF H.264 encoder defaults to CBR, which wastes bits on the HUD's
        // static dark background and then starves during fast motion — visible as
        // blocking exactly when the visuals get interesting. VBR around the same
        // target is a much better fit for this content. Runs AFTER both streams
        // exist (see the note above the audio block).
        {
            ICodecAPI* codec = nullptr;
            if (SUCCEEDED (writer->GetServiceForStream (videoIndex, GUID_NULL,
                                                        IID_PPV_ARGS (&codec)))
                && codec != nullptr)
            {
                VARIANT v {};
                v.vt = VT_UI4;

                // Prefer QUALITY-based rate control. Targeting an average bitrate is
                // the wrong instrument for this content: ALTER's frames swing between
                // an almost empty dark field and a screen full of strokes, and a VBR
                // controller aiming at a mean spends the budget on the cheap frames
                // and starves the expensive ones — which is exactly where the eye is
                // looking. Quality mode holds a constant quantiser instead, so the
                // busy frames keep their detail and the dark ones simply cost less.
                // Not every encoder exposes it (the NVIDIA MFT does), so fall back.
                bool qualityMode = false;
                {
                    v.ulVal = (ULONG) eAVEncCommonRateControlMode_Quality;
                    if (SUCCEEDED (codec->SetValue (&CODECAPI_AVEncCommonRateControlMode, &v)))
                    {
                        v.ulVal = 85;   // 0..100; 85 is visually clean without absurd files
                        qualityMode = SUCCEEDED (codec->SetValue (&CODECAPI_AVEncCommonQuality, &v));
                    }
                }

                if (! qualityMode)
                {
                    v.ulVal = (ULONG) eAVEncCommonRateControlMode_UnconstrainedVBR;
                    codec->SetValue (&CODECAPI_AVEncCommonRateControlMode, &v);
                }

                // The bitrate goes in EITHER WAY. In quality mode an encoder is
                // free to ignore it — but "free to ignore" is not "must ignore",
                // and one that reads it as a ceiling will otherwise fall back to
                // its own default, which is sized for streaming and nowhere near
                // what this content needs. Leaving it unset was a bet on every
                // encoder behaving the same; setting it costs nothing and removes
                // the bet.
                v.ulVal = (ULONG) bitrate;
                codec->SetValue (&CODECAPI_AVEncCommonMeanBitRate, &v);

                v.ulVal = (ULONG) juce::jmin<double> (100000000.0, bitrate * 1.5);
                codec->SetValue (&CODECAPI_AVEncCommonMaxBitRate, &v);

                // Spend the silicon. This is an offline export of a short clip, not
                // a live stream, so the fastest preset buys us nothing and costs
                // detail on exactly the thin strokes this app is made of.
                v.ulVal = 100;   // 0 = fastest ... 100 = best quality
                codec->SetValue (&CODECAPI_AVEncCommonQualityVsSpeed, &v);

                // Keyframe every 2 s: scrubbing stays usable and seeking in an
                // editor does not have to decode half the file.
                v.ulVal = (ULONG) (fps * 2);
                codec->SetValue (&CODECAPI_AVEncMPVGOPSize, &v);

                DBG ("ALTER REC: rate control = " << (qualityMode ? "quality 85, " : "VBR, ")
                     << (int) (bitrate / 1000000) << " Mbit/s at "
                     << width << "x" << height);
                codec->Release();
            }
        }

        if (FAILED (writer->BeginWriting())) { close(); return false; }
        ok = true;
        DBG ("ALTER REC: MP4 open " << width << "x" << height << " @" << fps
             << " fps, audio " << (hasAudio ? "YES" : "NO"));
        return true;
    }

    void writeVideo (const void* bgra, int sizeBytes, juce::int64 timeHns)
    {
        if (! ok) return;
        IMFMediaBuffer* buf = nullptr;
        if (FAILED (MFCreateMemoryBuffer ((DWORD) sizeBytes, &buf))) return;
        BYTE* dst = nullptr; DWORD maxLen = 0;
        if (SUCCEEDED (buf->Lock (&dst, &maxLen, nullptr)))
        {
            std::memcpy (dst, bgra, (size_t) sizeBytes);
            buf->Unlock();
            buf->SetCurrentLength ((DWORD) sizeBytes);
            IMFSample* s = nullptr;
            if (SUCCEEDED (MFCreateSample (&s)))
            {
                s->AddBuffer (buf);
                s->SetSampleTime (timeHns);
                s->SetSampleDuration (10000000LL / juce::jmax (1, fps));
                writer->WriteSample (videoIndex, s);
                s->Release();
            }
        }
        buf->Release();
    }

    /** Scale + letterbox `src` STRAIGHT into the encoder's own buffer: one pass,
        one copy. (The old path built two juce::Images, ran JUCE's general-purpose
        software resampler and copied the frame four times, per frame.) */
    void writeVideoScaled (const std::uint8_t* srcBGRA, int sw, int sh, juce::int64 timeHns,
                           juce::ThreadPool* scalePool)
    {
        if (! ok || srcBGRA == nullptr || sw < 1 || sh < 1) return;

        const DWORD bytes = (DWORD) ((size_t) width * (size_t) height * 4);
        IMFMediaBuffer* buf = nullptr;
        if (FAILED (MFCreateMemoryBuffer (bytes, &buf))) return;

        BYTE* dst = nullptr; DWORD maxLen = 0;
        if (SUCCEEDED (buf->Lock (&dst, &maxLen, nullptr)))
        {
            scaleLetterboxBGRA (srcBGRA, sw, sh, dst, width, height, scalePool);
            buf->Unlock();
            buf->SetCurrentLength (bytes);

            IMFSample* s = nullptr;
            if (SUCCEEDED (MFCreateSample (&s)))
            {
                s->AddBuffer (buf);
                s->SetSampleTime (timeHns);
                s->SetSampleDuration (10000000LL / juce::jmax (1, fps));
                writer->WriteSample (videoIndex, s);
                s->Release();
            }
        }
        buf->Release();
    }

    void writeAudio (const juce::int16* pcm, int numSamples, juce::int64 timeHns)
    {
        if (! ok || ! hasAudio || numSamples <= 0) return;
        const int bytes = numSamples * 2;
        IMFMediaBuffer* buf = nullptr;
        if (FAILED (MFCreateMemoryBuffer ((DWORD) bytes, &buf))) return;
        BYTE* dst = nullptr; DWORD maxLen = 0;
        if (SUCCEEDED (buf->Lock (&dst, &maxLen, nullptr)))
        {
            std::memcpy (dst, pcm, (size_t) bytes);
            buf->Unlock();
            buf->SetCurrentLength ((DWORD) bytes);
            IMFSample* s = nullptr;
            if (SUCCEEDED (MFCreateSample (&s)))
            {
                s->AddBuffer (buf);
                s->SetSampleTime (timeHns);
                s->SetSampleDuration ((juce::int64) numSamples * 10000000LL
                                      / juce::jmax (1, audioRate * audioChannels));
                writer->WriteSample (audioIndex, s);
                s->Release();
            }
        }
        buf->Release();
    }

    void close()
    {
        if (writer)
        {
            if (ok) writer->Finalize();
            writer->Release();
            writer = nullptr;
        }
        if (mfStarted) { MFShutdown(); mfStarted = false; }
        ok = false;
    }
};

// ── interleaved-stereo sample-rate conversion ─────────────────────────────────
// The Media Foundation AAC encoder accepts 44100 and 48000 Hz and NOTHING ELSE.
// WASAPI loopback hands us whatever the output device's mix format is, which on a
// producer's interface is very often 96 or 192 kHz — AddStream() then failed, the
// code quietly set hasAudio = false, and the export came out SILENT. macOS never
// hit this because AVAssetWriter resamples on its own, which is why it looked like
// a Windows-only bug. Convert to 48 kHz ourselves so it cannot happen.
struct HudRecorder::StereoResampler
{
    juce::WindowedSincInterpolator interp[2];
    std::vector<float> pending[2], scratch[2];
    double ratio = 1.0;                       // input samples consumed per output sample

    void prepare (int srcRate, int dstRate)
    {
        ratio = (double) juce::jmax (1, srcRate) / (double) juce::jmax (1, dstRate);
        for (auto& i : interp)  i.reset();
        for (auto& p : pending) p.clear();
    }

    /** Appends the converted signal to `dst`. Both sides are interleaved stereo. */
    void process (const std::vector<float>& in, std::vector<float>& dst)
    {
        const size_t frames = in.size() / 2;
        if (frames == 0) return;

        for (int ch = 0; ch < 2; ++ch)
        {
            auto& p = pending[(size_t) ch];
            p.reserve (p.size() + frames);
            for (size_t i = 0; i < frames; ++i)
                p.push_back (in[i * 2 + (size_t) ch]);
        }

        const int avail = (int) pending[0].size();
        // Leave a margin so the sinc window never reads past what we have.
        const int numOut = (int) std::floor ((double) avail / ratio) - 8;
        if (numOut <= 0) return;

        int used = 0;
        for (int ch = 0; ch < 2; ++ch)
        {
            scratch[(size_t) ch].resize ((size_t) numOut);
            used = interp[(size_t) ch].process (ratio, pending[(size_t) ch].data(),
                                                scratch[(size_t) ch].data(), numOut);
        }
        used = juce::jlimit (0, avail, used);

        dst.reserve (dst.size() + (size_t) numOut * 2);
        for (int i = 0; i < numOut; ++i)
        {
            dst.push_back (scratch[0][(size_t) i]);
            dst.push_back (scratch[1][(size_t) i]);
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            auto& p = pending[(size_t) ch];
            p.erase (p.begin(), p.begin() + used);
        }
    }
};

// ── Windows capture thread ────────────────────────────────────────────────────
// Everything about grabbing pixels on Windows lives here, off the message thread.
struct HudRecorder::GrabThread : public juce::Thread
{
    HudRecorder& owner;
    HWND hwnd = nullptr;
    bool wholeScreen = false;
    bool fixedRect   = false;
    RECT rect {};

    // GDI resources are created ONCE and reused for the whole recording. The old
    // code did GetDC + CreateCompatibleDC + CreateCompatibleBitmap + GetDIBits on
    // EVERY frame; GetDIBits on a device-dependent bitmap is a driver-side format
    // conversion plus a second full copy of the frame. A DIB section is plain
    // memory that BitBlt writes into directly, so both disappear.
    HDC     screenDC = nullptr, memDC = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldObj = nullptr;
    void*   dibBits = nullptr;
    int     dibW = 0, dibH = 0;
    int     lastW = -1, lastH = -1;
    RECT    lockedScreen {};                 // whole-screen: display chosen at start
    bool    screenLocked = false;

    explicit GrabThread (HudRecorder& o) : juce::Thread ("ALTER Capture"), owner (o) {}

    ~GrabThread() override
    {
        stopThread (2000);
        releaseDib();
        if (memDC    != nullptr) { DeleteDC (memDC); memDC = nullptr; }
        if (screenDC != nullptr) { ReleaseDC (nullptr, screenDC); screenDC = nullptr; }
    }

    void releaseDib()
    {
        if (memDC != nullptr && oldObj != nullptr) { SelectObject (memDC, oldObj); oldObj = nullptr; }
        if (dib != nullptr) { DeleteObject (dib); dib = nullptr; }
        dibBits = nullptr;
        dibW = dibH = 0;
    }

    bool ensureDib (int w, int h)
    {
        if (dib != nullptr && w == dibW && h == dibH) return true;
        releaseDib();

        if (screenDC == nullptr) screenDC = GetDC (nullptr);
        if (screenDC == nullptr) return false;
        if (memDC == nullptr)    memDC = CreateCompatibleDC (screenDC);
        if (memDC == nullptr)    return false;

        BITMAPINFO bi {};
        bi.bmiHeader.biSize        = sizeof (BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;                 // top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        dib = CreateDIBSection (screenDC, &bi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
        if (dib == nullptr || dibBits == nullptr) { releaseDib(); return false; }

        oldObj = SelectObject (memDC, dib);
        dibW = w; dibH = h;
        return true;
    }

    /** The screen rectangle (physical pixels) to blit for this frame. */
    bool sourceRect (RECT& out)
    {
        if (fixedRect)
        {
            out = rect;
            return (rect.right - rect.left) >= 2 && (rect.bottom - rect.top) >= 2;
        }

        if (wholeScreen)
        {
            // ONLY the display the HUD is on. The old code used SM_CXVIRTUALSCREEN,
            // i.e. the entire multi-monitor desktop: a three-screen setup paid for
            // three times the pixels and then produced a frame containing two
            // screens nobody asked for.
            //
            // The display is resolved ONCE and then held for the whole take. If the
            // user drags the HUD to another screen mid-recording we keep filming the
            // one they started on, rather than silently switching (and, on displays
            // of different sizes, letterboxing the rest of the clip).
            if (! screenLocked)
            {
                MONITORINFO mi { sizeof (MONITORINFO) };
                HMONITOR mon = (hwnd != nullptr && IsWindow (hwnd))
                                 ? MonitorFromWindow (hwnd, MONITOR_DEFAULTTOPRIMARY)
                                 : MonitorFromPoint (POINT { 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
                if (mon == nullptr || ! GetMonitorInfo (mon, &mi)) return false;
                lockedScreen = mi.rcMonitor;
                screenLocked = true;
            }
            out = lockedScreen;
            return true;
        }

        if (hwnd == nullptr || ! IsWindow (hwnd)) return false;

        RECT cr {};
        POINT tl { 0, 0 };
        if (! GetClientRect (hwnd, &cr) || ! ClientToScreen (hwnd, &tl)) return false;

        out.left   = tl.x;
        out.top    = tl.y;
        out.right  = tl.x + (cr.right  - cr.left);
        out.bottom = tl.y + (cr.bottom - cr.top);
        return true;
    }

    void run() override
    {
        const double period = 1000.0 / (double) HudRecorder::fps;
        double next = juce::Time::getMillisecondCounterHiRes();

        while (! threadShouldExit())
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (now < next)
            {
                wait ((int) juce::jmax (1.0, next - now));
                continue;
            }

            next += period;
            if (next < now)             // fell behind (encoder busy): resync, don't burst
                next = now + period;

            // Encoder still catching up — don't even pay for the grab.
            if (! owner.queueHasRoom()) { ++owner.dbgSkipped; owner.wakeWriter(); continue; }

            RECT r {};
            if (! sourceRect (r)) continue;

            const int w = (r.right - r.left) & ~1;
            const int h = (r.bottom - r.top) & ~1;
            if (w < 2 || h < 2) continue;

            // While the HUD is being dragged/resized the size changes every frame;
            // capturing mid-relayout gives a torn grab. Skip one frame and settle.
            if (w != lastW || h != lastH) { lastW = w; lastH = h; continue; }

            if (! ensureDib (w, h)) continue;

            // NOTE: no CAPTUREBLT. It forces the layered-window compositing path and
            // costs several milliseconds per call; since Windows 8 the screen DC is
            // already the DWM-composited desktop, so it buys us nothing here.
            if (! BitBlt (memDC, 0, 0, w, h, screenDC, r.left, r.top, SRCCOPY)) continue;
            GdiFlush();

            HudRecorder::RawFrame f;
            f.bgra = owner.acquireBuffer ((size_t) w * (size_t) h * 4);
            std::memcpy (f.bgra.data(), dibBits, (size_t) w * (size_t) h * 4);
            f.w = w;
            f.h = h;
            f.timeHns = (juce::int64) ((juce::Time::getMillisecondCounterHiRes()
                                        - owner.startMs) * 10000.0);
            ++owner.dbgGrabbed;
            owner.pushFrame (std::move (f));
        }
    }
};

#elif JUCE_MAC

// Implemented in HudCaptureMac.mm (AVFoundation AVAssetWriter, H.264 + AAC).
void* alterMp4Create();
bool  alterMp4Open          (void*, const char*, int, int, int, int, int, bool);
void  alterMp4WriteVideoBGRA (void*, const void*, int, int, long long);
void  alterMp4WriteAudioPCM  (void*, const short*, int, int, long long);
void  alterMp4Close         (void*);
void  alterMp4Destroy       (void*);

struct HudRecorder::Mp4Encoder
{
    void* h = nullptr;
    bool  ok = false, hasAudio = false;
    int   width = 0, height = 0, audioChannels = 1;

    ~Mp4Encoder() { close(); }

    bool open (const juce::File& file, int w, int hgt, int frameRate, int aRate, int aChannels,
               bool withAudio)
    {
        width = w & ~1; height = hgt & ~1; hasAudio = withAudio;
        audioChannels = juce::jmax (1, aChannels);
        h = alterMp4Create();
        const juce::String path = file.getFullPathName();
        ok = alterMp4Open (h, path.toRawUTF8(), width, height, frameRate, aRate, audioChannels,
                           withAudio);
        return ok;
    }
    void writeVideo (const void* bgra, int /*sizeBytes*/, juce::int64 timeHns)
    {
        if (ok) alterMp4WriteVideoBGRA (h, bgra, width, height, (long long) timeHns);
    }
    void writeAudio (const juce::int16* pcm, int numSamples, juce::int64 timeHns)
    {
        if (ok && hasAudio)
            alterMp4WriteAudioPCM (h, (const short*) pcm, numSamples, audioChannels, (long long) timeHns);
    }
    void close()
    {
        if (h) { if (ok) alterMp4Close (h); alterMp4Destroy (h); h = nullptr; }
        ok = false;
    }
};

#else
struct HudRecorder::Mp4Encoder {};
#endif

// Scale `src` to FIT inside `dst` (aspect preserved) and centre it on black —
// the size presets must never distort the HUD.
static void letterboxInto (juce::Image& dst, const juce::Image& src)
{
    juce::Graphics dg (dst);
    dg.fillAll (juce::Colours::black);
    if (! src.isValid()) return;
    const double s = juce::jmin ((double) dst.getWidth()  / (double) src.getWidth(),
                                 (double) dst.getHeight() / (double) src.getHeight());
    const float dw = (float) (src.getWidth()  * s);
    const float dh = (float) (src.getHeight() * s);
    dg.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    dg.drawImage (src, juce::Rectangle<float> (((float) dst.getWidth()  - dw) * 0.5f,
                                               ((float) dst.getHeight() - dh) * 0.5f, dw, dh));
}

static juce::Image makeEven (juce::Image img)
{
    if (! img.isValid()) return img;
    const int w = img.getWidth() & ~1, h = img.getHeight() & ~1;
    if (w <= 0 || h <= 0) return {};
    if (w != img.getWidth() || h != img.getHeight())
        img = img.rescaled (w, h);
    return img;
}

// Encoders want even dimensions; keep everything at least 2 px.
static inline int evenDim (double v) noexcept
{
    return juce::jmax (2, juce::roundToInt (v) & ~1);
}

void HudRecorder::resolveOutputSize (int srcW, int srcH, int& outW, int& outH) const noexcept
{
    if (srcW < 2 || srcH < 2) { outW = outH = 0; return; }

    if (tgtW < 2)                       // native size
    {
        // A 5K display would otherwise go straight into the encoder at 5120x2880.
        const int longSide = juce::jmax (srcW, srcH);
        const double s = longSide > maxCaptureDim ? (double) maxCaptureDim / (double) longSide : 1.0;
        outW = evenDim (srcW * s);
        outH = evenDim (srcH * s);
    }
    else if (tgtH < 2)                  // width-driven, height follows the HUD aspect
    {
        outW = evenDim (tgtW);
        outH = evenDim ((double) outW * (double) srcH / (double) srcW);
    }
    else                                // exact frame (custom): fit + letterbox
    {
        outW = evenDim (tgtW);
        outH = evenDim (tgtH);
    }
}

HudRecorder::HudRecorder() : juce::Thread ("HUD Recorder Writer") {}
HudRecorder::~HudRecorder() { stop(); }

// ── frame buffer pool ─────────────────────────────────────────────────────────
std::vector<std::uint8_t> HudRecorder::acquireBuffer (size_t bytes)
{
    {
        const juce::ScopedLock sl (poolLock);
        for (size_t i = 0; i < bufferPool.size(); ++i)
        {
            if (bufferPool[i].capacity() >= bytes)
            {
                auto b = std::move (bufferPool[i]);
                bufferPool.erase (bufferPool.begin() + (std::ptrdiff_t) i);
                b.resize (bytes);
                return b;
            }
        }
    }
    return std::vector<std::uint8_t> (bytes);
}

void HudRecorder::recycleBuffer (std::vector<std::uint8_t>&& b)
{
    if (b.capacity() == 0) return;
    const juce::ScopedLock sl (poolLock);
    if ((int) bufferPool.size() < maxQueued + 2)
        bufferPool.push_back (std::move (b));
}

bool HudRecorder::queueHasRoom() const
{
    const juce::ScopedLock sl (qLock);
    return (int) queue.size() < maxQueued;
}

void HudRecorder::pushFrame (RawFrame&& f)
{
    bool taken = false;
    {
        const juce::ScopedLock sl (qLock);
        if ((int) queue.size() < maxQueued)
        {
            queue.push_back (std::move (f));
            taken = true;
        }
    }
    if (! taken) recycleBuffer (std::move (f.bgra));   // dropped: give the memory back
    notify();
}

bool HudRecorder::start (juce::Component* t, juce::File outputFile)
{
    if (recording.load() || t == nullptr || outputFile == juce::File()) return false;

    target = t;
    frameCount = 0;
    headerWritten = false;
    recW = recH = 0;
    audioTotalSamples = 0;
    audioWrittenSamples = 0;
    movAudioWritten = 0;
    index.clear();
    audioPcm.clear();
    dbgSamples = 0; dbgPeak = 0.0f; dbgLastLogSec = 0.0;
    dbgPolls = dbgPollsEmpty = 0;
    lastLogicalW = lastLogicalH = -1;
    { const juce::ScopedLock sl (qLock); queue.clear(); }
    { const juce::ScopedLock sl (poolLock); bufferPool.clear(); }

   #if JUCE_WINDOWS
    // Resolve the native window HERE, on the message thread — the capture thread
    // must never touch juce::Component. The HWND itself is safe to use from any
    // thread (GetClientRect / ClientToScreen / BitBlt are cross-thread calls).
    HWND hwnd = nullptr;
    if (auto* topComp = t->getTopLevelComponent())
        if (auto* peer = topComp->getPeer())
            hwnd = (HWND) peer->getNativeHandle();
   #endif

    startMs = juce::Time::getMillisecondCounterHiRes();

   #if JUCE_WINDOWS || JUCE_MAC
    useMp4 = true;                                   // Media Foundation / AVFoundation
    if (! outputFile.hasFileExtension ("mp4"))
        outputFile = outputFile.withFileExtension ("mp4");
   #else
    useMp4 = false;                                  // MJPEG .avi fallback
    if (! outputFile.hasFileExtension ("avi"))
        outputFile = outputFile.withFileExtension ("avi");
   #endif

    // A transparent take is a different container entirely, so it overrides the
    // platform choice above before the file is created.
    if (alphaGrab != nullptr)
    {
        useMp4 = false;
        outputFile = outputFile.withFileExtension (alphaOutputExtension());
    }

    outputFile.getParentDirectory().createDirectory();
    outFile = outputFile;
    outFile.deleteFile();

    alphaOut.reset();

    if (alphaGrab != nullptr)
    {
        // The .mov is opened lazily on the first frame, like the MP4 encoder: the
        // frame size is not known until we have actually grabbed one.
        alphaOut = std::make_unique<AlphaWriter>();
    }
    else if (! useMp4)
    {
        // AVI path writes the container itself; MP4 (MF) creates the file in the worker.
        avi = std::make_unique<juce::FileOutputStream> (outFile);
        if (! avi->openedOk()) { avi.reset(); return false; }

        // Same trap as the .mov writer: FileOutputStream APPENDS to an existing
        // file, and deleteFile() above fails silently while something else has it
        // open. Truncate so re-recording over a take really replaces it.
        avi->setPosition (0);
        avi->truncate();
    }

    // ── audio source ──────────────────────────────────────────────────────────
    audioChannels = 2;                                // interleaved stereo either way
    audioScratch.clear();

    if (externalPull != nullptr)
    {
        // Audio comes from a chosen VST plugin instance — no system tap at all, so
        // the recording contains ONLY that track (not whatever else is playing).
        audioTap.reset();
        captureRate = juce::jlimit (8000, 192000, externalRate > 0 ? externalRate : 48000);
        // Drop whatever the source buffered before the user hit Record.
        { std::vector<float> discard; externalPull (discard); }
    }
    else
    {
        // system-audio tap (best effort) — created on the message thread (STA), like the
        // app's own capture; the worker only drains it.
        audioTap = std::make_unique<AudioTap>();
        captureRate = juce::jlimit (8000, 192000, audioTap->rate());
    }
    audioRate = captureRate;
    hasAudio = true;

   #if JUCE_WINDOWS
    // The Media Foundation AAC encoder takes 44.1 or 48 kHz ONLY. A 96/192 kHz
    // output device (or plugin stream) made AddStream() fail, which silently
    // dropped the whole audio track. Convert instead of losing the sound.
    resampler.reset();
    resampledScratch.clear();
    if (useMp4 && captureRate != 44100 && captureRate != 48000)
    {
        audioRate = 48000;
        resampler = std::make_unique<StereoResampler>();
        resampler->prepare (captureRate, audioRate);
    }
   #endif

    // Scaling workers: leave one core for the encoder and one for the message
    // thread. Above Full HD this is what decides whether the export holds 30 fps.
    scalePool = std::make_unique<juce::ThreadPool> (
                    juce::ThreadPoolOptions{}
                        .withNumberOfThreads (juce::jlimit (1, 7,
                            juce::SystemStats::getNumCpus() - 2))
                        .withThreadName ("ALTER Scale"));

    recording = true;
    startThread();

    // An alpha take paints COMPONENTS, which only the message thread may touch,
    // so it always uses the timer — never the BitBlt capture thread.
    if (alphaGrab != nullptr)
    {
        startTimerHz (fps);
    }
    else
    {
       #if JUCE_WINDOWS
        // Windows grabs pixels on its own thread; the message thread is left alone.
        grabber = std::make_unique<GrabThread> (*this);
        grabber->hwnd = hwnd;
        grabber->wholeScreen = (captureSource == CaptureSource::WholeScreen);
    grabber->fixedRect   = (captureSource == CaptureSource::ScreenRect);
    grabber->rect        = { captureRect.getX(), captureRect.getY(),
                             captureRect.getRight(), captureRect.getBottom() };
        grabber->startThread (juce::Thread::Priority::high);
       #else
        startTimerHz (fps);
       #endif
    }

    return true;
}

void HudRecorder::stop()
{
    if (! recording.load()) return;
    recording = false;
    stopTimer();

   #if JUCE_WINDOWS
    // Stop grabbing FIRST, so the writer thread drains a queue that is no longer
    // growing and the encoder finalises against a fixed amount of work.
    if (grabber != nullptr) { grabber->stopThread (2000); grabber.reset(); }
   #endif

    signalThreadShouldExit();
    notify();

    // The writer thread finishes with Mp4Encoder::close() → SinkWriter::Finalize(),
    // which legitimately takes a while (it drains the whole HW encoder queue and
    // writes the moov). The old stopThread(8000) KILLED the thread mid-Finalize
    // when that ran long — Media Foundation was left with internal locks held,
    // after which the app could not shut down (the "frozen after recording" bug).
    // Wait patiently instead; only hard-kill as a last resort after 60 s.
    if (! waitForThreadToExit (60000))
        stopThread (1000);   // pathological encoder hang — accept the risk

    // Which counter matters depends on the pipeline: the MP4 encoder and the .mov
    // writer keep their own. Reporting the MP4 one for a transparent take printed
    // a confident "0 samples" over an audio track that was in fact being written.
    const juce::int64 written = (alphaGrab != nullptr) ? movAudioWritten : audioWrittenSamples;

    DBG ("ALTER REC: finished — " << (int) written
         << " interleaved samples written to the audio track ("
         << juce::String (written / (double) juce::jmax (1, audioRate * audioChannels), 2)
         << " s at " << audioRate << " Hz)");

    audioTap.reset();
   #if JUCE_WINDOWS
    resampler.reset();
   #endif
    scalePool.reset();
    { const juce::ScopedLock sl (poolLock); bufferPool.clear(); }
}

void HudRecorder::timerCallback()
{
    if (! recording.load()) return;
    auto* c = target.getComponent();
    if (c == nullptr) return;

    // If the encoder worker is still behind, skip this frame entirely instead of
    // doing the (expensive) screen grab and then throwing it away. This keeps the
    // message thread responsive so the UI never freezes under heavy load.
    {
        // A transparent take batches its compression, so it needs somewhere to put
        // the batch. With the old depth the queue was full almost permanently and
        // the grab simply gave up 26 times a second.
        const int depth = (alphaGrab != nullptr) ? maxAlphaBatch * 2 : maxQueued;
        const juce::ScopedLock sl (qLock);
        if ((int) queue.size() >= depth) { notify(); return; }
    }

    RawFrame frame;

    if (alphaGrab != nullptr)
    {
        // Straight from the component tree, alpha intact. Nothing here touches the
        // screen, so what is behind the HUD — the DAW, the desktop — cannot leak
        // into the recording the way a screen grab would let it.
        //
        // Asked for at the FINISHED size: this runs on the message thread 30 times
        // a second, so every pixel painted here and discarded later is paid for in
        // UI responsiveness.
        int ow = 0, oh = 0;
        resolveOutputSize (c->getWidth(), c->getHeight(), ow, oh);
        if (ow >= 2 && oh >= 2)
            frame.image = alphaGrab (ow, oh);
    }
    else
    {
        frame = capture (c);
    }

    if (frame.valid())
    {
        frame.timeHns = (juce::int64) ((juce::Time::getMillisecondCounterHiRes() - startMs) * 10000.0);
        const int depth = (alphaGrab != nullptr) ? maxAlphaBatch * 2 : maxQueued;
        const juce::ScopedLock sl (qLock);
        if ((int) queue.size() < depth)
            queue.push_back (std::move (frame));
    }
    notify();
}

void HudRecorder::pollAudio()
{
    // Both paths deliver INTERLEAVED STEREO floats; convert to interleaved 16-bit.
    audioScratch.clear();

    bool got = false;
    if (externalPull != nullptr)      got = externalPull (audioScratch) > 0;
    else if (audioTap != nullptr)     got = audioTap->pull (audioScratch);

    // Diagnostic: how much signal the SOURCE actually delivered, once a second.
    // Reported EVEN WHEN NOTHING ARRIVES — a run with no line at all told us only
    // that the loop never got past the early return, which is the least useful
    // possible answer. Zero here means the problem is upstream of the recorder
    // (WASAPI loopback hears nothing because the DAW holds the device in ASIO /
    // exclusive mode, or the plugin's UDP 'W' stream is not advancing); a healthy
    // peak here with a silent file means the problem is the encoder.
    {
        float pk = 0.0f;
        for (float f : audioScratch) pk = juce::jmax (pk, std::abs (f));
        dbgSamples += (juce::int64) audioScratch.size();
        dbgPeak = juce::jmax (dbgPeak, pk);
        ++dbgPolls;
        if (! got) ++dbgPollsEmpty;

        const double nowSec = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (nowSec - dbgLastLogSec > 1.0)
        {
            dbgLastLogSec = nowSec;
            DBG ("ALTER REC: audio in " << (int) dbgSamples << " samples, peak "
                 << juce::String (dbgPeak, 4)
                 << (externalPull != nullptr ? "  (plugin)" : "  (system mix)")
                 << "  [" << (int) dbgPollsEmpty << "/" << (int) dbgPolls
                 << " polls empty]");
            dbgSamples = 0;
            dbgPeak = 0.0f;
            dbgPolls = dbgPollsEmpty = 0;
        }
    }

    if (! got || audioScratch.empty()) return;

   #if JUCE_WINDOWS
    // Convert to a rate the AAC encoder will actually accept (see StereoResampler).
    if (resampler != nullptr)
    {
        resampledScratch.clear();
        resampler->process (audioScratch, resampledScratch);
        if (resampledScratch.empty()) return;
        audioScratch.swap (resampledScratch);
    }
   #endif

    audioPcm.reserve (audioPcm.size() + audioScratch.size());
    for (float f : audioScratch)
    {
        f = std::isfinite (f) ? juce::jlimit (-1.0f, 1.0f, f) : 0.0f;
        audioPcm.push_back ((juce::int16) juce::roundToInt (f * 32767.0f));
    }
}

void HudRecorder::run()
{
    auto popOne = [this] (RawFrame& out) -> bool
    {
        const juce::ScopedLock sl (qLock);
        if (queue.empty()) return false;
        out = std::move (queue.front());
        queue.pop_front();
        return true;
    };

   #if JUCE_WINDOWS
    // Media Foundation needs COM (MTA) on this worker.
    const bool comInited = (useMp4 && SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED)));
   #endif
   #if JUCE_WINDOWS || JUCE_MAC
    // Encoder is created lazily on the first frame (it needs the frame size) in processFrame().
    if (useMp4) mp4 = std::make_unique<Mp4Encoder>();
   #endif

    while (! threadShouldExit())
    {
        pollAudio();
        flushAudioToEncoder();
        flushAudioToMov();

        if (alphaOut != nullptr)
        {
            // DRAIN FIRST, THEN COMPRESS. The batch is the entire point of this
            // path and the previous arrangement destroyed it: one frame was popped
            // per iteration and encodeAlphaBatch ran on every iteration, so it
            // almost always found exactly one frame waiting and "compressed the
            // batch" on a single thread. The parallelism was real and never had
            // anything to work on — 4.1 fps became 4.6.
            RawFrame f;
            while (popOne (f)) processFrame (std::move (f));   // appends to alphaPending

            if (alphaPending.empty()) wait (5);
            else                      encodeAlphaBatch (false);
        }
        else
        {
            RawFrame f;
            if (popOne (f)) processFrame (std::move (f));
            else            wait (15);
        }

       #if JUCE_DEBUG
        // Frame budget, once a second. `skipped` is the one that matters: it counts
        // grabs the capture thread did NOT take because the writer was still busy,
        // and every one of them is a frame missing from the file.
        if (const double nowSec = juce::Time::getMillisecondCounterHiRes() * 0.001;
            nowSec - dbgFrameLogSec > 1.0)
        {
            dbgFrameLogSec = nowSec;
            const int enc = dbgEncoded.exchange (0);
            const double ms = dbgEncodeMs.exchange (0.0);
            DBG ("ALTER REC: " << dbgGrabbed.exchange (0) << " grabbed, "
                 << dbgSkipped.exchange (0) << " skipped (queue full), "
                 << enc << " encoded, "
                 << juce::String (enc > 0 ? ms / enc : 0.0, 1) << " ms/frame");
        }
       #endif
    }

    pollAudio();
    RawFrame f;
    while (popOne (f)) processFrame (std::move (f));
    encodeAlphaBatch (true);
    pollAudio();
    flushAudioToEncoder();
    flushAudioToMov();

    // The .mov index (sample sizes, offsets, durations) can only be written once
    // every frame is in, which is why the file is finished HERE rather than at the
    // first Stop click.
    if (alphaOut != nullptr)
    {
        flushAudioToMov();
        const bool wrote = alphaOut->mov.close();
        DBG ("ALTER REC: transparent .mov " << (wrote ? "written, " : "FAILED, ")
             << alphaOut->mov.getNumFrames() << " captured frames -> "
             << alphaOut->mov.getNumSlots() << " at " << fps << " fps");
        alphaOut.reset();
        return;
    }

   #if JUCE_WINDOWS || JUCE_MAC
    if (useMp4)
    {
        if (mp4 != nullptr) { mp4->close(); mp4.reset(); }
       #if JUCE_WINDOWS
        if (comInited) CoUninitialize();
       #endif
        return;
    }
   #endif

    // flush any trailing audio (AVI path)
    if (headerWritten && ! audioPcm.empty())
    {
        writeAudioChunk (audioPcm.data(), (int) audioPcm.size());
        audioPcm.clear();
    }

    finalizeAvi();
}

// Drain accumulated PCM into the transparent .mov (worker thread).
//
// SILENCE IS PADDED, exactly as on the MP4 path. It is tempting to think PCM needs
// no such thing because each sample carries its own duration — that is true and it
// is not the point. A PCM track has no timestamps at all: sample N plays at N/rate,
// full stop. So a gap in the source does NOT leave a hole, it CLOSES one: every
// sample after the gap moves earlier by however long the source was quiet, and the
// whole rest of the take drifts out of sync with the picture. The gaps are real,
// too — the plugin's UDP stream stops the moment the DAW does.
//
// Padding against the wall clock keeps sample N where it belongs.
void HudRecorder::flushAudioToMov()
{
    if (alphaOut == nullptr || ! alphaOut->mov.isOpen())
        return;

    const int chans   = juce::jmax (1, audioChannels);
    const int rateXch = juce::jmax (1, audioRate) * chans;

    const double      elapsedSec = (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;
    const juce::int64 expected   = (juce::int64) (elapsedSec * (double) rateXch);

    juce::int64 behind = expected - movAudioWritten - (juce::int64) audioPcm.size();
    behind -= behind % chans;
    while (behind > rateXch / 10)                      // only real gaps (> ~100 ms)
    {
        int n = (int) juce::jmin<juce::int64> (behind, (juce::int64) rateXch);
        n -= n % chans;
        if (n <= 0) break;

        std::vector<juce::int16> silence ((size_t) n, 0);
        alphaOut->mov.addAudio (silence.data(), n, audioRate, chans);
        movAudioWritten += n;
        behind -= n;
    }

    if (audioPcm.empty()) return;

    const int whole = (int) audioPcm.size() - (int) (audioPcm.size() % (size_t) chans);
    if (whole <= 0) return;

    alphaOut->mov.addAudio (audioPcm.data(), whole, audioRate, chans);
    movAudioWritten += whole;
    audioPcm.erase (audioPcm.begin(), audioPcm.begin() + whole);
}

// Drain accumulated PCM into the MP4 encoder (worker thread). No-op for the AVI path.
void HudRecorder::flushAudioToEncoder()
{
   #if JUCE_WINDOWS || JUCE_MAC
    if (! useMp4 || mp4 == nullptr || ! mp4->ok || ! hasAudio) return;

    const int rate = juce::jmax (1, audioRate);
    const int chans = juce::jmax (1, audioChannels);
    const int rateXch = rate * chans;                  // interleaved samples per second

    // System-audio loopback (WASAPI / ScreenCaptureKit) delivers NO data while the
    // source is silent. That would freeze the audio timeline while the video keeps
    // advancing on the wall clock, so after a silent gap the audio lands far behind
    // the video — the muxer then stalls the video to wait for it and the recording
    // visibly stutters. Keep audio aligned with real time by padding silent gaps.
    const double      elapsedSec = (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;
    const juce::int64 expected   = (juce::int64) (elapsedSec * (double) rateXch);

    juce::int64 behind = expected - audioWrittenSamples - (juce::int64) audioPcm.size();
    behind -= behind % chans;                          // keep frame alignment
    while (behind > rateXch / 10)                      // only real gaps (> ~100 ms)
    {
        int n = (int) juce::jmin<juce::int64> (behind, (juce::int64) rateXch);  // ≤ 1 s per write
        n -= n % chans;
        if (n <= 0) break;
        std::vector<juce::int16> silence ((size_t) n, 0);
        mp4->writeAudio (silence.data(), n, audioWrittenSamples * 10000000LL / rateXch);
        audioWrittenSamples += n;
        behind -= n;
    }

    if (! audioPcm.empty())
    {
        // never split a stereo frame across two writes
        const int whole = (int) audioPcm.size() - (int) (audioPcm.size() % (size_t) chans);
        if (whole > 0)
        {
            mp4->writeAudio (audioPcm.data(), whole,
                             audioWrittenSamples * 10000000LL / rateXch);
            audioWrittenSamples += whole;
            audioPcm.erase (audioPcm.begin(), audioPcm.begin() + whole);
        }
    }
   #endif
}


// PNG-compress the frames collected so far, IN PARALLEL, and append them in order.
//
// This is the whole ball game for a transparent export. A 1920x734 frame comes out
// around 460 KB of PNG, and zlib on one thread needs roughly a quarter of a second
// for it in a debug build — so the writer could absorb about four frames a second
// while the grab offered thirty. Everything else was fine: the container said 30 fps
// and the timestamps were honest, so the file faithfully described a slideshow, and
// 86% of it was the same frame repeated.
//
// The frames are independent, so they compress independently. Order is restored on
// the way out because a movie's samples must land in the file in the order they play.
void HudRecorder::encodeAlphaBatch (bool flushAll)
{
    if (alphaOut == nullptr || alphaPending.empty()) return;

    const int batch = flushAll ? (int) alphaPending.size()
                               : juce::jmin ((int) alphaPending.size(), maxAlphaBatch);
    if (batch <= 0) return;

    // Open on the first frame: the size is not known until one exists.
    if (! alphaOut->mov.isOpen())
    {
        int ow = 0, oh = 0;
        resolveOutputSize (alphaPending.front().image.getWidth(),
                           alphaPending.front().image.getHeight(), ow, oh);
        if (ow < 2 || oh < 2) { alphaPending.clear(); return; }
        if (! alphaOut->mov.open (outFile, ow, oh, fps)) { alphaPending.clear(); return; }
    }

    const int ow = alphaOut->mov.getWidth(), oh = alphaOut->mov.getHeight();

    std::vector<juce::MemoryBlock> encoded ((size_t) batch);

    auto encodeOne = [&] (int i)
    {
        auto img = alphaPending[(size_t) i].image;
        if (! img.isValid()) return;

        // Normally already the right size — the grab was asked for it. The rescale
        // is the safety net for a frame straddling a HUD resize; rescaled()
        // resamples alpha with colour, so soft edges stay soft.
        if (img.getWidth() != ow || img.getHeight() != oh)
            img = img.rescaled (ow, oh, juce::Graphics::highResamplingQuality);

        juce::MemoryOutputStream png (encoded[(size_t) i], false);
        juce::PNGImageFormat fmt;
        fmt.writeImageToStream (img, png);
    };

    const int workers = (scalePool != nullptr && batch > 1)
                            ? juce::jlimit (1, batch, scalePool->getNumThreads() + 1) : 1;

    if (workers <= 1)
    {
        for (int i = 0; i < batch; ++i) encodeOne (i);
    }
    else
    {
        std::atomic<int> next { 0 }, done { 0 };
        juce::WaitableEvent allDone;

        auto drain = [&]
        {
            for (;;)
            {
                const int i = next.fetch_add (1);
                if (i >= batch) break;
                encodeOne (i);
            }
            if (done.fetch_add (1) + 1 == workers) allDone.signal();
        };

        for (int t = 1; t < workers; ++t) scalePool->addJob (drain);
        drain();                                    // this thread takes a share too

        while (done.load() < workers) allDone.wait (50);
    }

    const double t0 = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < batch; ++i)
    {
        if (encoded[(size_t) i].getSize() > 0)
        {
            alphaOut->mov.addEncodedFrame (encoded[(size_t) i].getData(),
                                           encoded[(size_t) i].getSize(),
                                           alphaPending[(size_t) i].timeHns / 10000);
            ++dbgEncoded;
        }
    }
    dbgEncodeMs = dbgEncodeMs.load() + (juce::Time::getMillisecondCounterHiRes() - t0);

    alphaPending.erase (alphaPending.begin(), alphaPending.begin() + batch);
}

void HudRecorder::processFrame (RawFrame frame)
{
    // ── transparent .mov ─────────────────────────────────────────────────────
    // Handled in batches by encodeAlphaBatch(), not here: PNG compression is the
    // entire cost of this format and doing it one frame at a time on this thread
    // capped a 1920x734 take at about four frames a second.
    if (alphaOut != nullptr)
    {
        if (! frame.image.isValid()) return;
        alphaPending.push_back (std::move (frame));
        return;
    }

   #if JUCE_WINDOWS
    if (useMp4 && mp4 != nullptr)
    {
        if (frame.w < 2 || frame.h < 2
            || (int) frame.bgra.size() < frame.w * frame.h * 4)
            return;

        if (! mp4->ok)
        {
            // The stream dimensions are decided by the FIRST frame and stay fixed;
            // later frames (HUD resized mid-recording) are letterboxed into them.
            int ow = 0, oh = 0;
            resolveOutputSize (frame.w, frame.h, ow, oh);
            if (ow < 2 || oh < 2) return;
            if (! mp4->open (outFile, ow, oh, fps, audioRate, audioChannels, hasAudio))
                return;
        }

        // Scale (or plain copy) straight into the encoder's buffer — one pass.
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        mp4->writeVideoScaled (frame.bgra.data(), frame.w, frame.h, frame.timeHns,
                               scalePool.get());
        dbgEncodeMs = dbgEncodeMs.load() + (juce::Time::getMillisecondCounterHiRes() - t0);
        ++dbgEncoded;
        recycleBuffer (std::move (frame.bgra));

        flushAudioToEncoder();
        return;
    }
   #endif

   #if JUCE_MAC
    if (useMp4)
    {
        if (mp4 == nullptr) return;

        // Ensure we have BGRA bytes + dimensions (the BitBlt path gives them directly;
        // the rare component-snapshot fallback gives an Image we convert once here).
        if (frame.bgra.empty() && frame.image.isValid())
        {
            const int w = frame.image.getWidth() & ~1, h = frame.image.getHeight() & ~1;
            if (w >= 2 && h >= 2)
            {
                frame.w = w; frame.h = h;
                frame.bgra.resize ((size_t) w * h * 4);
                juce::Image::BitmapData bd (frame.image, juce::Image::BitmapData::readOnly);
                if (bd.pixelStride == 4)
                {
                    // ARGB image: bytes are already B,G,R,A in memory → fast line copy.
                    for (int y = 0; y < h; ++y)
                        std::memcpy (frame.bgra.data() + (size_t) y * w * 4,
                                     bd.getLinePointer (y), (size_t) w * 4);
                }
                else
                {
                    for (int y = 0; y < h; ++y)
                    {
                        std::uint8_t* d = frame.bgra.data() + (size_t) y * w * 4;
                        for (int x = 0; x < w; ++x)
                        {
                            auto p = bd.getPixelColour (x, y);
                            d[x * 4 + 0] = p.getBlue();
                            d[x * 4 + 1] = p.getGreen();
                            d[x * 4 + 2] = p.getRed();
                            d[x * 4 + 3] = 255;
                        }
                    }
                }
            }
        }
        if (frame.w < 2 || frame.h < 2 || (int) frame.bgra.size() < frame.w * frame.h * 4)
            return;

        if (! mp4->ok)
        {
            // The stream dimensions are decided by the FIRST frame and stay fixed;
            // later frames (HUD resized mid-recording) are letterboxed into them.
            int ow = 0, oh = 0;
            resolveOutputSize (frame.w, frame.h, ow, oh);
            if (ow < 2 || oh < 2) return;
            if (! mp4->open (outFile, ow, oh, fps, audioRate, audioChannels, hasAudio))
                return;
        }

        if (frame.w == mp4->width && frame.h == mp4->height)
        {
            mp4->writeVideo (frame.bgra.data(), frame.w * frame.h * 4, frame.timeHns);
        }
        else
        {
            // Frame size ≠ stream size (format preset, or the HUD was resized after
            // the encoder opened — the stream is FIXED). Letterbox the frame into the
            // stream dimensions: aspect preserved, black bars, never distorted.
            juce::Image src (juce::Image::ARGB, frame.w, frame.h, false, juce::SoftwareImageType());
            {
                juce::Image::BitmapData bd (src, juce::Image::BitmapData::writeOnly);
                for (int y = 0; y < frame.h; ++y)
                    std::memcpy (bd.getLinePointer (y),
                                 frame.bgra.data() + (size_t) y * frame.w * 4, (size_t) frame.w * 4);
            }
            juce::Image dst (juce::Image::ARGB, mp4->width, mp4->height, false, juce::SoftwareImageType());
            letterboxInto (dst, src);

            std::vector<std::uint8_t> buf ((size_t) mp4->width * mp4->height * 4);
            {
                juce::Image::BitmapData bd (dst, juce::Image::BitmapData::readOnly);
                for (int y = 0; y < mp4->height; ++y)
                    std::memcpy (buf.data() + (size_t) y * mp4->width * 4,
                                 bd.getLinePointer (y), (size_t) mp4->width * 4);
            }
            mp4->writeVideo (buf.data(), mp4->width * mp4->height * 4, frame.timeHns);
        }

        flushAudioToEncoder();
        return;
    }
   #endif

    if (avi == nullptr) return;

    // Build the full-res image (the per-pixel conversion runs here on the WORKER
    // thread, not the message thread, so the UI stays responsive even at 4K).
    juce::Image img = frame.image;
    if (! img.isValid() && frame.w > 0 && frame.h > 0
        && (int) frame.bgra.size() >= frame.w * frame.h * 4)
    {
        img = juce::Image (juce::Image::ARGB, frame.w, frame.h, false, juce::SoftwareImageType());
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < frame.h; ++y)
        {
            const std::uint8_t* s = frame.bgra.data() + (size_t) y * frame.w * 4;
            std::uint8_t* d = bd.getLinePointer (y);
            for (int x = 0; x < frame.w; ++x)
            {
                d[x * bd.pixelStride + 0] = s[x * 4 + 0];
                d[x * bd.pixelStride + 1] = s[x * 4 + 1];
                d[x * bd.pixelStride + 2] = s[x * 4 + 2];
                d[x * bd.pixelStride + 3] = 255;
            }
        }
    }
    if (! img.isValid()) return;

    // Output size (AVI path): same rules as the MP4 path.
    if (tgtW >= 2)
    {
        int ow = 0, oh = 0;
        resolveOutputSize (img.getWidth(), img.getHeight(), ow, oh);
        if (ow >= 2 && oh >= 2 && (img.getWidth() != ow || img.getHeight() != oh))
        {
            juce::Image dst (juce::Image::ARGB, ow, oh, false, juce::SoftwareImageType());
            letterboxInto (dst, img);
            img = dst;
        }
    }
    else
    // High-quality downscale to the capped long side (worker thread → affordable).
    if (const int longSide = juce::jmax (img.getWidth(), img.getHeight()); longSide > maxCaptureDim)
    {
        const double s = (double) maxCaptureDim / (double) longSide;
        const int ow = juce::jmax (2, (juce::roundToInt (img.getWidth()  * s)) & ~1);
        const int oh = juce::jmax (2, (juce::roundToInt (img.getHeight() * s)) & ~1);
        img = img.rescaled (ow, oh, juce::Graphics::highResamplingQuality);
    }
    img = makeEven (img);
    if (! img.isValid()) return;

    if (! headerWritten)
    {
        recW = img.getWidth();
        recH = img.getHeight();
        if (recW <= 0 || recH <= 0) return;
        writeAviHeader();
        headerWritten = true;
    }

    if (img.getWidth() != recW || img.getHeight() != recH)
        img = img.rescaled (recW, recH);

    juce::MemoryBlock mb;
    {
        juce::MemoryOutputStream mos (mb, false);
        juce::JPEGImageFormat jpg;
        jpg.setQuality (1.0f);   // max quality: neon-on-dark lines need it (JPEG rings otherwise)
        jpg.writeImageToStream (img, mos);
    }
    if (mb.getSize() > 0)
        writeVideoChunk (mb.getData(), (int) mb.getSize());

    // interleave the audio captured so far
    if (hasAudio && ! audioPcm.empty())
    {
        writeAudioChunk (audioPcm.data(), (int) audioPcm.size());
        audioPcm.clear();
    }
}

void HudRecorder::writeAviHeader()
{
    auto w32 = [this] (juce::uint32 v) { avi->writeInt ((int) v); };
    auto w16 = [this] (juce::uint16 v) { avi->writeShort ((short) v); };
    auto wcc = [this] (const char* f) { avi->write (f, 4); };

    const int streams = hasAudio ? 2 : 1;
    const juce::uint32 hdrlSize = hasAudio ? 294u : 192u;

    wcc ("RIFF"); riffSizePos = avi->getPosition(); w32 (0); wcc ("AVI ");

    wcc ("LIST"); w32 (hdrlSize); wcc ("hdrl");

    // ---- avih ----
    wcc ("avih"); w32 (56);
    w32 (1000000u / (juce::uint32) fps);
    w32 (0); w32 (0);
    w32 (0x10);                                  // AVIF_HASINDEX
    totalFramesPos = avi->getPosition(); w32 (0);
    w32 (0);
    w32 ((juce::uint32) streams);
    w32 (0);
    w32 ((juce::uint32) recW); w32 ((juce::uint32) recH);
    w32 (0); w32 (0); w32 (0); w32 (0);

    // ---- video stream ----
    wcc ("LIST"); w32 (116); wcc ("strl");
    wcc ("strh"); w32 (56);
    wcc ("vids"); wcc ("MJPG");
    w32 (0);
    w16 (0); w16 (0);
    w32 (0);
    w32 (1);                                     // dwScale
    w32 ((juce::uint32) fps);                    // dwRate
    w32 (0);
    vidLengthPos = avi->getPosition(); w32 (0);  // dwLength
    w32 (0);
    w32 (0xFFFFFFFFu);
    w32 (0);
    w16 (0); w16 (0); w16 ((juce::uint16) recW); w16 ((juce::uint16) recH);

    wcc ("strf"); w32 (40);
    w32 (40);
    w32 ((juce::uint32) recW); w32 ((juce::uint32) recH);
    w16 (1); w16 (24);
    wcc ("MJPG");
    w32 ((juce::uint32) (recW * recH * 3));
    w32 (0); w32 (0); w32 (0); w32 (0);

    // ---- audio stream ----
    if (hasAudio)
    {
        const juce::uint32 chans      = (juce::uint32) juce::jmax (1, audioChannels);
        const juce::uint32 blockAlign = 2 * chans;   // 16-bit × channels
        wcc ("LIST"); w32 (94); wcc ("strl");
        wcc ("strh"); w32 (56);
        wcc ("auds"); w32 (0);                    // fccHandler
        w32 (0);
        w16 (0); w16 (0);
        w32 (0);
        w32 (1);                                  // dwScale
        w32 ((juce::uint32) audioRate);           // dwRate
        w32 (0);
        audLengthPos = avi->getPosition(); w32 (0); // dwLength (samples)
        w32 (0);
        w32 (0xFFFFFFFFu);
        w32 (blockAlign);                         // dwSampleSize
        w16 (0); w16 (0); w16 (0); w16 (0);

        wcc ("strf"); w32 (18);                   // WAVEFORMATEX
        w16 (1);                                  // PCM
        w16 ((juce::uint16) chans);               // channels
        w32 ((juce::uint32) audioRate);           // samples/sec
        w32 ((juce::uint32) (audioRate * (int) blockAlign)); // avg bytes/sec
        w16 ((juce::uint16) blockAlign);          // block align
        w16 (16);                                 // bits/sample
        w16 (0);                                  // cbSize
    }

    // ---- movi ----
    wcc ("LIST"); moviSizePos = avi->getPosition(); w32 (0);
    moviFourccPos = avi->getPosition(); wcc ("movi");
}

void HudRecorder::writeVideoChunk (const void* jpeg, int len)
{
    const juce::int64 pos = avi->getPosition();
    IdxEntry e; e.id[0]='0'; e.id[1]='0'; e.id[2]='d'; e.id[3]='c';
    e.offset = (juce::uint32) (pos - moviFourccPos); e.size = (juce::uint32) len;
    index.push_back (e);

    avi->write ("00dc", 4);
    avi->writeInt (len);
    avi->write (jpeg, (size_t) len);
    if (len & 1) avi->writeByte (0);
    ++frameCount;
}

void HudRecorder::writeAudioChunk (const juce::int16* pcm, int numSamples)
{
    const int bytes = numSamples * 2;
    const juce::int64 pos = avi->getPosition();
    IdxEntry e; e.id[0]='0'; e.id[1]='1'; e.id[2]='w'; e.id[3]='b';
    e.offset = (juce::uint32) (pos - moviFourccPos); e.size = (juce::uint32) bytes;
    index.push_back (e);

    avi->write ("01wb", 4);
    avi->writeInt (bytes);
    avi->write (pcm, (size_t) bytes);
    if (bytes & 1) avi->writeByte (0);
    audioTotalSamples += numSamples;
}

void HudRecorder::finalizeAvi()
{
    if (avi == nullptr) return;

    if (headerWritten && frameCount > 0)
    {
        auto w32 = [this] (juce::uint32 v) { avi->writeInt ((int) v); };

        const juce::int64 idx1Pos = avi->getPosition();
        avi->write ("idx1", 4);
        avi->writeInt (16 * (int) index.size());
        for (const auto& e : index)
        {
            avi->write (e.id, 4);
            avi->writeInt (0x10);                 // AVIIF_KEYFRAME
            w32 (e.offset);
            w32 (e.size);
        }

        const juce::int64 fileEnd = avi->getPosition();

        avi->setPosition (riffSizePos);    w32 ((juce::uint32) (fileEnd - 8));
        avi->setPosition (totalFramesPos); w32 ((juce::uint32) frameCount);
        avi->setPosition (vidLengthPos);   w32 ((juce::uint32) frameCount);
        // dwLength counts BLOCKS (frames), and a block is one interleaved frame.
        if (hasAudio) { avi->setPosition (audLengthPos);
                        w32 ((juce::uint32) (audioTotalSamples / juce::jmax (1, audioChannels))); }
        avi->setPosition (moviSizePos);    w32 ((juce::uint32) (idx1Pos - moviFourccPos));
        avi->setPosition (fileEnd);
        avi->flush();
        avi.reset();
    }
    else
    {
        avi.reset();
        outFile.deleteFile();
    }
}

// Message-thread capture path. Windows no longer uses it at all — see GrabThread,
// which does the same job on its own thread with cached GDI resources.
HudRecorder::RawFrame HudRecorder::capture (juce::Component* c)
{
#if JUCE_WINDOWS
    juce::ignoreUnused (c);
    return {};
#else
    auto* top = c->getTopLevelComponent();
    if (top == nullptr) return {};

    if (captureSource == CaptureSource::WholeScreen)
        return captureScreen();

    const int lw = top->getWidth()  & ~1;
    const int lh = top->getHeight() & ~1;

    // While the HUD is being resized the size changes every frame; capturing then
    // (screen grab + GL relayout) can freeze the UI. Skip until the size is stable.
    if (lw != lastLogicalW || lh != lastLogicalH)
    {
        lastLogicalW = lw;
        lastLogicalH = lh;
        return {};
    }
    if (lw < 2 || lh < 2) return {};

    RawFrame frame;

 #if JUCE_MAC
    if (auto* peer = top->getPeer())
    {
        juce::Image m;
        if (alterCaptureMacWindow (peer->getNativeHandle(), m) && m.isValid())
            frame.image = m;
    }
 #endif

    if (! frame.valid())
        frame.image = top->createComponentSnapshot (top->getLocalBounds());

    return frame;
#endif
}

// Whole-display grab: the HUD, the DAW, everything. No window bookkeeping needed
// (the screen does not change size mid-recording the way a window does).
HudRecorder::RawFrame HudRecorder::captureScreen()
{
#if JUCE_WINDOWS
    return {};                       // handled by GrabThread::sourceRect()
#else
    RawFrame frame;

 #if JUCE_MAC
    juce::Image m;
    if (alterCaptureMacScreen (m) && m.isValid())
        frame.image = m;
 #endif

    return frame;
#endif
}
