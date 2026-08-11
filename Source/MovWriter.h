/*
  ==============================================================================

    MovWriter.h
    A minimal QuickTime (.mov) writer for LOSSLESS VIDEO WITH AN ALPHA CHANNEL.

    WHY THIS FILE EXISTS
    --------------------
    Neither of the platform encoders the HUD recorder normally uses can carry
    alpha. Media Foundation has no alpha-capable video encoder at all, and the
    H.264 it does produce is 4:2:0 and opaque by definition. So a transparent
    export needs its own container, written here.

    WHY 'png ' AND NOT ANIMATION/RLE
    --------------------------------
    QuickTime Animation (RLE) is the traditional answer, but implementing it means
    hand-writing a codec: skip codes, signed run codes, per-line state, all in
    big-endian bytes that no compiler will check. A silent mistake there produces a
    file that looks fine in one player and tears in another.

    QuickTime also defines a 'png ' video codec, where every sample is simply a
    complete PNG file. That makes the compression JUCE's problem instead of ours —
    PNGImageFormat is already in the build — and PNG is lossless, carries a real
    alpha channel, and is decoded by QuickTime, Premiere, After Effects, Resolve
    and FFmpeg alike. What is left to write here is only the container.

    AUDIO
    -----
    A second track carrying UNCOMPRESSED 16-bit PCM ('sowt'). No encoder needed and
    nothing to go wrong in it: QuickTime has carried raw PCM since the beginning and
    every editor reads it. It costs about 5.6 MB a minute at 48 kHz stereo, which
    next to lossless PNG video is nothing.

    WHAT IT DOES NOT DO
    -------------------
    32-bit files only (a take is capped at 4 GB; close() reports failure rather than
    writing a corrupt index).

  ==============================================================================
*/

#pragma once
#include <JuceHeader.h>
#include <vector>
#include <algorithm>
#include <utility>

class AlterMovWriter
{
public:
    AlterMovWriter() = default;
    ~AlterMovWriter() { close(); }

    /** Creates the file and reserves the media-data header. */
    bool open (const juce::File& file, int frameW, int frameH, int framesPerSecond)
    {
        close();

        width  = frameW & ~1;
        height = frameH & ~1;
        fps    = juce::jlimit (1, 240, framesPerSecond);
        if (width < 2 || height < 2) return false;

        outFile = file;
        outFile.deleteFile();
        outFile.getParentDirectory().createDirectory();

        out = std::make_unique<juce::FileOutputStream> (outFile);
        if (! out->openedOk()) { out.reset(); return false; }

        // TRUNCATE. juce::FileOutputStream opens an existing file positioned at its
        // END — it appends, it does not replace. Normally deleteFile() above has
        // already dealt with that, but a file still open in an editor or a player
        // cannot be deleted on Windows, and then the new recording was written
        // AFTER the old movie: the file grew, the old index was still the first one
        // a player found, and re-recording over a take looked like it had silently
        // done nothing.
        out->setPosition (0);
        out->truncate();

        // ── ftyp ─────────────────────────────────────────────────────────────
        writeU32 (20);           writeTag ("ftyp");
        writeTag ("qt  ");       writeU32 (0x20050300);
        writeTag ("qt  ");

        // ── mdat ─────────────────────────────────────────────────────────────
        // The size is not known until every frame is in, so a placeholder goes
        // down now and close() comes back to patch it.
        mdatSizePos = out->getPosition();
        writeU32 (0);            writeTag ("mdat");
        mdatStart = out->getPosition();

        samples.clear();
        audioChunks.clear();
        audioFrames = 0;
        ok = true;
        return true;
    }

    bool isOpen() const noexcept { return ok; }

    /** Appends one frame. `timeMs` is the frame's presentation time measured from
        the start of the recording; it is what the sample durations are derived
        from, so a dropped frame lengthens its predecessor instead of silently
        speeding the video up. The image must be ARGB. */
    bool addFrame (const juce::Image& argb, juce::int64 timeMs)
    {
        if (! ok || out == nullptr || ! argb.isValid()) return false;

        juce::MemoryOutputStream png;
        {
            juce::PNGImageFormat fmt;
            if (! fmt.writeImageToStream (argb, png))
                return false;
        }

        Sample s;
        s.offset = out->getPosition();
        s.size   = (juce::uint32) png.getDataSize();
        s.timeMs = timeMs;
        samples.push_back (s);

        return out->write (png.getData(), png.getDataSize());
    }

    /** Appends a frame that has ALREADY been PNG-encoded elsewhere — which is how
        the recorder encodes several frames at once. Compression is the whole cost
        of this format, so it must not be tied to the one thread that writes. */
    bool addEncodedFrame (const void* pngData, size_t bytes, juce::int64 timeMs)
    {
        if (! ok || out == nullptr || pngData == nullptr || bytes == 0) return false;

        Sample s;
        s.offset = out->getPosition();
        s.size   = (juce::uint32) bytes;
        s.timeMs = timeMs;
        samples.push_back (s);

        return out->write (pngData, bytes);
    }

    /** Appends interleaved 16-bit PCM. Call with whatever arrives, whenever it
        arrives — each call becomes one chunk and the index sorts it out. */
    bool addAudio (const juce::int16* interleaved, int numSamples, int sampleRateHz, int channels)
    {
        if (! ok || out == nullptr || interleaved == nullptr || numSamples <= 0) return false;

        audioRate     = juce::jmax (8000, sampleRateHz);
        audioChannels = juce::jlimit (1, 2, channels);

        const int frames = numSamples / audioChannels;
        if (frames <= 0) return false;

        AudioChunk c;
        c.offset = out->getPosition();
        c.frames = frames;
        audioChunks.push_back (c);
        audioFrames += (juce::int64) frames;

        return out->write (interleaved, (size_t) frames * (size_t) audioChannels * 2);
    }

    int getNumFrames() const noexcept { return (int) samples.size(); }
    int getWidth()     const noexcept { return width;  }
    int getHeight()    const noexcept { return height; }

    /** Frames in the FINISHED movie. More than getNumFrames() means slots had to
        repeat a frame — i.e. capture fell behind the target rate. */
    int getNumSlots() const noexcept { return (int) timeline.size(); }

    /** Writes the movie index and finalises the file. Returns false (and deletes
        the file) if there is nothing to write or the take overflowed 32 bits. */
    bool close()
    {
        if (! ok || out == nullptr) { out.reset(); ok = false; return false; }

        const juce::int64 mdatEnd = out->getPosition();
        const juce::int64 mdatLen = mdatEnd - mdatSizePos;

        bool good = ! samples.empty() && mdatEnd < 0xFFFFFFFFLL;

        if (good)
        {
            // CONSTANT FRAME RATE, resampled onto a fixed grid.
            //
            // The honest thing looks like the right thing here and is not: giving
            // every frame the duration it actually took produces a variable-rate
            // movie. Players cope; editors do not. After Effects and Premiere
            // conform a VFR clip to a fixed timeline rate, and their idea of how
            // to do that — duplicate here, drop there — is exactly what reads as
            // stutter, even though the file describes the motion correctly.
            //
            // So the timestamps are used to decide WHICH captured frame belongs in
            // each 1/fps slot, and the file then declares one constant duration. A
            // slot with no new frame repeats the previous one, which costs an index
            // entry and no pixels, and is what every screen recorder does.
            // The grid starts at TIME ZERO of the recording, not at the first frame
            // that happened to arrive. The audio track has no timestamps — sample N
            // simply plays at N/rate — so its zero is the moment recording began,
            // and giving the video a different zero would offset the whole take by
            // however long the first grab took. A first slot with nothing to show
            // repeats the first real frame, which costs an index entry.
            const juce::int64 span = samples.back().timeMs;
            const int slots = juce::jmax (1, (int) ((span * fps + 500) / 1000) + 1);

            timeline.clear();
            timeline.reserve ((size_t) slots);

            size_t cursor = 0;
            for (int i = 0; i < slots; ++i)
            {
                const juce::int64 slotMs = (juce::int64) i * 1000 / fps;

                while (cursor + 1 < samples.size() && samples[cursor + 1].timeMs <= slotMs)
                    ++cursor;

                timeline.push_back (cursor);
            }

            totalDuration = (juce::uint32) ((juce::int64) slots * kFrameTicks);
            writeMoov();

            // Patch the mdat length now that everything after it is written.
            out->setPosition (mdatSizePos);
            writeU32 ((juce::uint32) mdatLen);
        }

        out->flush();
        out.reset();
        ok = false;

        if (! good) outFile.deleteFile();
        return good;
    }

    juce::File getFile() const { return outFile; }

private:
    struct Sample     { juce::int64 offset = 0; juce::uint32 size = 0; juce::int64 timeMs = 0; };
    struct AudioChunk { juce::int64 offset = 0; int frames = 0; };

    // 1000 ticks per FRAME, so the timescale is fps * 1000 and one frame lasts
    // exactly kFrameTicks — no rounding anywhere, at any frame rate.
    static constexpr juce::uint32 kFrameTicks = 1000;
    juce::uint32 timescale() const noexcept { return (juce::uint32) fps * kFrameTicks; }

    // ── primitive writers (QuickTime is big-endian throughout) ───────────────
    void writeU8  (juce::uint8 v)  { out->writeByte ((char) v); }
    void writeU16 (juce::uint16 v) { out->writeShortBigEndian ((short) v); }
    void writeU32 (juce::uint32 v) { out->writeIntBigEndian ((int) v); }
    void writeTag (const char* t)  { out->write (t, 4); }
    void writeZeros (int n)        { for (int i = 0; i < n; ++i) writeU8 (0); }

    /** Opens an atom and returns where its size field lives, for patchAtom(). */
    juce::int64 beginAtom (const char* type)
    {
        const auto pos = out->getPosition();
        writeU32 (0);
        writeTag (type);
        return pos;
    }

    void patchAtom (juce::int64 sizePos)
    {
        const auto end = out->getPosition();
        out->setPosition (sizePos);
        writeU32 ((juce::uint32) (end - sizePos));
        out->setPosition (end);
    }

    void writeMoov()
    {
        const auto moov = beginAtom ("moov");

        // ── mvhd ─────────────────────────────────────────────────────────────
        {
            const auto a = beginAtom ("mvhd");
            writeU32 (0);                       // version + flags
            writeU32 (0); writeU32 (0);         // creation / modification time
            writeU32 (timescale());
            writeU32 (totalDuration);
            writeU32 (0x00010000);              // preferred rate 1.0
            writeU16 (0x0100);                  // preferred volume 1.0
            writeZeros (10);                    // reserved
            writeMatrix();
            writeZeros (24);                    // pre-defined
            writeU32 (audioFrames > 0 ? 3 : 2);  // next track id
            patchAtom (a);
        }

        // ── trak ─────────────────────────────────────────────────────────────
        {
            const auto trak = beginAtom ("trak");

            {
                const auto a = beginAtom ("tkhd");
                writeU32 (0x0000000F);          // version 0, enabled | in movie | in preview
                writeU32 (0); writeU32 (0);
                writeU32 (1);                   // track id
                writeU32 (0);                   // reserved
                writeU32 (totalDuration);
                writeZeros (8);                 // reserved
                writeU16 (0);                   // layer
                writeU16 (0);                   // alternate group
                writeU16 (0);                   // volume (video: 0)
                writeU16 (0);                   // reserved
                writeMatrix();
                writeU32 ((juce::uint32) width  << 16);   // 16.16 fixed
                writeU32 ((juce::uint32) height << 16);
                patchAtom (a);
            }

            {
                const auto mdia = beginAtom ("mdia");

                {
                    const auto a = beginAtom ("mdhd");
                    writeU32 (0);
                    writeU32 (0); writeU32 (0);
                    writeU32 (timescale());
                    writeU32 (totalDuration);
                    writeU16 (0x55C4);          // language: undetermined
                    writeU16 (0);               // quality
                    patchAtom (a);
                }

                {
                    const auto a = beginAtom ("hdlr");
                    writeU32 (0);
                    writeTag ("mhlr");
                    writeTag ("vide");
                    writeU32 (0); writeU32 (0); writeU32 (0);
                    writeU8 (0);                // empty component name
                    patchAtom (a);
                }

                {
                    const auto minf = beginAtom ("minf");

                    {
                        const auto a = beginAtom ("vmhd");
                        writeU32 (0x00000001);  // flags: no lean ahead
                        writeU16 (0);           // graphics mode: copy
                        writeU16 (0); writeU16 (0); writeU16 (0);   // opcolor
                        patchAtom (a);
                    }

                    {
                        const auto a = beginAtom ("hdlr");
                        writeU32 (0);
                        writeTag ("dhlr");
                        writeTag ("alis");
                        writeU32 (0); writeU32 (0); writeU32 (0);
                        writeU8 (0);
                        patchAtom (a);
                    }

                    {
                        const auto dinf = beginAtom ("dinf");
                        const auto dref = beginAtom ("dref");
                        writeU32 (0);           // version + flags
                        writeU32 (1);           // one entry
                        writeU32 (12);          // entry size
                        writeTag ("alis");
                        writeU32 (0x00000001);  // self-reference: media is in this file
                        patchAtom (dref);
                        patchAtom (dinf);
                    }

                    {
                        const auto stbl = beginAtom ("stbl");
                        writeStsd();
                        writeStts();

                        {   // stsc: one sample per chunk, so one entry covers all
                            const auto a = beginAtom ("stsc");
                            writeU32 (0);
                            writeU32 (1);
                            writeU32 (1);       // first chunk
                            writeU32 (1);       // samples per chunk
                            writeU32 (1);       // sample description id
                            patchAtom (a);
                        }

                        // A repeated slot points at the SAME bytes again: the
                        // index carries the duplicate, the mdat does not.
                        {   // stsz: PNG samples all differ, so a full table
                            const auto a = beginAtom ("stsz");
                            writeU32 (0);
                            writeU32 (0);       // 0 = sizes follow individually
                            writeU32 ((juce::uint32) timeline.size());
                            for (auto i : timeline) writeU32 (samples[i].size);
                            patchAtom (a);
                        }

                        {   // stco: chunk offsets == sample offsets
                            const auto a = beginAtom ("stco");
                            writeU32 (0);
                            writeU32 ((juce::uint32) timeline.size());
                            for (auto i : timeline) writeU32 ((juce::uint32) samples[i].offset);
                            patchAtom (a);
                        }

                        patchAtom (stbl);
                    }

                    patchAtom (minf);
                }

                patchAtom (mdia);
            }

            patchAtom (trak);
        }

        if (audioFrames > 0)
            writeAudioTrak();

        patchAtom (moov);
    }


    /** Second track: uncompressed 16-bit PCM. Every sample is the same size, so
        the index is unusually small — one stts entry, one stsz size, and a chunk
        list. */
    void writeAudioTrak()
    {
        const juce::uint32 bytesPerFrame = (juce::uint32) audioChannels * 2;

        const auto trak = beginAtom ("trak");

        {
            const auto a = beginAtom ("tkhd");
            writeU32 (0x0000000F);              // enabled | in movie | in preview
            writeU32 (0); writeU32 (0);
            writeU32 (2);                       // track id
            writeU32 (0);
            // Duration is in the MOVIE timescale, which is the video's — the audio
            // clock is its own and lives in mdhd below.
            writeU32 ((juce::uint32) (audioFrames * timescale() / juce::jmax (1, audioRate)));
            writeZeros (8);
            writeU16 (0);                       // layer
            writeU16 (0);                       // alternate group
            writeU16 (0x0100);                  // volume 1.0 — this one IS audio
            writeU16 (0);
            writeMatrix();
            writeU32 (0); writeU32 (0);         // no width/height for sound
            patchAtom (a);
        }

        {
            const auto mdia = beginAtom ("mdia");

            {
                const auto a = beginAtom ("mdhd");
                writeU32 (0);
                writeU32 (0); writeU32 (0);
                writeU32 ((juce::uint32) audioRate);        // the audio's own clock
                writeU32 ((juce::uint32) audioFrames);
                writeU16 (0x55C4);
                writeU16 (0);
                patchAtom (a);
            }

            {
                const auto a = beginAtom ("hdlr");
                writeU32 (0);
                writeTag ("mhlr");
                writeTag ("soun");
                writeU32 (0); writeU32 (0); writeU32 (0);
                writeU8 (0);
                patchAtom (a);
            }

            {
                const auto minf = beginAtom ("minf");

                {
                    const auto a = beginAtom ("smhd");
                    writeU32 (0);
                    writeU16 (0);               // balance
                    writeU16 (0);               // reserved
                    patchAtom (a);
                }

                {
                    const auto a = beginAtom ("hdlr");
                    writeU32 (0);
                    writeTag ("dhlr");
                    writeTag ("alis");
                    writeU32 (0); writeU32 (0); writeU32 (0);
                    writeU8 (0);
                    patchAtom (a);
                }

                {
                    const auto dinf = beginAtom ("dinf");
                    const auto dref = beginAtom ("dref");
                    writeU32 (0);
                    writeU32 (1);
                    writeU32 (12);
                    writeTag ("alis");
                    writeU32 (0x00000001);
                    patchAtom (dref);
                    patchAtom (dinf);
                }

                {
                    const auto stbl = beginAtom ("stbl");

                    {   // stsd: 'sowt' = signed 16-bit LITTLE-endian, which is what
                        // the rest of this app already has in memory. 'twos' would
                        // mean byte-swapping every sample for no gain.
                        const auto a = beginAtom ("stsd");
                        writeU32 (0);
                        writeU32 (1);

                        const auto e = beginAtom ("sowt");
                        writeZeros (6);
                        writeU16 (1);                       // data reference index
                        writeU16 (0);                       // version 0
                        writeU16 (0);                       // revision
                        writeU32 (0);                       // vendor
                        writeU16 ((juce::uint16) audioChannels);
                        writeU16 (16);                      // bits per sample
                        writeU16 (0);                       // compression id
                        writeU16 (0);                       // packet size
                        writeU32 ((juce::uint32) audioRate << 16);   // 16.16 fixed
                        patchAtom (e);

                        patchAtom (a);
                    }

                    {   // stts: every sample lasts exactly one tick of the audio clock
                        const auto a = beginAtom ("stts");
                        writeU32 (0);
                        writeU32 (1);
                        writeU32 ((juce::uint32) audioFrames);
                        writeU32 (1);
                        patchAtom (a);
                    }

                    {   // stsc: run-length over chunks whose sample count matches
                        std::vector<std::pair<juce::uint32, juce::uint32>> runs;  // firstChunk, perChunk
                        for (size_t i = 0; i < audioChunks.size(); ++i)
                        {
                            const auto n = (juce::uint32) audioChunks[i].frames;
                            if (runs.empty() || runs.back().second != n)
                                runs.push_back ({ (juce::uint32) i + 1, n });
                        }

                        const auto a = beginAtom ("stsc");
                        writeU32 (0);
                        writeU32 ((juce::uint32) runs.size());
                        for (const auto& r : runs)
                        {
                            writeU32 (r.first);
                            writeU32 (r.second);
                            writeU32 (1);       // sample description id
                        }
                        patchAtom (a);
                    }

                    {   // stsz: one constant size, so no table at all
                        const auto a = beginAtom ("stsz");
                        writeU32 (0);
                        writeU32 (bytesPerFrame);
                        writeU32 ((juce::uint32) audioFrames);
                        patchAtom (a);
                    }

                    {
                        const auto a = beginAtom ("stco");
                        writeU32 (0);
                        writeU32 ((juce::uint32) audioChunks.size());
                        for (const auto& c : audioChunks) writeU32 ((juce::uint32) c.offset);
                        patchAtom (a);
                    }

                    patchAtom (stbl);
                }

                patchAtom (minf);
            }

            patchAtom (mdia);
        }

        patchAtom (trak);
    }

    void writeStsd()
    {
        const auto a = beginAtom ("stsd");
        writeU32 (0);                       // version + flags
        writeU32 (1);                       // one description

        const auto e = beginAtom ("png ");  // the sample entry IS an atom
        writeZeros (6);                     // reserved
        writeU16 (1);                       // data reference index
        writeU16 (0);                       // version
        writeU16 (0);                       // revision
        writeTag ("appl");                  // vendor
        writeU32 (0);                       // temporal quality
        writeU32 (512);                     // spatial quality (lossless)
        writeU16 ((juce::uint16) width);
        writeU16 ((juce::uint16) height);
        writeU32 (0x00480000);              // 72 dpi horizontal
        writeU32 (0x00480000);              // 72 dpi vertical
        writeU32 (0);                       // data size
        writeU16 (1);                       // frames per sample

        // 32-byte Pascal string
        const char* name = "PNG";
        writeU8 (3);
        out->write (name, 3);
        writeZeros (28);

        // THE POINT OF THE WHOLE FILE: depth 32 is what declares that these
        // samples carry an alpha channel. At 24 every decoder throws it away.
        writeU16 (32);
        writeU16 (0xFFFF);                  // colour table id: none
        patchAtom (e);

        patchAtom (a);
    }

    void writeStts()
    {
        // Constant rate, so the entire table is one entry: N samples, each of
        // exactly kFrameTicks. This is the atom an editor reads to decide the
        // clip's frame rate, and a single clean entry is what stops it from
        // conforming the timing itself.
        const auto a = beginAtom ("stts");
        writeU32 (0);
        writeU32 (1);
        writeU32 ((juce::uint32) timeline.size());
        writeU32 (kFrameTicks);
        patchAtom (a);
    }

    void writeMatrix()
    {
        // Identity, in 16.16 / 2.30 fixed point.
        writeU32 (0x00010000); writeU32 (0); writeU32 (0);
        writeU32 (0); writeU32 (0x00010000); writeU32 (0);
        writeU32 (0); writeU32 (0); writeU32 (0x40000000);
    }

    std::unique_ptr<juce::FileOutputStream> out;
    juce::File   outFile;
    bool         ok = false;
    int          width = 0, height = 0;
    juce::int64  mdatSizePos = 0, mdatStart = 0;
    juce::uint32 totalDuration = 0;
    int          fps = 30;
    std::vector<Sample>  samples;    // what was actually captured
    std::vector<size_t>  timeline;   // one entry per CFR slot -> index into samples

    std::vector<AudioChunk> audioChunks;
    juce::int64             audioFrames = 0;
    int                     audioRate = 48000, audioChannels = 2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AlterMovWriter)
};
