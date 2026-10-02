#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cdreader/metadata.h"

// Building blocks of an MP4 / M4A audio file (ISO/IEC 14496-12 boxes and
// the iTunes metadata convention), codec independent. AlacWriter
// (alac_writer.h) writes
//
//   ftyp  "M4A " (compatible: M4A, mp42, isom)
//   mdat  the encoded frames, streamed (its size is patched at the end)
//   moov  mvhd, trak (tkhd, mdia (mdhd, hdlr "soun", minf (smhd, dinf/dref,
//         stbl (stsd, stts, stsc, stsz, stco / co64)))), udta/meta/ilst
//
// with the movie box last, because the frame sizes are only known then.
namespace cdr::mp4 {

// Appends `v` as a `bytes`-byte big-endian number.
void putBe(std::vector<uint8_t>& out, uint64_t v, int bytes);

// A box: 32-bit size, 4-character type, payload. A full box adds a version
// byte and 24 bits of flags before the payload.
std::vector<uint8_t> box(const std::string& type, const std::vector<uint8_t>& payload);
std::vector<uint8_t> fullBox(const std::string& type, uint8_t version, uint32_t flags,
                             const std::vector<uint8_t>& payload);

// The "ftyp" box of an M4A audio file.
std::vector<uint8_t> fileType();

// AudioSampleEntry of the sample description (stsd): `format` (e.g. "alac")
// with the codec's configuration box(es) appended as `children`.
std::vector<uint8_t> audioSampleEntry(const std::string& format, unsigned channels, unsigned sampleSize,
                                      uint32_t sampleRate, const std::vector<uint8_t>& children);

// iTunes-style tags as a "udta" box (udta/meta/hdlr "mdir" + ilst): ©nam
// title, ©ART artist, ©alb album, aART album artist, trkn track number /
// total, ©day year, ©gen genre, ©too encoder, and freeform
// "----:com.apple.iTunes:CDDB" with the CDDB disc id. Text is UTF-8; empty
// fields are omitted.
std::vector<uint8_t> itunesMetadata(const TrackMetadata& metadata, const std::string& encoder);

// One audio track of fixed-duration frames ("samples" in MP4 terms) stored
// back to back in the file.
struct AudioTrack {
    uint32_t sampleRate = 44100;        // also the time scale of the movie and the track
    uint64_t duration = 0;              // in samples (per channel)
    uint32_t frameDuration = 4096;      // samples per frame, every frame but the last
    std::vector<uint8_t> sampleEntry;   // see audioSampleEntry()
    std::vector<uint32_t> frameSizes;   // bytes of each frame
    std::vector<uint64_t> frameOffsets; // file offset of each frame (ascending)
    unsigned framesPerChunk = 10;       // frames referenced by one stco / co64 entry
};

// The "moov" box for `track` and the `udta` box (may be empty).
std::vector<uint8_t> movie(const AudioTrack& track, const std::vector<uint8_t>& udta);

}  // namespace cdr::mp4
