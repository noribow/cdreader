// Embedded CUE sheet checked with the reference tools: a disc image written
// by FlacWriter must pass `flac -t`, `metaflac --export-cuesheet-to` must give
// the expected track positions, and re-importing that sheet with metaflac
// (which validates it as CD-DA) must produce a byte-identical CUESHEET block.
// The sheet carries an MCN (CATALOG) and ISRCs (#22), which must survive the
// round trip, and so must a sheet with an HTOA, a pregap (INDEX 00) and an
// INDEX 02 (#25). With ffprobe, the ISRC / BARCODE tags of a FLAC track and of a
// WAV track (ID3 chunk) must be readable as well.
//
// Usage: cdreader_flac_cuesheet_check <flac> <metaflac> [<ffprobe>]
// Exits with 77 (reported as "skipped" by ctest) when a tool is missing.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cdreader/cue_sheet.h"
#include "cdreader/flac_writer.h"
#include "cdreader/gaps.h"
#include "cdreader/wav_writer.h"
#include "test_temp.h"

namespace fs = std::filesystem;

namespace {

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> metadataBlock(const fs::path& path, int type) {
    const std::string f = readText(path);
    size_t pos = 4;
    for (bool last = false; !last && pos + 4 <= f.size();) {
        const uint8_t header = uint8_t(f[pos]);
        last = (header & 0x80) != 0;
        const size_t length =
            size_t(uint8_t(f[pos + 1])) << 16 | size_t(uint8_t(f[pos + 2])) << 8 | uint8_t(f[pos + 3]);
        if ((header & 0x7F) == type) return std::vector<uint8_t>(f.begin() + long(pos + 4), f.begin() + long(pos + 4 + length));
        pos += 4 + length;
    }
    return {};
}

std::string quote(const fs::path& p) { return "\"" + p.string() + "\""; }

int run(const std::string& command) {
#ifdef _WIN32
    return std::system(("\"" + command + "\"").c_str());  // cmd.exe strips the outer quotes
#else
    return std::system(command.c_str());
#endif
}

std::string lower(std::string s) {
    for (char& c : s) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
    failures += ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || std::string(argv[1]).empty() || std::string(argv[2]).empty()) {
        std::printf("flac / metaflac not available - skipping\n");
        return 77;
    }
    const std::string flac = quote(fs::u8path(argv[1]));
    const std::string metaflac = quote(fs::u8path(argv[2]));
    const fs::path dir = cdr_test::testTempDir() / "cdreader_flac_cuesheet";
    fs::create_directories(dir);
    const fs::path image = dir / "image.flac";
    const fs::path copy = dir / "reimported.flac";
    const fs::path exported = dir / "exported.cue";

    // Three tracks of 200, 150 and 301 sectors, the second with pre-emphasis.
    cdr::AlbumMetadata album;
    album.artist = "Artist";
    album.title = "Album";
    album.trackTitles = {"One", "Two", "Three"};
    album.mcn = "4988001234567";
    album.trackIsrcs = {"JPVI09912345", "", "USRC17607839"};  // track 2 has none
    std::vector<cdr::Track> tracks(3);
    const uint32_t lengths[] = {200, 150, 301};
    uint32_t lba = 0;
    for (size_t i = 0; i < tracks.size(); ++i) {
        tracks[i].number = int(i) + 1;
        tracks[i].startLba = lba;
        tracks[i].lengthSectors = lengths[i];
        lba += lengths[i];
    }
    tracks[1].preEmphasis = true;
    cdr::EmbeddedCueSheet cue;
    cue.tracks = cdr::singleFileCueTracks(tracks, "image.flac", album);
    cue.totalSectors = lba;
    cue.mcn = album.mcn;
    cue.text = cdr::formatCueSheet(album, cue.tracks);

    cdr::FlacWriter writer;
    writer.setEmbeddedCueSheet(cue);
    writer.open(image, album.forTrack(0, 3));
    std::vector<uint8_t> pcm(size_t(lba) * cdr::kSectorBytes);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = uint8_t(i * 7 + (i >> 9));
    writer.write(pcm.data(), pcm.size());
    writer.close();

    check(run(flac + " -t -s " + quote(image)) == 0, "flac -t accepts the image");
    check(run(metaflac + " --export-cuesheet-to=" + quote(exported) + " " + quote(image)) == 0,
          "metaflac exports the CUESHEET block");
    const std::string sheet = readText(exported);
    check(sheet.find("CATALOG 4988001234567\n") != std::string::npos, "MCN exported as CATALOG");
    check(sheet.find("  TRACK 01 AUDIO\n    ISRC JPVI09912345\n    INDEX 01 00:00:00") != std::string::npos,
          "track 1 at 00:00:00 with its ISRC");
    check(sheet.find("  TRACK 02 AUDIO\n    FLAGS PRE\n    INDEX 01 00:02:50") != std::string::npos,
          "track 2 at 00:02:50 (200 sectors) with pre-emphasis");
    check(sheet.find("  TRACK 03 AUDIO\n    ISRC USRC17607839\n    INDEX 01 00:04:50") != std::string::npos,
          "track 3 at 00:04:50 (350 sectors) with its ISRC");
    check(sheet.find("REM FLAC__lead-out 170 " + std::to_string(uint64_t(lba) * 588)) != std::string::npos,
          "lead-out at the end of the audio");

    fs::copy_file(image, copy, fs::copy_options::overwrite_existing);
    const bool reimported = run(metaflac + " --remove --block-type=CUESHEET " + quote(copy)) == 0 &&
                            run(metaflac + " --import-cuesheet-from=" + quote(exported) + " " + quote(copy)) == 0;
    check(reimported, "metaflac re-imports the sheet (CD-DA validation)");
    check(reimported && metadataBlock(copy, 5) == metadataBlock(image, 5) && !metadataBlock(image, 5).empty(),
          "re-imported CUESHEET block is byte-identical");

    // A disc with an HTOA (track 1 at 00:01:00), a 2-second pregap before
    // track 2 and an INDEX 02 in track 3 (#25): the CUESHEET block starts
    // track 1 at INDEX 00 (offset 0) and track 2 at its INDEX 00, and must
    // round-trip through metaflac byte for byte as well.
    {
        const fs::path gapImage = dir / "gaps.flac";
        const fs::path gapCopy = dir / "gaps_reimported.flac";
        const fs::path gapExported = dir / "gaps.cue";
        std::vector<cdr::Track> discTracks = {{1, 75, 300}, {2, 375, 450}, {3, 825, 226}};
        discTracks[1].preEmphasis = true;
        cdr::DiscGaps gaps;
        gaps.status = cdr::DiscGaps::Status::Detected;
        gaps.htoaSectors = 75;
        for (const cdr::Track& t : discTracks) {
            cdr::TrackIndexes ti;
            ti.track = t.number;
            ti.index01Lba = t.startLba;
            ti.status = cdr::TrackIndexes::Status::Detected;
            gaps.tracks.push_back(ti);
        }
        gaps.tracks[0].pregapSectors = 75;
        gaps.tracks[1].pregapSectors = 150;
        gaps.tracks[2].laterIndexes = {900};
        cdr::EmbeddedCueSheet gapCue;
        gapCue.tracks = cdr::singleFileCueTracks(discTracks, "gaps.flac", album, gaps, true);
        gapCue.totalSectors = 1051;
        gapCue.mcn = album.mcn;
        gapCue.text = cdr::formatCueSheet(album, gapCue.tracks);
        cdr::FlacWriter gapWriter;
        gapWriter.setEmbeddedCueSheet(gapCue);
        gapWriter.open(gapImage, album.forTrack(0, 3));
        std::vector<uint8_t> gapPcm(size_t(1051) * cdr::kSectorBytes);
        for (size_t i = 0; i < gapPcm.size(); ++i) gapPcm[i] = uint8_t(i * 5 + (i >> 11));
        gapWriter.write(gapPcm.data(), gapPcm.size());
        gapWriter.close();

        check(run(flac + " -t -s " + quote(gapImage)) == 0, "flac -t accepts the image with pregaps");
        check(run(metaflac + " --export-cuesheet-to=" + quote(gapExported) + " " + quote(gapImage)) == 0,
              "metaflac exports the CUESHEET block with INDEX 00");
        const std::string gapSheet = readText(gapExported);
        check(gapSheet.find("  TRACK 01 AUDIO\n    ISRC JPVI09912345\n    INDEX 00 00:00:00\n    INDEX 01 00:01:00\n") !=
                  std::string::npos,
              "HTOA: track 1 INDEX 00 at 00:00:00, INDEX 01 at 00:01:00");
        check(gapSheet.find("  TRACK 02 AUDIO\n    FLAGS PRE\n    INDEX 00 00:03:00\n    INDEX 01 00:05:00\n") !=
                  std::string::npos,
              "track 2 INDEX 00 at 00:03:00, INDEX 01 at 00:05:00");
        check(gapSheet.find("    INDEX 01 00:11:00\n    INDEX 02 00:12:00\n") != std::string::npos,
              "track 3 INDEX 01 at 00:11:00, INDEX 02 at 00:12:00");
        fs::copy_file(gapImage, gapCopy, fs::copy_options::overwrite_existing);
        const bool gapReimported =
            run(metaflac + " --remove --block-type=CUESHEET " + quote(gapCopy)) == 0 &&
            run(metaflac + " --import-cuesheet-from=" + quote(gapExported) + " " + quote(gapCopy)) == 0;
        check(gapReimported, "metaflac re-imports the sheet with INDEX 00 (CD-DA validation)");
        check(gapReimported && metadataBlock(gapCopy, 5) == metadataBlock(gapImage, 5) &&
                  !metadataBlock(gapImage, 5).empty(),
              "re-imported CUESHEET block with INDEX 00 is byte-identical");
        if (failures) std::printf("%s\n", gapSheet.c_str());
    }

    // ffprobe reads the ISRC (Vorbis comment ISRC, ID3 TSRC) and the MCN
    // (BARCODE) of single tracks.
    if (argc > 3 && *argv[3]) {
        const std::string ffprobe = quote(fs::u8path(argv[3]));
        const cdr::TrackMetadata track = album.forTrack(1, 3);
        const std::vector<uint8_t> trackPcm(pcm.begin(), pcm.begin() + 200 * cdr::kSectorBytes);
        const fs::path flacTrack = dir / "track.flac";
        const fs::path wavTrack = dir / "track.wav";
        cdr::FlacWriter flacWriter;
        flacWriter.open(flacTrack, track);
        flacWriter.write(trackPcm.data(), trackPcm.size());
        flacWriter.close();
        cdr::WavWriter wavWriter;
        wavWriter.open(wavTrack, track);
        wavWriter.write(trackPcm.data(), trackPcm.size());
        wavWriter.close();
        for (const fs::path& file : {flacTrack, wavTrack}) {
            const fs::path out = dir / "tags.txt";
            const int status = run(ffprobe + " -v error -show_entries format_tags -of default=noprint_wrappers=1 " +
                                   quote(file) + " > " + quote(out) + " 2>&1");
            const std::string tags = lower(readText(out));
            // Vorbis comment keys are kept; FFmpeg reports the ID3 frame as TSRC.
            const std::string isrc = file == wavTrack ? "tag:tsrc=jpvi09912345\n" : "tag:isrc=jpvi09912345\n";
            const bool ok = status == 0 && tags.find(isrc) != std::string::npos &&
                            tags.find("tag:barcode=4988001234567\n") != std::string::npos;
            check(ok, "ffprobe reads ISRC and BARCODE from " + file.filename().string());
            if (!ok) std::printf("%s\n", readText(out).c_str());
        }
    } else {
        std::printf("ffprobe not available - tag check skipped\n");
    }

    fs::remove_all(dir);
    std::printf("\n%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
