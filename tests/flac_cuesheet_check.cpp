// Embedded CUE sheet checked with the reference tools: a disc image written
// by FlacWriter must pass `flac -t`, `metaflac --export-cuesheet-to` must give
// the expected track positions, and re-importing that sheet with metaflac
// (which validates it as CD-DA) must produce a byte-identical CUESHEET block.
//
// Usage: cdreader_flac_cuesheet_check <flac> <metaflac>
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
    const fs::path dir = fs::temp_directory_path() / "cdreader_flac_cuesheet";
    fs::create_directories(dir);
    const fs::path image = dir / "image.flac";
    const fs::path copy = dir / "reimported.flac";
    const fs::path exported = dir / "exported.cue";

    // Three tracks of 200, 150 and 301 sectors, the second with pre-emphasis.
    cdr::AlbumMetadata album;
    album.artist = "Artist";
    album.title = "Album";
    album.trackTitles = {"One", "Two", "Three"};
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
    check(sheet.find("  TRACK 01 AUDIO\n    INDEX 01 00:00:00") != std::string::npos, "track 1 at 00:00:00");
    check(sheet.find("  TRACK 02 AUDIO\n    FLAGS PRE\n    INDEX 01 00:02:50") != std::string::npos,
          "track 2 at 00:02:50 (200 sectors) with pre-emphasis");
    check(sheet.find("  TRACK 03 AUDIO\n    INDEX 01 00:04:50") != std::string::npos, "track 3 at 00:04:50 (350 sectors)");
    check(sheet.find("REM FLAC__lead-out 170 " + std::to_string(uint64_t(lba) * 588)) != std::string::npos,
          "lead-out at the end of the audio");

    fs::copy_file(image, copy, fs::copy_options::overwrite_existing);
    const bool reimported = run(metaflac + " --remove --block-type=CUESHEET " + quote(copy)) == 0 &&
                            run(metaflac + " --import-cuesheet-from=" + quote(exported) + " " + quote(copy)) == 0;
    check(reimported, "metaflac re-imports the sheet (CD-DA validation)");
    check(reimported && metadataBlock(copy, 5) == metadataBlock(image, 5) && !metadataBlock(image, 5).empty(),
          "re-imported CUESHEET block is byte-identical");

    fs::remove_all(dir);
    std::printf("\n%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
