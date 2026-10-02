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
