#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cdreader/metadata.h"
#include "cdreader/scsi.h"
#include "cdreader/toc.h"

// Q sub-channel data read with READ SUB-CHANNEL (MMC, opcode 42h): the media
// catalog number (MCN, mode 2 Q frames) and the per-track ISRC (mode 3).
// Parsing is kept separate from CdDrive so that it is unit tested on raw
// responses; CdDrive::readSubChannel() is the generic transport-level call
// (also for the current-position format 01h used for pregap detection, #25).
//
// Per-sector Q frames (READ CD with sub-channel data, #25) are parsed here as
// well: they carry the track / index number and the absolute position of
// every sector, which gap detection (gaps.h) uses to find INDEX 00 / 02+.
namespace cdr {

class CdDrive;

// Sub-channel data format codes of READ SUB-CHANNEL (CDB byte 3).
enum class SubChannelFormat : uint8_t {
    CurrentPosition = 0x01,
    MediaCatalogNumber = 0x02,
    Isrc = 0x03,
};

// Bytes of a format 01h / 02h / 03h response: 4-byte header + 20 bytes.
constexpr size_t kSubChannelResponseBytes = 24;

// MCN: 13 decimal digits (UPC / EAN-13). All zeros (what some drives report
// for "no MCN" with MCVal set) is not a catalog number.
bool isValidMcn(const std::string& mcn);
// ISRC: CC-XXX-YY-NNNNN without dashes: 2 letters (country), 3 letters or
// digits (registrant), 2 digits (year), 5 digits (designation). Upper case.
bool isValidIsrc(const std::string& isrc);

// Result of reading the MCN or one track's ISRC. Reading never aborts a rip:
// everything except Found only means "not known".
struct SubChannelCode {
    enum class Status {
        Found,        // `value` holds a validated code
        NotPresent,   // the drive answered: no code on this disc / track (MCVal / TCVal = 0)
        Invalid,      // the drive answered with a malformed response or code (`detail`)
        Unsupported,  // the drive rejected the command (ILLEGAL REQUEST)
        Failed,       // other SCSI / transport error (`detail`)
        Skipped,      // not read (disabled, or the drive already rejected the command)
    };
    Status status = Status::Skipped;
    std::string value;
    std::string detail;

    bool found() const { return status == Status::Found; }
    // The code, or why there is none ("not present", "unsupported by the drive", ...).
    std::string describe() const;
};

// Parse a READ SUB-CHANNEL response (`length` = bytes transferred).
// parseIsrcResponse() also checks that the response is for `track`.
SubChannelCode parseMcnResponse(const uint8_t* data, size_t length);
SubChannelCode parseIsrcResponse(const uint8_t* data, size_t length, int track);
// Classifies a failed READ SUB-CHANNEL command (Unsupported or Failed).
SubChannelCode subChannelError(const ScsiResult& result);

// --- Per-sector Q frames (#25) -------------------------------------------------

// Sub-channel selection of READ CD (BEh, CDB byte 10 bits 2..0).
enum class SubChannelSelection : uint8_t {
    None = 0,
    RawPW = 1,       // 96 bytes: one bit of each channel P..W per byte (Q = bit 6)
    FormattedQ = 2,  // 16 bytes: the Q frame, de-interleaved by the drive
};
// Bytes of sub-channel data per sector for a selection (0, 96, 16).
size_t subChannelBytesPerSector(SubChannelSelection selection);

// CRC-16 of a Q frame (CRC-CCITT, x^16 + x^12 + x^5 + 1, initial value 0,
// MSB first; check value of "123456789" = 31C3h). On the disc the 16 parity
// bits are stored inverted after the 80 data bits (Red Book / IEC 60908).
uint16_t subQCrc16(const uint8_t* data, size_t length);

// One decoded Q frame. Mode 1 (ADR 1) frames carry the position; mode 2
// (MCN) / mode 3 (ISRC) frames, about 1 in 100, do not.
struct QFrame {
    enum class Crc {
        Absent,   // the drive returned no CRC (formatted Q with CRC bytes 0, READ SUB-CHANNEL)
        Valid,
        Invalid,  // the frame is corrupt: never use it
    };
    bool parsed = false;     // BCD fields and numbers are well formed (mode 1), or another mode
    uint8_t control = 0;     // 4 bits: 0 = 2-channel audio, bit 2 = data, bit 0 = pre-emphasis
    uint8_t adr = 0;         // mode: 1 = position, 2 = MCN, 3 = ISRC
    int track = 0;           // 1..99, 170 = lead-out (AAh)
    int index = 0;           // 0 = pregap, 1.. = index points
    int32_t relative = 0;    // sectors from INDEX 01 of the track (negative in the pregap)
    int32_t absoluteLba = 0; // absolute position as an LBA (MSF - 150; negative before LBA 0)
    Crc crc = Crc::Absent;

    // A mode 1 frame that can be trusted as far as it can be checked.
    bool usable() const { return parsed && adr == 1 && crc != Crc::Invalid; }
    // Monotone along the disc: track * 100 + index (used by gap detection).
    int key() const { return track * 100 + index; }
};

// The 12 Q bytes (10 data + 2 CRC) of a raw P-W block (96 bytes, Q = bit 6).
void deinterleaveQ(const uint8_t* rawPw, uint8_t q[12]);
// The inverse (tests / the fake drive): builds 96 bytes with the Q bits in bit 6
// and the P bit (bit 7) set when `pause`; the R-W bits are left as found in `rawPw`.
void interleaveQ(const uint8_t q[12], bool pause, uint8_t* rawPw);

// Decodes 10 Q data bytes (+ 2 CRC bytes when `withCrc`, checked).
QFrame parseQ(const uint8_t* q, bool withCrc);
// Formatted Q (16 bytes): bytes 0..9 as on the disc; bytes 10..11 hold the
// CRC on drives that return it (both 0: no CRC, not checked).
QFrame parseFormattedQ(const uint8_t* data);
// Raw P-W (96 bytes): de-interleaves Q and checks its CRC.
QFrame parseRawPwQ(const uint8_t* rawPw);
// READ SUB-CHANNEL format 01h (current position) response with LBA
// addressing (`length` = bytes transferred, at least 16).
QFrame parseCurrentPosition(const uint8_t* data, size_t length);

// The disc's MCN and the ISRCs of its audio tracks.
struct DiscCodes {
    bool read = false;            // readDiscCodes() was called (also when everything failed)
    SubChannelCode mcn;
    std::map<int, SubChannelCode> isrcs;  // by track number

    std::string mcnValue() const { return mcn.found() ? mcn.value : std::string(); }
    // The validated ISRC of `track`, empty if unknown.
    std::string isrc(int track) const;
    // rip.log lines: "MCN: ..." and "Track NN ISRC: ..." (the CLI and Android share them).
    std::vector<std::string> logLines() const;
    // Copies the found codes into album.mcn / album.trackIsrcs (replacing them).
    void applyTo(AlbumMetadata& album) const;
};

// Reads the MCN and the ISRC of each of `tracks` (data tracks are skipped).
// Never throws: errors end up in the statuses. When the drive rejects READ
// SUB-CHANNEL (ILLEGAL REQUEST) once, the remaining reads are skipped, since
// some drives are slow to fail every command.
DiscCodes readDiscCodes(CdDrive& drive, const std::vector<Track>& tracks);

}  // namespace cdr
