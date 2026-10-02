// Tests for the USB mass storage Bulk-Only Transport (platform/android)
// against an in-memory USB device. Same minimal runner as test_main.cpp.

#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <vector>

#include "bot_transport.h"
#include "cdreader/cd_drive.h"
#include "cdreader/ripper.h"
#include "cdreader/scsi.h"
#include "cdreader/toc.h"
#include "fake_drive.h"
#include "fake_usb_device.h"

namespace {

int failures = 0;
std::vector<std::pair<const char*, std::function<void()>>>& registry() {
    static std::vector<std::pair<const char*, std::function<void()>>> r;
    return r;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().emplace_back(name, std::move(fn)); }
};

#define TEST(name)                                  \
    static void name();                             \
    static Register reg_##name(#name, name);        \
    static void name()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                          \
    do {                                                                                        \
        auto va = (a);                                                                          \
        auto vb = (b);                                                                          \
        if (!(va == vb)) {                                                                      \
            std::fprintf(stderr, "  %s:%d: CHECK_EQ(%s, %s) failed: %lld != %lld\n", __FILE__, \
                         __LINE__, #a, #b, (long long)va, (long long)vb);                       \
            ++failures;                                                                         \
        }                                                                                       \
    } while (0)

using cdr::DataDirection;
using cdr::usb::BulkOnlyTransport;

// Three audio tracks; lead-out at 10 seconds (as in test_main.cpp).
FakeDrive makeAudioDisc() { return FakeDrive({{0, false}, {300, false}, {450, false}}, 750); }

// A fake drive behind a fake USB bridge behind the transport under test.
struct Rig {
    FakeDrive drive = makeAudioDisc();
    FakeUsbDevice device{drive, 3};
    BulkOnlyTransport bot{device, 3};

    cdr::ScsiResult inquiry(uint8_t* data, size_t length = 36) {
        const uint8_t cdb[6] = {0x12, 0, 0, 0, uint8_t(length), 0};
        return bot.execute(cdb, sizeof cdb, data, length, DataDirection::In, 10);
    }
    cdr::ScsiResult testUnitReady() {
        const uint8_t cdb[6] = {};
        return bot.execute(cdb, sizeof cdb, nullptr, 0, DataDirection::None, 10);
    }
    cdr::ScsiResult readToc(std::vector<uint8_t>& data) {
        data.assign(804, 0xEE);
        const uint8_t cdb[10] = {0x43, 0, 0, 0, 0, 0, 1, uint8_t(data.size() >> 8), uint8_t(data.size()), 0};
        return bot.execute(cdb, sizeof cdb, data.data(), data.size(), DataDirection::In, 10);
    }
    // The device is in a clean state: a normal command goes through.
    bool recovered() {
        uint8_t data[36];
        const cdr::ScsiResult r = inquiry(data);
        return r.ok() && r.transferred == 36 && std::memcmp(data + 8, "FAKE", 4) == 0 && !device.haltedIn() &&
               !device.haltedOut();
    }
};

}  // namespace

TEST(data_in_command_round_trip) {
    Rig rig;
    uint8_t data[36] = {};
    const cdr::ScsiResult r = rig.inquiry(data);
    CHECK(r.ok());
    CHECK_EQ(r.transferred, size_t(36));
    CHECK(std::memcmp(data + 8, "FAKE", 4) == 0);
    CHECK_EQ(rig.device.lastFlags, 0x80);
    CHECK_EQ(rig.device.lastTransferLength, 36u);
    CHECK_EQ(rig.device.lastLun, 0);
    CHECK_EQ(rig.device.lastTag, rig.bot.lastTag());
    CHECK_EQ(rig.device.resets, 0);
}

TEST(tags_increase_per_command) {
    Rig rig;
    CHECK(rig.testUnitReady().ok());
    const uint32_t first = rig.device.lastTag;
    CHECK(rig.testUnitReady().ok());
    CHECK_EQ(rig.device.lastTag, first + 1);
}

TEST(no_data_command) {
    Rig rig;
    const cdr::ScsiResult r = rig.testUnitReady();
    CHECK(r.ok());
    CHECK_EQ(r.transferred, size_t(0));
    CHECK_EQ(rig.device.lastFlags, 0x00);
    CHECK_EQ(rig.device.lastTransferLength, 0u);
}

TEST(lun_is_sent_in_cbw) {
    FakeDrive drive = makeAudioDisc();
    FakeUsbDevice device(drive);
    BulkOnlyTransport bot(device, 0, 1);
    const uint8_t cdb[6] = {};
    CHECK(bot.execute(cdb, sizeof cdb, nullptr, 0, DataDirection::None, 10).ok());
    CHECK_EQ(device.lastLun, 1);
    CHECK_EQ(bot.getMaxLun(), 0);
}

TEST(failed_command_reports_sense) {
    Rig rig;
    rig.drive.discPresent = false;
    const cdr::ScsiResult r = rig.testUnitReady();
    CHECK(r.transportOk);
    CHECK_EQ(r.status, 0x02);
    CHECK_EQ(r.sense.key, 0x2);
    CHECK_EQ(r.sense.asc, 0x3A);
    CHECK_EQ(r.sense.ascq, 0x00);
    CHECK_EQ(rig.device.requestSenses, 1);
    CHECK(rig.device.opcodes == (std::vector<uint8_t>{0x00, 0x03}));
    CHECK_EQ(rig.device.resets, 0);
    CHECK(rig.recovered());
}

TEST(failed_data_in_with_stall) {
    Rig rig;
    rig.drive.discPresent = false;
    std::vector<uint8_t> data;
    const cdr::ScsiResult r = rig.readToc(data);
    CHECK(r.transportOk);
    CHECK_EQ(r.status, 0x02);
    CHECK_EQ(r.sense.asc, 0x3A);
    CHECK_EQ(r.transferred, size_t(0));
    CHECK(data[0] == 0 && data[803] == 0);  // no stale bytes
    CHECK_EQ(rig.device.clearHaltsIn, 1);
    CHECK_EQ(rig.device.resets, 0);
    CHECK(rig.recovered());
}

TEST(failed_data_in_without_data) {
    Rig rig;
    rig.device.stallDataInOnError = false;
    rig.drive.discPresent = false;
    std::vector<uint8_t> data;
    const cdr::ScsiResult r = rig.readToc(data);
    CHECK_EQ(r.status, 0x02);
    CHECK_EQ(r.sense.key, 0x2);
    CHECK_EQ(rig.device.clearHaltsIn, 0);
    CHECK(rig.recovered());
}

TEST(csw_sent_instead_of_data) {
    Rig rig;
    rig.device.cswInsteadOfDataOnError = true;
    rig.drive.discPresent = false;
    std::vector<uint8_t> data;
    const cdr::ScsiResult r = rig.readToc(data);
    CHECK(r.transportOk);
    CHECK_EQ(r.status, 0x02);
    CHECK_EQ(r.sense.asc, 0x3A);
    CHECK_EQ(r.transferred, size_t(0));
    CHECK(data[0] == 0 && data[12] == 0);  // the CSW bytes are not reported as data
    CHECK_EQ(rig.device.resets, 0);
    CHECK(rig.recovered());
}

TEST(short_transfer_uses_residue) {
    Rig rig;
    rig.device.injectShortBy = 6;
    uint8_t data[36];
    std::memset(data, 0xEE, sizeof data);
    const cdr::ScsiResult r = rig.inquiry(data);
    CHECK(r.ok());
    CHECK_EQ(r.transferred, size_t(30));
    CHECK(data[29] != 0xEE);
    CHECK(data[30] == 0 && data[35] == 0);
    CHECK(rig.recovered());
}

TEST(short_read_cd_is_an_error_for_the_drive_layer) {
    Rig rig;
    cdr::CdDrive drive(rig.bot);
    std::vector<uint8_t> buffer(2 * cdr::kSectorBytes);
    rig.device.injectShortBy = cdr::kSectorBytes;
    const cdr::ScsiResult r = drive.readAudio(10, 2, buffer.data());
    CHECK(!r.ok());
    CHECK(r.error.find("short read") != std::string::npos);
    CHECK(drive.readAudio(10, 2, buffer.data()).ok());
}

TEST(bad_csw_signature_triggers_reset_recovery) {
    Rig rig;
    rig.device.injectBadSignature = true;
    uint8_t data[36];
    const cdr::ScsiResult r = rig.inquiry(data);
    CHECK(!r.transportOk);
    CHECK(r.error.find("signature") != std::string::npos);
    CHECK_EQ(rig.device.resets, 1);
    CHECK_EQ(rig.device.clearHaltsIn, 1);
    CHECK_EQ(rig.device.clearHaltsOut, 1);
    CHECK(rig.recovered());
}

TEST(csw_tag_mismatch_triggers_reset_recovery) {
    Rig rig;
    rig.device.injectWrongTag = true;
    const cdr::ScsiResult r = rig.testUnitReady();
    CHECK(!r.transportOk);
    CHECK(r.error.find("tag") != std::string::npos);
    CHECK_EQ(rig.device.resets, 1);
    CHECK(rig.recovered());
}

TEST(phase_error_triggers_reset_recovery) {
    Rig rig;
    rig.device.injectPhaseError = true;
    std::vector<uint8_t> data;
    const cdr::ScsiResult r = rig.readToc(data);
    CHECK(!r.transportOk);
    CHECK(r.error.find("phase error") != std::string::npos);
    CHECK_EQ(rig.device.resets, 1);
    CHECK(rig.recovered());
}

TEST(stalled_csw_is_read_again) {
    Rig rig;
    rig.device.injectCswStall = true;
    uint8_t data[36];
    const cdr::ScsiResult r = rig.inquiry(data);
    CHECK(r.ok());
    CHECK_EQ(r.transferred, size_t(36));
    CHECK_EQ(rig.device.clearHaltsIn, 1);
    CHECK_EQ(rig.device.resets, 0);
}

TEST(stalled_cbw_needs_reset_recovery) {
    Rig rig;
    rig.device.injectCbwStall = true;
    const cdr::ScsiResult r = rig.testUnitReady();
    CHECK(!r.transportOk);
    CHECK(r.error.find("CBW") != std::string::npos);
    CHECK_EQ(rig.device.resets, 1);
    CHECK(rig.recovered());
}

TEST(data_timeout_triggers_reset_recovery) {
    Rig rig;
    rig.device.injectDataTimeout = true;
    uint8_t data[36];
    const cdr::ScsiResult r = rig.inquiry(data);
    CHECK(!r.transportOk);
    CHECK(r.error.find("timeout") != std::string::npos);
    CHECK_EQ(rig.device.resets, 1);
    CHECK(rig.recovered());
}

TEST(invalid_cdb_is_rejected_without_io) {
    Rig rig;
    const uint8_t cdb[17] = {};
    CHECK(!rig.bot.execute(cdb, 17, nullptr, 0, DataDirection::None, 10).transportOk);
    CHECK(!rig.bot.execute(cdb, 0, nullptr, 0, DataDirection::None, 10).transportOk);
    CHECK_EQ(rig.device.commands, 0);
}

TEST(data_out_command) {
    Rig rig;
    // MODE SELECT(10) with an 8-byte parameter list; the fake drive rejects the opcode.
    const uint8_t cdb[10] = {0x55, 0x10, 0, 0, 0, 0, 0, 0, 8, 0};
    uint8_t params[8] = {};
    const cdr::ScsiResult r = rig.bot.execute(cdb, sizeof cdb, params, sizeof params, DataDirection::Out, 10);
    CHECK_EQ(rig.device.dataOutCommands, 1);
    CHECK(rig.device.opcodes == (std::vector<uint8_t>{0x55, 0x03}));
    CHECK(r.transportOk);
    CHECK_EQ(r.status, 0x02);
    CHECK_EQ(r.sense.key, 0x5);
    CHECK_EQ(r.sense.asc, 0x20);
    CHECK(rig.recovered());
}

TEST(drive_layer_over_bulk_only_transport) {
    Rig rig;
    cdr::CdDrive drive(rig.bot);
    CHECK(drive.inquiry().displayName() == "FAKE CD-ROM DRIVE (1.00)");
    CHECK(drive.isReady());
    const cdr::Toc toc = drive.readToc();
    CHECK_EQ(toc.tracks.size(), size_t(3));
    CHECK_EQ(toc.leadOutLba, 750u);
    CHECK_EQ(toc.tracks[1].startLba, 300u);
    rig.drive.discPresent = false;
    CHECK(!drive.isReady());
    bool threw = false;
    try {
        drive.readToc();
    } catch (const cdr::ScsiError& e) {
        threw = true;
        CHECK_EQ(e.result().sense.asc, 0x3A);
    }
    CHECK(threw);
}

// Ripping through BOT gives exactly the same PCM as talking to the drive
// directly, including retries after read errors and offset correction.
TEST(rip_over_bulk_only_transport_matches_direct_rip) {
    auto rip = [](cdr::ScsiTransport& transport, cdr::TrackRipResult& result) {
        cdr::CdDrive drive(transport);
        const cdr::Toc toc = drive.readToc();
        cdr::RipOptions options;
        options.readOffsetSamples = 30;
        cdr::Ripper ripper(drive, toc, options);
        std::vector<uint8_t> bytes;
        uint32_t lastDone = 0;
        result = ripper.ripTrack(
            *toc.findTrack(2), [&](const uint8_t* p, size_t n) { bytes.insert(bytes.end(), p, p + n); },
            [&](uint32_t done, uint32_t) { lastDone = done; });
        CHECK_EQ(lastDone, 150u);
        return bytes;
    };

    FakeDrive direct = makeAudioDisc();
    direct.failuresBySector[320] = 2;
    cdr::TrackRipResult expected;
    const std::vector<uint8_t> reference = rip(direct, expected);

    Rig rig;
    rig.drive.failuresBySector[320] = 2;
    cdr::TrackRipResult got;
    const std::vector<uint8_t> bytes = rip(rig.bot, got);

    CHECK(bytes == reference);
    CHECK_EQ(bytes.size(), size_t(150) * cdr::kSectorBytes);
    CHECK_EQ(got.crc32, expected.crc32);
    CHECK_EQ(got.retries, 2u);
    CHECK_EQ(got.unreadableSectors, 0u);
    CHECK_EQ(rig.device.requestSenses, 2);  // one per failed READ CD
    CHECK_EQ(rig.device.resets, 0);
}

int main() {
    for (auto& [name, fn] : registry()) {
        const int before = failures;
        try {
            fn();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  unexpected exception: %s\n", e.what());
            ++failures;
        }
        std::printf("%s %s\n", failures == before ? "[ OK ]" : "[FAIL]", name);
    }
    std::printf("\n%s (%zu tests)\n", failures ? "FAILED" : "PASSED", registry().size());
    return failures ? 1 : 0;
}
