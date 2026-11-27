#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

/// A fresh directory under the system temp dir, removed with everything in it.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        static std::atomic<int> n{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
             / ("vp-test-" + std::to_string(stamp) + "-" + std::to_string(n++));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};
