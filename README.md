# logos-verified-proxy-module

Light-client-verified Ethereum JSON-RPC for Logos, wrapping status-im's
[`nimbus_verified_proxy`](https://github.com/status-im/nimbus-eth1/tree/master/nimbus_verified_proxy)
in its C library form (`libverifproxy`).

An ordinary RPC client forwards a request to a provider and **trusts the answer**.
This module doesn't: it syncs the beacon-chain light client from a trusted block
root and verifies every `eth_*` response against the attested execution state,
requesting Merkle proofs from the (untrusted) provider. A provider that lies
produces an *error*, not a wrong value.

No extra process, no local port — the verification runs in-process, and results
come back over the normal Logos RPC surface.

## Quick start

```bash
nix build && lm methods ./result/lib/verified_proxy_module_plugin.so
```

Get a trusted block root — this is the root of trust, so take it from a source
you trust, not from the same provider you are about to verify:

```bash
curl -s https://beaconstate.info/eth/v1/beacon/headers/finalized | jq -r '.data.root'
```

`config.json`:

```json
{
  "network": "mainnet",
  "trustedBlockRoot": "0x...",
  "executionApiUrls": ["wss://eth-mainnet.example/v2/<key>"],
  "beaconApiUrls":    ["https://beaconstate.info"]
}
```

Then:

```bash
logosctl call verified_proxy_module configure json:@config.json
```

```bash
logosctl call verified_proxy_module start && logosctl call verified_proxy_module status
```

```bash
logosctl call verified_proxy_module ethGetBalance 0xd8dA6BF26964aF9D7eEd9e03E53415D37aA96045 latest
```

The execution provider **must support `eth_getProof`** — verification is
impossible without it. (Infura notably does not.)

## API

| Method | Notes |
|---|---|
| `configure(config)` | Validate and store config. Synchronous; starts nothing. |
| `getConfig()` | Effective config, credentials redacted. |
| `start()` / `stop()` | Blocking, bounded by `startTimeoutMs` / `drainTimeoutMs`. |
| `ok()` / `status()` | Health probe and full state. `status()` never blocks on the proxy thread. |
| `rpc(method, params)` | Any method the proxy supports. `params` is a JSON-RPC array. |
| `ethBlockNumber()`, `ethGetBalance(...)`, `ethCall(...)`, … | Typed wrappers over the same path. |

Events: `proxyStarted`, `proxyStopped`, `proxyStateChanged`.

All RPC methods are **synchronous** — they return the verified result, or an
error, within `callTimeoutMs`. Consumers that want concurrency use the generated
`<method>Async` twin on their side; the module is `concurrency: "multi"`, so
blocked callers do not stall each other.

### `rpc()` and `optimisticStateFetch`

`eth_call`, `eth_estimateGas` and `eth_createAccessList` take a **third
positional parameter**, `optimisticStateFetch` (a bool) — an upstream extension
to the standard JSON-RPC signature. The typed wrappers supply it; anything
calling `rpc()` with a hand-built params array must too.

## Configuration

Required: `trustedBlockRoot` (`0x` + 64 hex), `executionApiUrls`,
`beaconApiUrls`. `network` is one of `mainnet`, `sepolia`, `hoodi`. OP-Stack L2
is enabled by setting `opExecutionApiUrls` (there is no `op-*` network name in
the library's JSON config — that is a CLI-only option on the standalone binary).

Module-side knobs: `callTimeoutMs` (30000), `startTimeoutMs` (120000),
`drainTimeoutMs` (2000), `pumpIntervalMs` (50), `maxInFlight` (64),
`keepAlive` (`off` | `interval` | `continuous`), `keepAliveIntervalMs` (1000),
`autoStart` (false). Upstream tuning lives under `tuning`.

Config is persisted to the host-provided per-instance directory and reloaded on
load. `VERIFIED_PROXY_MODULE_CONFIG` (inline JSON or a path) supplies a
deploy-time default.

### Two fields are validated for safety, not tidiness

`network` and `logLevel` are whitelisted **before** they can reach the library,
because an unrecognised value there reaches a Nim `quit()` that would terminate
the whole host process:

* an unknown network reaches nimbus-eth2's `getMetadataForNetwork`, whose
  fallthrough is `fatal` + `quit 1`;
* a log level Nim's `updateLogLevel` rejects reaches `setupLogging`'s `quit 1`.

Neither is validated upstream. Everything else — bad JSON, a missing
`trustedBlockRoot`, a malformed URL — is already caught and turned into a
`NULL` return, so validating it here only improves the error message.

## The keep-alive, and a caveat worth knowing

`processVerifProxyTasks` only advances chronos while a call is in flight, so an
**idle proxy does not advance its light client**. The heartbeat
(`keepAlive: "interval"`, the default) issues `eth_syncing`, which drives the
sync loop and touches no execution backend. `keepAlive: "continuous"` keeps it
turning permanently; `"off"` accepts cold starts.

Sync observability is limited by the library: there is no exported getter for
the finalized/optimistic slot, and `eth_syncing` returns a hardcoded `false`.
`status().state == "degraded"` therefore means "up, but heartbeats are failing",
inferred from those calls' error strings.

## Development

```bash
nix build .#unit-tests && ./result-tests/bin/verified_proxy_module_tests
```

Unit tests link a mocked `libverifproxy` (`tests.mockCLibs`), so they never
build the ~25-minute upstream toolchain. Unlike a synchronous mock, it queues
completions and drains them only from the pump, so the cross-thread design is
actually exercised.

```bash
nix build .#libverifproxy   # the upstream archive alone (slow, cached)
```

## Licence

MIT / Apache-2.0, matching the Logos workspace.
