#include "checkpoint_store.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

fs::path CheckpointStore::dirFor(const fs::path& base, const std::string& network,
                                 const std::string& trustedBlockRoot) {
    std::string root = trustedBlockRoot.rfind("0x", 0) == 0 ? trustedBlockRoot.substr(2)
                                                            : trustedBlockRoot;
    std::transform(root.begin(), root.end(), root.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return base / "lc-checkpoints" / (network + "-" + root);
}

bool CheckpointStore::pathFor(const std::string& key, fs::path& out, std::string& err) const {
    // The key becomes a file name, so it may not name anything else.
    const bool safe = !key.empty() && key.size() <= 64
        && std::all_of(key.begin(), key.end(), [](unsigned char c) {
               return std::isalnum(c) || c == '_' || c == '-';
           });
    if (!safe) {
        err = "refusing storage key '" + key + "'";
        return false;
    }
    out = m_dir / (key + ".bin");
    return true;
}

CheckpointStore::Read CheckpointStore::get(const std::string& key, std::string& out,
                                           std::string& err) const {
    fs::path p;
    if (!pathFor(key, p, err)) return Read::Failed;

    std::error_code ec;
    if (!fs::exists(p, ec)) {
        if (!ec) return Read::NotFound;
        err = "cannot stat " + p.string() + ": " + ec.message();
        return Read::Failed;
    }
    const auto size = fs::file_size(p, ec);
    std::ifstream f(p, std::ios::binary);
    if (!ec && f) {
        out.resize(static_cast<size_t>(size));
        if (size > 0) f.read(&out[0], static_cast<std::streamsize>(size));
    }
    if (ec || !f) {
        err = "cannot read " + p.string() + (ec ? ": " + ec.message() : std::string());
        out.clear();
        return Read::Failed;
    }
    return Read::Found;
}

bool CheckpointStore::put(const std::string& key, const uint8_t* data, size_t len,
                          std::string& err) const {
    fs::path p;
    if (!pathFor(key, p, err)) return false;

    std::error_code ec;
    fs::create_directories(m_dir, ec);
    if (ec) {
        err = "cannot create " + m_dir.string() + ": " + ec.message();
        return false;
    }

    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
        f.close();
        if (!f) {
            fs::remove(tmp, ec);
            err = "cannot write " + tmp.string();
            return false;
        }
    }

    fs::rename(tmp, p, ec);
    if (ec) {
        // Some platforms refuse to rename over an existing file.
        std::error_code rmEc;
        fs::remove(p, rmEc);
        ec.clear();
        fs::rename(tmp, p, ec);
    }
    if (ec) {
        err = "cannot replace " + p.string() + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}
