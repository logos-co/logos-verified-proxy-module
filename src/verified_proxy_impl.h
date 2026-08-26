#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <logos_json.h>
#include <logos_module_context.h>
#include <logos_result.h>

struct ProxyConfig;
class ProxyRuntime;
class RpcHttpServer;

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
    ///   "httpServer": { "enabled": false, "host": "127.0.0.1", "port": 8545 },
    ///   "maxInFlight": 64, "autoStart": false
    /// }
    /// @endcode
    ///
    /// Returns success, or a specific message naming the offending field.
    StdLogosResult configure(const LogosMap& config);

    /// The effective configuration with defaults merged in and provider
    /// credentials redacted. Returns an empty object if configure() has not run.
    LogosMap getConfig();

    /// The same configuration WITHOUT redaction, for restoring a form.
    ///
    /// configure() persists what it accepts, and onContextReady() reloads it,
    /// so the module already remembers the last working setup across restarts.
    /// A UI repopulating its fields needs the real URLs, which getConfig()
    /// deliberately masks because provider endpoints can carry API keys.
    ///
    /// Returns an empty object if configure() has not run. Treat the result as
    /// a credential: do not log it, and prefer getConfig() anywhere the values
    /// are only being displayed.
    LogosMap getConfigUnredacted();

    /// A complete, ready-to-submit configuration for `network`, with this
    /// module's defaults filled in — the endpoint pair from the network table
    /// plus every tuning and timeout default.
    ///
    /// Intended for bootstrapping from a CLI:
    ///
    ///     logosctl call verified_proxy_module defaultConfig mainnet
    ///
    /// gives a template to edit. `trustedBlockRoot` is deliberately left EMPTY
    /// — it anchors the entire trust model and cannot be defaulted; fetch one
    /// with fetchFinalizedRoot() or supply one you already trust.
    ///
    /// Returns an empty object for an unsupported network.
    LogosMap defaultConfig(const std::string& network);

    // ── Lifecycle ────────────────────────────────────────────────────────

    /// Start the proxy and wait for the light client to initialise.
    ///
    /// Blocks up to `startTimeoutMs`. On success the result value carries the
    /// chain id. Also emits `proxyStarted`.
    StdLogosResult start();

    /// Stop the proxy: drain in-flight calls, then release the context.
    ///
    /// `drainTimeoutMs` (default 2000) bounds when the drain stops STARTING new
    /// turns of the library's task pump — not when this returns. The deadline is
    /// checked between calls into the library, and one such call was measured
    /// blocking for up to 3.3s, so budget `drainTimeoutMs` plus up to one pump
    /// duration. Bounded, but not tight: a measured stop() took 1102ms.
    ///
    /// Callers still blocked in an RPC call are released with
    /// "proxy shutting down" rather than waiting out their own timeout.
    ///
    /// Also emits `proxyStopped`.
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

    /// URL of this module's own JSON-RPC endpoint, or "" when it is not
    /// running (`httpServer.enabled` defaults to false).
    ///
    /// This is the integration seam for anything that speaks plain JSON-RPC:
    /// hand it to `eth_rpc_module`'s `ChainConfig.endpoint`, or to ethers /
    /// viem / cast, and their reads become light-client-verified without any
    /// of them knowing this module exists.
    ///
    /// Note the library itself ships no server — `libverifproxy` never imports
    /// `json_rpc_frontend`, and its symbols are absent from the archive. This
    /// endpoint is the module's, forwarding to the same verified `proxyCall`
    /// path the typed methods use.
    std::string localEndpoint();

    /// The networks this module accepts, each with its chain id and a
    /// suggested public endpoint pair.
    ///
    /// A UI should build its network selector from THIS rather than hardcoding
    /// a list: `network` is one of exactly two config fields whose value
    /// reaches a `quit()` inside the Nim library when upstream does not
    /// recognise it, taking the whole host process down, so a UI list that
    /// drifts from the module's whitelist is a crash waiting to happen.
    ///
    /// Returns a list of `{"name", "chainId", "beaconApiUrl",
    /// "executionApiUrl"}`. The two URLs are a convenience for prefilling a
    /// form and may be EMPTY, which means no public endpoint is known to meet
    /// the requirements for that network — a beacon endpoint has to serve the
    /// light-client REST API and an execution endpoint has to support
    /// `eth_getProof`. Empty is deliberate: a plausible URL that cannot
    /// actually verify is worse than none, because it fails long after the
    /// choice that caused it.
    LogosList supportedNetworks();

    /// Fetch the current finalized beacon block root from `beaconUrl`.
    ///
    /// A convenience for operators who have no root to hand: it queries
    /// `<beaconUrl>/eth/v1/beacon/headers/finalized` and returns
    /// `{"root": "0x…", "slot": "…"}`. Takes the URL as an argument rather
    /// than reading the stored config so it is usable before configure().
    ///
    /// This is deliberately NOT part of the trust model. A root fetched from
    /// the same endpoint you are about to distrust anchors nothing — it is a
    /// starting point for testing, and an operator running against real value
    /// should take the root from a source they independently trust and paste
    /// it in. The method lives here rather than in a UI because Basecamp
    /// sandboxes `ui_qml` plugins away from the network entirely; a core
    /// module is the only component allowed to make the request.
    StdLogosResult fetchFinalizedRoot(const std::string& beaconUrl);

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
    std::unique_ptr<RpcHttpServer> m_http;
    bool m_configured = false;
};
