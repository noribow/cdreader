#include "cdreader/scsi.h"

#include <cstdio>

namespace cdr {

SenseInfo parseSense(const uint8_t* sense, size_t length) {
    SenseInfo info;
    if (sense == nullptr || length < 1) return info;
    const uint8_t code = sense[0] & 0x7F;
    if ((code == 0x70 || code == 0x71) && length >= 14) {  // fixed format
        info.key = sense[2] & 0x0F;
        info.asc = sense[12];
        info.ascq = sense[13];
    } else if ((code == 0x72 || code == 0x73) && length >= 4) {  // descriptor format
        info.key = sense[1] & 0x0F;
        info.asc = sense[2];
        info.ascq = sense[3];
    }
    return info;
}

static const char* senseKeyName(uint8_t key) {
    switch (key) {
        case 0x0: return "NO SENSE";
        case 0x1: return "RECOVERED ERROR";
        case 0x2: return "NOT READY";
        case 0x3: return "MEDIUM ERROR";
        case 0x4: return "HARDWARE ERROR";
        case 0x5: return "ILLEGAL REQUEST";
        case 0x6: return "UNIT ATTENTION";
        case 0x7: return "DATA PROTECT";
        case 0xB: return "ABORTED COMMAND";
        default: return "OTHER";
    }
}

std::string ScsiResult::describe() const {
    if (!transportOk) return error.empty() ? std::string("transport error") : error;
    if (status == 0) return "OK";
    char buf[128];
    std::snprintf(buf, sizeof buf, "SCSI status 0x%02X, sense %s (key %X, ASC %02X, ASCQ %02X)",
                  status, senseKeyName(sense.key), sense.key, sense.asc, sense.ascq);
    return buf;
}

}  // namespace cdr
