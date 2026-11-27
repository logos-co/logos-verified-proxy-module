// The file layer behind libverifproxy's storage callbacks.

#include <filesystem>
#include <fstream>
#include <string>

#include <logos_test.h>

#include "checkpoint_store.h"
#include "temp_dir.h"

namespace fs = std::filesystem;

namespace {

// Binary on purpose: the library's value is a fork byte and SSZ.
std::string sample(const char* tail) {
    return std::string(1, '\x02') + std::string(1, '\0') + tail;
}

const std::string kRoot = "0x" + std::string(64, 'a');

} // namespace

LOGOS_TEST(checkpoint_store_round_trips_a_binary_value) {
    TempDir dir;
    CheckpointStore store(dir.path / "cp");
    const std::string value = sample("checkpoint");
    std::string err;

    LOGOS_ASSERT_TRUE(store.put("lc_checkpoint",
                                reinterpret_cast<const uint8_t*>(value.data()),
                                value.size(), err));
    std::string got;
    LOGOS_ASSERT_TRUE(store.get("lc_checkpoint", got, err) == CheckpointStore::Read::Found);
    LOGOS_ASSERT_TRUE(got == value);
}

LOGOS_TEST(checkpoint_store_reports_a_missing_key_as_not_found) {
    TempDir dir;
    CheckpointStore store(dir.path / "never-written");
    std::string got, err;
    LOGOS_ASSERT_TRUE(store.get("lc_checkpoint", got, err) == CheckpointStore::Read::NotFound);
    LOGOS_ASSERT_TRUE(got.empty());
}

LOGOS_TEST(checkpoint_store_replaces_a_value_and_leaves_no_temporary_file) {
    TempDir dir;
    CheckpointStore store(dir.path);
    const std::string first = sample("first, and longer than the second");
    const std::string second = sample("second");
    std::string err;

    LOGOS_ASSERT_TRUE(store.put("lc_checkpoint", reinterpret_cast<const uint8_t*>(first.data()),
                                first.size(), err));
    LOGOS_ASSERT_TRUE(store.put("lc_checkpoint", reinterpret_cast<const uint8_t*>(second.data()),
                                second.size(), err));
    std::string got;
    LOGOS_ASSERT_TRUE(store.get("lc_checkpoint", got, err) == CheckpointStore::Read::Found);
    LOGOS_ASSERT_TRUE(got == second);
    LOGOS_ASSERT_FALSE(fs::exists(dir.path / "lc_checkpoint.bin.tmp"));
}

LOGOS_TEST(checkpoint_store_refuses_a_key_that_names_a_path) {
    TempDir dir;
    CheckpointStore store(dir.path / "cp");
    const std::string value = sample("x");
    std::string got, err;

    LOGOS_ASSERT_FALSE(store.put("../escape", reinterpret_cast<const uint8_t*>(value.data()),
                                 value.size(), err));
    LOGOS_ASSERT_FALSE(err.empty());
    LOGOS_ASSERT_FALSE(fs::exists(dir.path / "escape.bin"));
    LOGOS_ASSERT_TRUE(store.get("a/b", got, err) == CheckpointStore::Read::Failed);
    LOGOS_ASSERT_TRUE(store.get("", got, err) == CheckpointStore::Read::Failed);
}

LOGOS_TEST(checkpoint_store_fails_a_write_it_cannot_place) {
    TempDir dir;
    // A FILE where the store's directory should be.
    { std::ofstream(dir.path / "blocked") << "x"; }
    CheckpointStore store(dir.path / "blocked");
    const std::string value = sample("x");
    std::string err;
    LOGOS_ASSERT_FALSE(store.put("lc_checkpoint", reinterpret_cast<const uint8_t*>(value.data()),
                                 value.size(), err));
    LOGOS_ASSERT_FALSE(err.empty());
}

LOGOS_TEST(checkpoint_store_gives_each_network_and_root_its_own_directory) {
    const fs::path base = "/persist";
    const fs::path sepolia = CheckpointStore::dirFor(base, "sepolia", kRoot);

    LOGOS_ASSERT_TRUE(sepolia == base / "lc-checkpoints" / ("sepolia-" + std::string(64, 'a')));
    LOGOS_ASSERT_TRUE(CheckpointStore::dirFor(base, "mainnet", kRoot) != sepolia);
    LOGOS_ASSERT_TRUE(CheckpointStore::dirFor(base, "sepolia", "0x" + std::string(64, 'b'))
                      != sepolia);
    // The root's hex case is not part of its identity.
    LOGOS_ASSERT_TRUE(CheckpointStore::dirFor(base, "sepolia", "0x" + std::string(64, 'A'))
                      == sepolia);
}
