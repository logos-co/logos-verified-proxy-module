#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

/// libverifproxy's light-client checkpoint on disk, behind its storage
/// callbacks (nimbus-eth1#5103): one file per key the library names.
class CheckpointStore {
public:
    /// One directory per (network, trusted root). The library restores a stored
    /// checkpoint without checking it against trustedBlockRoot, so changing
    /// either must start from the new anchor, not inherit the old one's.
    static std::filesystem::path dirFor(const std::filesystem::path& base,
                                        const std::string& network,
                                        const std::string& trustedBlockRoot);

    explicit CheckpointStore(std::filesystem::path dir) : m_dir(std::move(dir)) {}

    enum class Read { Found, NotFound, Failed };
    Read get(const std::string& key, std::string& out, std::string& err) const;

    /// Replaces the value through a temporary file and a rename, so a crash
    /// mid-write leaves the previous checkpoint rather than a torn one.
    bool put(const std::string& key, const uint8_t* data, size_t len, std::string& err) const;

    const std::filesystem::path& dir() const { return m_dir; }

private:
    bool pathFor(const std::string& key, std::filesystem::path& out, std::string& err) const;

    std::filesystem::path m_dir;
};
