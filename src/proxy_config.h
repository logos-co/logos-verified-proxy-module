#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

/// Validated configuration for the verified proxy.
///
/// Two of these fields are a SAFETY boundary, not hygiene. `startVerifProxy`
/// catches its own `CatchableError`s and returns NULL for bad JSON, a missing
/// trustedBlockRoot or a malformed URL — but `eth2Network` and `logLevel` are
/// not validated upstream at all, and a bad value reaches a `quit()` that takes
/// the whole HOST process down:
///
///   * an unknown network reaches nimbus-eth2's `getMetadataForNetwork`, whose
///     fallthrough is `fatal "config.yaml not found for network"` + `quit 1`;
///   * a log level Nim's `updateLogLevel` rejects reaches `setupLogging`, which
///     writes to stderr and `quit 1`s.
///
/// So both are whitelisted here, before the value can ever cross the FFI.
struct ProxyConfig {
    // ── Required ─────────────────────────────────────────────────────────
    std::string network = "mainnet";     // -> eth2Network
    std::string trustedBlockRoot;        // 0x + 64 hex
    std::vector<std::string> executionApiUrls;
    std::vector<std::string> beaconApiUrls;

    // ── Optional backends ────────────────────────────────────────────────
    // OP-Stack L2 is enabled by SETTING opExecutionApiUrls, not by naming an
    // op-* network: the library's JSON config has no OP network key (that is a
    // CLI-only option on the standalone binary).
    std::vector<std::string> opExecutionApiUrls;
    std::vector<std::string> privateTxUrls;
    std::vector<std::string> archiveUrls;

    // ── Logging ──────────────────────────────────────────────────────────
    std::string logLevel = "INFO";
    std::string logFormat = "Json";

    // ── Upstream tuning knobs (passed through verbatim) ───────────────────
    int64_t maxBlockWalk = 1000;
    int64_t maxWindowJumps = 500;
    int64_t parallelBlockDownloads = 10;
    int64_t maxLightClientUpdates = 128;
    int64_t headerStoreLen = 256;
    int64_t storageCacheLen = 256;
    int64_t codeCacheLen = 64;
    int64_t accountCacheLen = 128;
    bool syncHeaderStore = true;
    int64_t freezeAtSlot = 0;

    // ── Module-side knobs (never sent upstream) ──────────────────────────
    int64_t callTimeoutMs = 30000;
    int64_t startTimeoutMs = 120000;
    /// Bound on the shutdown drain — a POLLING bound, not a hard one. The drain
    /// loop checks the deadline BETWEEN `processVerifProxyTasks` calls, and a
    /// single such call was measured blocking up to 3253ms on sepolia, so
    /// stop() (and the destructor join) can overshoot this by roughly that
    /// much. Measured stop() in a 15-minute run: 1102ms.
    int64_t drainTimeoutMs = 2000;
    int64_t pumpIntervalMs = 50;
    int64_t maxInFlight = 64;
    /// "off" | "interval" | "continuous". `processVerifProxyTasks` only polls
    /// while `pendingCalls > 0`, so an idle proxy does not advance its light
    /// client at all — the heartbeat is what keeps chronos turning.
    ///
    /// MEASURED on sepolia, 5 minutes idle (2026-08-20):
    ///   off        head 11532988 -> 11532949  (BACKWARDS 39 blocks), 3186ms
    ///   continuous head 11532988 -> 11533012  (+24, i.e. tracking), 0ms
    ///
    /// So "off" is not merely a cold start: the reported head REGRESSES, which
    /// a consumer polling block numbers will see as time running backwards.
    /// Treat it as a diagnostic setting, not a supported deployment.
    std::string keepAlive = "interval";
    int64_t keepAliveIntervalMs = 1000;
    bool autoStart = false;

    /// Optional JSON-RPC 2.0 endpoint in front of the proxy.
    ///
    /// Off by default: a module should not open a listening socket unless
    /// asked. Defaults to loopback when enabled — this endpoint serves chain
    /// state, so binding it to 0.0.0.0 is a deliberate act, not a default.
    bool httpEnabled = false;
    std::string httpHost = "127.0.0.1";
    int64_t httpPort = 8545;

    /// Parse and validate. Returns false and fills `err` with a specific,
    /// actionable message on the first problem found.
    static bool fromJson(const nlohmann::json& in, ProxyConfig& out, std::string& err);

    /// The JSON string `startVerifProxy` expects. Note the URL lists are
    /// COMMA-SEPARATED STRINGS upstream, not arrays.
    std::string toUpstreamJson() const;

    /// Round-trippable view for getConfig(), with URL credentials redacted —
    /// provider URLs routinely carry an API key in the path or query.
    nlohmann::json redacted() const;

    /// The chain id this network must report, or 0 if unknown. The library
    /// hardcodes mainnet->1, sepolia->11155111, hoodi->560048 and does NOT
    /// verify it against the provider (the README says otherwise; it is stale),
    /// so the module checks it after start().
    int64_t expectedChainId() const;
};
