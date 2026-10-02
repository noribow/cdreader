#pragma once

// A scratch directory private to this test process (#30), used instead of
// std::filesystem::temp_directory_path() so that test executables run in
// parallel (ctest -j, several build trees) never share a file name.
//
// The directory is created under the system temp directory on first use as
// "cdreader_test_<random>" and removed with everything in it when the
// process exits. Set CDREADER_KEEP_TEST_TEMP=1 to keep it for inspection.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

namespace cdr_test {

class ProcessTempDir {
public:
    ProcessTempDir() {
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        std::random_device device;
        std::mt19937_64 random((uint64_t(device()) << 32) ^ device());
        for (int attempt = 0; attempt < 100; ++attempt) {
            char name[40];
            std::snprintf(name, sizeof name, "cdreader_test_%016llx", static_cast<unsigned long long>(random()));
            const std::filesystem::path candidate = base / name;
            std::error_code ec;
            // create_directory() is false when the name already exists, so a
            // directory is never shared with another process.
            if (std::filesystem::create_directory(candidate, ec)) {
                path_ = candidate;
                return;
            }
        }
        throw std::runtime_error("cannot create a test directory in " + base.string());
    }
    ~ProcessTempDir() {
        const char* keep = std::getenv("CDREADER_KEEP_TEST_TEMP");
        if (keep != nullptr && *keep != '\0' && std::string(keep) != "0") {
            std::fprintf(stderr, "test files kept in %s\n", path_.string().c_str());
            return;
        }
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ProcessTempDir(const ProcessTempDir&) = delete;
    ProcessTempDir& operator=(const ProcessTempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// The process's scratch directory (created on the first call).
inline const std::filesystem::path& testTempDir() {
    static const ProcessTempDir dir;
    return dir.path();
}

}  // namespace cdr_test
