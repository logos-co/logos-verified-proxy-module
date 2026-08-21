#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <logos_json.h>
#include <logos_module_context.h>
#include <logos_result.h>

struct ProxyConfig;
class ProxyRuntime;

/// Light-client-verified Ethereum JSON-RPC.
///
/// Wraps status-im's `libverifproxy` (the C library form of
/// nimbus_verified_proxy). Unlike a plain RPC client, every answer is verified
/// against the beacon-chain light client's attested execution state, with
/// Merkle proofs requested from the untrusted provider — so a lying provider
/// produces an error rather than a wrong answer.
///
/// Lifecycle: configure() -> start() -> call methods -> stop().
///
/// All RPC methods are SYNCHRONOUS: they return the verified result, or an
/// error, within `callTimeoutMs`. Consumers that want concurrency use the
/// generated `<method>Async` twin on their side; this module is
/// `concurrency: "multi"`, so blocked callers do not stall each other.
class VerifiedProxyImpl : public LogosModuleContext {
public:
    VerifiedProxyImpl();
    ~VerifiedProxyImpl();

    // ── Configuration ────────────────────────────────────────────────────

    /// Validate and store the proxy configuration. Synchronous; starts nothing.
    ///
    /// Required: `trustedBlockRoot` (0x + 64 hex), `executionApiUrls` and
    /// `beaconApiUrls` (arrays of http/https/ws/wss URLs). The provider must
    /// support `eth_getProof`.
    ///
    /// `network` is one of mainnet, sepolia, hoodi — enforced here, because an
    /// unrecognised value reaches a `quit()` inside the library and would take
    /// the whole host process down. `logLevel` is whitelisted for the same
    /// reason. OP-Stack L2 is enabled by setting `opExecutionApiUrls`.
    ///
    /// @code{.json}
    /// {
    ///   "network": "mainnet",
    ///   "trustedBlockRoot": "0x...",
    ///   "executionApiUrls": ["wss://..."],
    ///   "beaconApiUrls": ["https://..."],
    ///   "opExecutionApiUrls": [], "privateTxUrls": [], "archiveUrls": [],
    ///   "logLevel": "INFO", "logFormat": "Json",
    ///   "tuning": { "maxBlockWalk": 1000, "headerStoreLen": 256 },
    ///   "callTimeoutMs": 30000, "startTimeoutMs": 120000,
    ///   "keepAlive": "interval", "keepAliveIntervalMs": 1000,   // do not use "off"
    ///   "maxInFlight": 64, "autoStart": false
    /// }
    /// @endcode
    ///
    /// Returns success, or a specific message naming the offending field.
    StdLogosResult configure(const LogosMap& config);

    /// The effective configuration with defaults merged in and provider
    /// credentials redacted. Returns an empty object if configure() has not run.
    LogosMap getConfig();

    // ── Lifecycle ────────────────────────────────────────────────────────

    /// Start the proxy and wait for the light client to initialise.
    ///
    /// Blocks up to `startTimeoutMs`. On success the result value carries the
    /// chain id. Also emits `proxyStarted`.
    StdLogosResult start();

    /// Stop the proxy: drain in-flight calls, then release the context.
    /// Blocks up to `drainTimeoutMs`. Also emits `proxyStopped`.
    StdLogosResult stop();

    /// True when the proxy is running and its last heartbeat succeeded.
    bool ok();

    /// Module and proxy state. Never blocks on the proxy thread.
    ///
    /// @code{.json}
    /// {
    ///   "state": "uninitialized|configured|starting|running|degraded|stopping|stopped|error",
    ///   "network": string, "chainId": number,
    ///   "startedAt": number, "uptimeSeconds": number,
    ///   "head": { "blockNumber": string, "updatedAt": number },
    ///   "counters": { "callsTotal": number, "callsFailed": number,
    ///                 "callsInFlight": number, "leakedCalls": number,
    ///                 "heartbeatFailures": number },
    ///   "lastError": string
    /// }
    /// @endcode
    LogosMap status();

    /// This module's version, as declared in metadata.json.
    std::string moduleVersion();

    /// The nimbus-eth1 revision this module was built against.
    std::string libraryVersion();

    // ── Verified JSON-RPC ────────────────────────────────────────────────

    /// Any method the proxy supports, dispatched through the library's own
    /// `proxyCall`. `params` is a JSON-RPC params array.
    ///
    /// Note that `eth_call`, `eth_estimateGas` and `eth_createAccessList` take
    /// a THIRD positional parameter, `optimisticStateFetch` (a bool) — an
    /// upstream extension to the standard JSON-RPC signature. The typed
    /// wrappers below supply it for you.
    ///
    /// Returns the decoded result value on success.
    StdLogosResult rpc(const std::string& method, const LogosList& params);

    /// Current verified head block number.
    ///
    /// Returns a JSON **number**, not a hex quantity string — upstream's
    /// encoding is not uniform (`ethChainId` and `ethGasPrice` do return hex
    /// strings). Measured against sepolia, not inferred from the JSON-RPC spec.
    StdLogosResult ethBlockNumber();

    /// The chain id the proxy is configured for, as a hex quantity string.
    StdLogosResult ethChainId();

    /// Verified account balance in wei, as a hex quantity string.
    /// `blockTag` is "latest", "pending", "earliest", or a hex block number.
    StdLogosResult ethGetBalance(const std::string& address, const std::string& blockTag);

    /// Verified contract code at `address`, as a hex byte string.
    StdLogosResult ethGetCode(const std::string& address, const std::string& blockTag);

    /// Verified block. `fullTransactions` selects full objects over hashes.
    StdLogosResult ethGetBlockByNumber(const std::string& blockTag, bool fullTransactions);

    /// Verified `eth_call`. `txArgs` is a transaction object ({to, data, ...}).
    /// `optimisticStateFetch` trades a stricter state check for latency.
    StdLogosResult ethCall(const LogosMap& txArgs, const std::string& blockTag,
                           bool optimisticStateFetch);

    /// Verified transaction by index within a block.
    StdLogosResult ethGetTransactionByBlockNumberAndIndex(const std::string& blockTag,
                                                          uint64_t index);

logos_events:
    /// Emitted when start() finishes. {"success":bool,"chainId":number,"error":string}
    void proxyStarted(const std::string& payload);

    /// Emitted when stop() finishes. {"success":bool}
    void proxyStopped(const std::string& payload);

    /// Emitted on every proxy state transition.
    /// {"state":string,"previous":string,"error":string}
    void proxyStateChanged(const std::string& payload);

protected:
    void onContextReady() override;

private:
    // Held by pointer so this header — which the code generator parses as TEXT
    // to derive the module's contract — stays free of the FFI and config types.
    std::unique_ptr<ProxyConfig> m_cfg;
    std::unique_ptr<ProxyRuntime> m_rt;
    bool m_configured = false;
};
