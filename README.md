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
| `getConfig()` | Effective config, credentials redacted. Use this to DISPLAY configuration. |
| `getConfigUnredacted()` | The same, unredacted, so a UI can repopulate a form. Treat as a credential. |
| `defaultConfig(network)` | A complete, ready-to-submit config for a network. `trustedBlockRoot` is left empty. |
| `start()` | Blocks until the light client initialises, bounded by `startTimeoutMs`. |
| `stop()` | Drains, then releases. See the note on `drainTimeoutMs` below — it is not a tight bound. |
| `ok()` / `status()` | Health probe and full state. `status()` never blocks on the proxy thread. |
| `supportedNetworks()` | The accepted networks with their chain ids and a default endpoint pair. Build a UI selector from this, not a hardcoded list. |
| `fetchFinalizedRoot(beaconUrl)` | Convenience: asks a beacon node for its current finalized root. **Not** a trust anchor — see below. |
| `rpc(method, params)` | Any method the proxy supports. `params` is a JSON-RPC array. |
| `ethBlockNumber()`, `ethGetBalance(...)`, `ethCall(...)`, … | 30 typed `eth*` wrappers over the same path. |
| `opBlockNumber()`, `opGetBalance(...)`, … | The 30 `op*` mirrors. Need an OP-Stack network and `opExecutionApiUrls`. |

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

## The JSON-RPC endpoint

`libverifproxy` deliberately ships **no** server — `library/verifproxy.nim`
imports `json_rpc_backend` (the client it calls providers with) and the
in-process `engine/rpc_frontend`, but never `json_rpc_frontend`; the HTTP/WS
server exists only in the standalone `nimbus_verified_proxy` binary, and its
symbols are absent from the archive we link. So this module provides one.

Off by default — a module should not open a listening socket unless asked:

```json
{ "httpServer": { "enabled": true, "host": "127.0.0.1", "port": 8545 } }
```

`localEndpoint()` returns the URL (or `""`), and `status().httpServer` reports
it. Every request is forwarded through the **same** verified `proxyCall` path
the typed methods use — one verification path, one error shape.

```bash
curl -s -X POST -H 'content-type: application/json'   --data '{"jsonrpc":"2.0","id":1,"method":"eth_blockNumber","params":[]}'   http://127.0.0.1:8545
```

Point ethers, viem, cast or `eth_rpc_module`'s `ChainConfig.endpoint` at that
URL and their reads become light-client-verified without any of them knowing
this module exists.

Two adaptations make that actually true, rather than nearly true:

* **`eth_call`, `eth_estimateGas` and `eth_createAccessList` (and their `op_`
  twins) take a third positional parameter upstream**, `optimisticStateFetch`,
  which the JSON-RPC spec does not have. Every stock client sends two and the
  library answers `parameters missing`. The endpoint appends the default, and
  leaves an explicitly-supplied third parameter alone.
* **Bare-number results are rendered as hex quantities.** Upstream's encoding is
  not uniform: `eth_chainId` and `eth_gasPrice` answer hex strings but
  `eth_blockNumber` answers a JSON number, which no client expects. Confined to
  this layer — `rpc()` and the typed methods still return exactly what the
  library produced.

Supported: batches, notifications (dispatched, no response), and the reserved
error codes — `-32700` parse, `-32600` invalid request, `-32601` method not
found, `-32602` invalid params, `-32000` verification/backend failure.

**It binds loopback by default and refuses anything but POST.** This endpoint
answers *state* queries, so exposing it beyond `127.0.0.1` is a deliberate act.
There is no authentication: treat a non-loopback bind as publishing an open RPC
node.

## Configuration

Required: `trustedBlockRoot` (`0x` + 64 hex), `executionApiUrls`,
`beaconApiUrls`. `network` is one of `mainnet`, `sepolia`, `hoodi`. OP-Stack L2
is enabled by setting `opExecutionApiUrls` (there is no `op-*` network name in
the library's JSON config — that is a CLI-only option on the standalone binary).

Module-side knobs: `callTimeoutMs` (30000), `startTimeoutMs` (120000),
`drainTimeoutMs` (2000 — a polling bound, see below), `pumpIntervalMs` (50),
`maxInFlight` (64), `httpServer` (see above),
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

## The keep-alive is not optional

`processVerifProxyTasks` only advances chronos while a call is in flight, so an
**idle proxy does not advance its light client at all**. The heartbeat
(`keepAlive: "interval"`, the default) issues `eth_syncing`, which drives the
sync loop and touches no execution backend.

Measured on sepolia over a 5-minute idle:

| | `keepAlive: "off"` | `keepAlive: "continuous"` |
|---|---|---|
| head at start | 11532988 | 11532988 |
| head after 5 min idle | **11532949** — *39 blocks backwards* | 11533012 (+24, tracking) |
| latency of that call | 3186 ms | 0 ms |
| light-client headers tracked | 3 | 26 |

`"off"` does not merely go stale: the reported head **regresses**, so a consumer
polling block numbers sees time run backwards. Treat it as a diagnostic
setting, not a deployment option.

## Pump behaviour, measured

`status().pump` reports a histogram of how long `processVerifProxyTasks` blocks,
split by whether a call was in flight. Over 15 minutes on sepolia (21,510
samples, 358 verified calls):

| | idle | busy |
|---|---|---|
| `<1ms` | **99.991%** | 88.8% |
| `<500ms` | — | 98.1% |
| `<2000ms` | — | 99.94% |
| max observed | — | **3253 ms** |

Idle pumps essentially always return instantly, because chronos `poll()` is not
entered when nothing is pending — so `pumpIntervalMs` (50ms) is what actually
paces the idle loop, as intended.

Two consequences worth knowing:

* `drainCommands()` runs immediately before the poll, so **worst-case
  command-queue latency equals worst-case pump duration, ~3.25s**. Bounded, and
  far under a 30s `callTimeoutMs`, but not nothing.
* `drainTimeoutMs` is a **polling** bound: the drain loop checks its deadline
  between pump calls, so `stop()` — and the destructor join — can overshoot it
  by up to one pump duration. Measured `stop()` in that run: 1102 ms.
* Every wait in `ProxyRuntime` is bounded and every expiry names itself:
  `start()`, `stop()` and `call()` return an error carrying the operation, the
  waiting and proxy thread ids and the runtime's state, and the destructor
  aborts with the same report if the proxy thread has not left `threadMain`
  30 s after shutdown was requested. `join()` takes no deadline, so without
  that a wedged proxy thread hangs its caller — a host, or a CI job — with no
  output at all.

Verified reads have a much fatter latency tail than a plain RPC call: the worst
single `eth_blockNumber` in that run took **12.6 s**, against a 30 s default
`callTimeoutMs`. Budget accordingly.

## Known limitations

* **State reads need a provider with a wide proof window.** `eth_getBalance`,
  `eth_getCode` and `eth_getTransactionCount` resolve their proof against the
  light client's *finalized* header, which lags the chain head. Against free
  public sepolia providers this exceeds their `eth_getProof` window and the call
  fails with `distance to target block exceeds maximum proof window`; the
  backend is then marked ineligible for `GetProof` until it recovers. Reads that
  need no proof (`eth_blockNumber`, `eth_chainId`, `eth_gasPrice`,
  `eth_getBlockByNumber`) work fine. Point `archiveUrls` at a provider that
  serves historical proofs.
* **Return encodings are not uniform.** `eth_blockNumber` answers a JSON
  *number*; `eth_chainId` and `eth_gasPrice` answer hex *strings*;
  `eth_getBlockByNumber` and `eth_syncing` answer objects. There is no
  JSON-RPC envelope — the value is returned bare.
* **`logLevel` is validated but inert.** The library logs
  `Logging configuration options not enabled in the current build`, so the level
  is not applied at runtime. It still has to be whitelisted, because an invalid
  value reaches a `quit()` before that point.
* Sync observability is limited: there is no exported getter for the
  finalized/optimistic slot. `status().state == "degraded"` means "up, but
  heartbeats are failing", inferred from their error strings — three
  consecutive failures degrade, one success clears it. `status().head` is
  refreshed by a separate `eth_blockNumber` probe every fifth heartbeat,
  because `eth_syncing` answers a hardcoded `false` and cannot report it.
* `fetchFinalizedRoot()` exists because Basecamp sandboxes `ui_qml` plugins
  away from the network entirely — an `XMLHttpRequest` from a panel is refused
  with *"sandboxed ui_qml modules may not use the network"* — so a UI that
  wants to offer "fetch me a root" has to route it through a core module.
  It is a convenience for getting started, **not** part of the trust model: a
  root taken from the same endpoint you are about to verify against anchors
  nothing. For anything holding real value, obtain the root independently.

## Executable tutorial

`doctests/verified-proxy-runtime.test.yaml` is a tutorial that is also a test.
It packages this commit as an `.lgx`, installs it, starts a `logoscore` daemon,
configures the proxy from a freshly fetched trusted root, bootstraps the light
client against **live Sepolia**, makes verified calls, and compares one against
the untrusted provider so the head lag is visible rather than asserted.

It talks to the real network deliberately. A verified proxy that cannot reach a
beacon node and prove its way to the chain head is not doing the one thing it
exists for, and mocking that away would prove nothing. The cost is that a
network or endpoint outage turns the doc-test red for reasons outside this repo,
which is why it is a separate workflow from CI.

```bash
nix run github:logos-co/logos-doctest -- run doctests/verified-proxy-runtime.test.yaml --verbose
```

## Windows: blocked on the Nim version, not on the cross toolchain

`nix build .#packages.x86_64-windows.libverifproxy` gets a long way and then
fails in the same place every time:

```
vendor/nimbus-eth2/beacon_chain/spec/state_transition_block.nim(1018, 32)
Error: invalid type: 'typeof(SomeBeaconBlockBody)' in this context
[using system Nim: …/x86_64-w64-mingw32-nim-wrapped-2.2.4]
```

Everything that was expected to be hard already works: the mingw cross stdenv,
the `ar` shim that `--app:staticlib` needs, the vendored nat-libs (which branch
on `$(OS)` and silently take the POSIX path under cross), and Nim itself
compiling most of the tree.

The blocker is a **compiler version**. `USE_SYSTEM_NIM=1` substitutes the
nixpkgs Nim for the one nimbus-build-system pins, and the `nixpkgs-windows` pin
inside `logos-nix` carries **2.2.4** for every spelling — `nim`, `nim-2_2` and
`nim-unwrapped-2_2` alike — while nimbus-eth2's beacon-chain sources need
**2.2.10**, which is what the native build gets and compiles cleanly with.

Three things that do NOT fix it, checked so the next person does not repeat them:

- Passing a Nim from another nixpkgs. The build-side compiler in a cross set is
  a *wrapper* (`x86_64-w64-mingw32-nim-wrapper`) that carries the mingw
  toolchain configuration; a plain `nixpkgs#nim` is a native compiler that knows
  nothing about the target.
- Dropping `USE_SYSTEM_NIM=1` so nimbus-build-system builds its own pinned
  2.2.10. It fetches Nim over the network, which the Nix sandbox forbids —
  which is exactly why upstream's own nix build sets the flag.
- Overriding `nim-unwrapped-2_2` from this flake. `wrapNim` is not exposed, so
  the wrapper cannot be rebuilt from an overridden compiler without an overlay
  applied where the package set is constructed.

The fix belongs in `logos-nix`: bump `nixpkgs-windows` far enough to carry Nim
2.2.10, or apply an overlay inside `mkWindowsPkgs`. Doing it here by importing a
second nixpkgs would put two builds of `libstdc++-6.dll` in one directory, which
resolves by filename and would be a worse problem than the one it solves.

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
