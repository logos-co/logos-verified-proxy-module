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
error, within `queueTimeoutMs + callTimeoutMs`. Capacity waits in a FIFO queue;
`callTimeoutMs` starts only after admission. Consumers that want concurrency use
the generated `<method>Async` twin on their side; the module is
`concurrency: "multi"`, so blocked callers do not stall each other. Its generated
host glue uses four reusable workers (`max_workers: 4`); additional calls queue
instead of creating an unbounded number of Qt threads and event-dispatcher
pipes.

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
the typed methods use — one verification path, one error shape. The listener's
connection count is capped at the runtime's effective in-flight limit, so its
thread-per-connection mode cannot consume descriptors without bound.

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

Module-side knobs: `callTimeoutMs` (30000), `queueTimeoutMs` (30000),
`startTimeoutMs` (120000), `drainTimeoutMs` (2000 — a polling bound, see below),
`pumpIntervalMs` (50), `maxInFlight` (64), `httpServer` (see above),
`keepAlive` (`off` | `interval` | `continuous`), `keepAliveIntervalMs` (12000,
floored at one beacon slot — see below), `autoStart` (false). Upstream tuning
lives under `tuning`.

`maxInFlight` is an operator ceiling, not a promise that 64 calls can run at
once. Calls above the active ceiling wait in FIFO order for up to
`queueTimeoutMs`; their separate `callTimeoutMs` begins only when they are
admitted. On macOS and Linux the runtime reads the process' live
`RLIMIT_NOFILE`, counts descriptors already used by the host, reserves 25%
(between 16 and 128 descriptors) for Qt, logs, DNS/TLS and lifecycle work, and
derives a lower effective ceiling when necessary. Each admission rechecks the
live count because Nimbus/Chronos may open a fresh HTTP connection for a call
and one verified call can fan out to `tuning.parallelBlockDownloads` requests.
Windows retains the configured ceiling because it has no equivalent
per-process socket limit to query.

The current calculation and queue depth are visible under `status().resources`, including
`openDescriptors`, `softDescriptorLimit`, `descriptorReserve`,
`estimatedDescriptorsPerCall`, `effectiveMaxInFlight`, `queuedCalls` and
`queueTimeoutMs`. A call is rejected only when its queue deadline expires (or
the proxy stops), with the last blocker included in the error. Descriptor-caused
expiries retain the actionable `OS file-descriptor budget is exhausted` detail;
they do not take the module process down. Internal heartbeat/head probes never
wait and yield to queued user calls, because they run on the thread that frees
capacity.

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

## The module drives the sync

Since [status-im/nimbus-eth1#4828](https://github.com/status-im/nimbus-eth1/pull/4828)
the library never syncs on its own, and requests no longer trigger a sync: every
call fails with `light client doesn't know the current and next sync
committees, sync first` until the host has called `nvp_eth_sync`. So the pump
runs a sync cycle once per `nvp_eth_syncInterval()` (one beacon slot):
`nvp_eth_sync`, then `nvp_op_sync` when `opExecutionApiUrls` is set, since the
OP anchor reads L1 headers.

* `start()` returns only after the first cycle succeeds. A failing first sync is
  retried every 2 s until `startTimeoutMs`, and the timeout error carries the
  last sync error.
* Three consecutive failed cycles degrade the proxy; one good cycle clears that.
* `status().sync` reports `intervalMs`, `lastSyncedAt`, `failures`,
  `consecutiveFailures` and `lastError`.

The sync runs whatever `keepAlive` says, so `"off"` no longer lets an idle
proxy's head go stale or run backwards, as it did before #4828.

## The heartbeat

`keepAlive: "interval"` (the default) issues `eth_getBlockByNumber("latest",
false)` once per beat, after the first sync. `keepAliveIntervalMs` is **raised
to one beacon slot (12 s) if it is set lower**, and the raised value is what
`getConfig()` reports back: the verified head moves at most once per slot, so a
faster beat only adds execution requests. Three consecutive beat failures take
three slots (~36 s) to degrade the proxy.

### What the beat is, and why it is that call

`eth_getBlockByNumber("latest", false)`, on the library author's
recommendation. It is one call per beat and it earns the round trip three times
over:

* It exercises the **whole** path a user call takes — the synced check, header
  store, an execution backend, and verifying the block against the verified
  header. The `eth_syncing` it replaces never reached an execution backend, so
  two minutes of `No eligible backend for capability` could pass with
  `status()` still reporting `running` while every user call failed. That is
  what happened in [#11].
* It answers with the head, so the separate `eth_blockNumber` probe is gone.
* `selectBackend()` decays negative scores toward 0 only when it is *called*,
  so a beat that reaches an execution backend is also what lets a penalised
  backend recover while the module is otherwise idle.

The cost is one execution request per slot, and a failing beat now penalises
the backend through `penaltyOr` — which is the point rather than a side effect.

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
  admission, `start()`, `stop()` and `call()` return an error carrying the
  limiting operation or resource (with thread/runtime state on lifecycle and
  dispatched-call timeouts), and the destructor
  aborts with the same report if the proxy thread has not left `threadMain`
  30 s after shutdown was requested. `join()` takes no deadline, so without
  that a wedged proxy thread hangs its caller — a host, or a CI job — with no
  output at all.

Verified reads have a much fatter latency tail than a plain RPC call: the worst
single `eth_blockNumber` in that run took **12.6 s**, against a 30 s default
`callTimeoutMs`. A saturated caller can first spend up to the separate 30 s
`queueTimeoutMs`; budget the combined deadline accordingly.

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
  sync cycles or heartbeats are failing" — three consecutive failures of
  either degrade, one success clears it. `status().head` comes
  out of the beat itself — `eth_getBlockByNumber("latest")` answers with the
  block it just verified.
* **A fault in the library's heap takes the host process with it.** Everything
  the library allocates lives in a Nim `--mm:refc` heap the module cannot
  inspect, guard or recover from, and it runs in-process. [#11] recorded a
  `SIGSEGV` inside that GC's cycle collector — `markS` under `collectCycles`,
  reached from an ordinary allocation for a beacon REST request — after 90
  minutes of mainnet uptime with a 1 s heartbeat. A crashed module reports
  `The Verified Proxy module stopped unexpectedly` and needs a restart. The
  likely cause was the compiler rather than the load, and the library is no
  longer built with it: see [Which Nim builds the library](#which-nim-builds-the-library).
  Reported upstream as [status-im/nimbus-eth1#4813][upstream-gc].

  [#11]: https://github.com/logos-co/logos-verified-proxy-module/issues/11
  [upstream-gc]: https://github.com/status-im/nimbus-eth1/issues/4813
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

## Which Nim builds the library

Nim 2.2.12 — the compiler nimbus-eth1 builds itself with, from the pinned rev's
`vendor/nimbus-build-system` submodule. nimbus-eth1's own `flake.lock` still
locks nimbus-build-system at 2.2.10 — the submodule moved in
[nimbus-eth1#4761][nimbus-4761] and the lock did not follow — so taking the
compiler from that flake builds with 2.2.10. This flake builds it from the
submodule instead, so a nimbus bump that moves Nim moves it here too.

2.2.10 corrupts the refc heap. It resets a case object by zeroing only the
active branch ([nim-lang/Nim#25992][nim-25992]), leaving stale bytes in the rest
of the union for the GC to read as a pointer — and nim-results' `Result` is a
case object. With Nim's GC assertions on (`-d:useGcAssert -d:useSysAssert`) and the
library driven the way this module drives it, against mainnet:

| compiler | outcome |
|---|---|
| 2.2.10 | `[GCASSERT] decRef: interiorPtr` in `rpcCallEvm`, on the first `eth_call` — 7 runs of 7 (x86_64-linux and aarch64-darwin), 14–32 s in |
| 2.2.10 + only the #25992 fix | clean, `eth_call`s included |
| 2.2.12 | clean under the [#11] load: a 1 s beat, a receipt polled for a transaction that never lands, `eth_call` |

[#11]'s log shows `eth_call`'s capability (`CreateAccessList`) in use, so this
is the most likely cause of that crash — though a silent corruption cannot be
tied to one fault after the fact. To see which compiler built an archive:
`strings -a result/lib/libverifproxy.a | grep -om1 'nbs-nim-[0-9.]*'`
(`nim-unwrapped-[0-9.]*` on Windows).

[nimbus-4761]: https://github.com/status-im/nimbus-eth1/pull/4761
[nim-25992]: https://github.com/nim-lang/Nim/issues/25992

## Windows

```bash
nix build .#packages.x86_64-windows.libverifproxy   # the static library
nix build .#packages.x86_64-windows.default         # the module plugin
nix build .#packages.x86_64-windows.unit-tests      # the unit tests, with a test manifest
```

CI runs the unit tests natively on a Windows runner (`.github/workflows/windows.yml`).

Both build on an `x86_64-linux` builder. `libverifproxy` produces a `pe-x86-64`
archive — `$out/lib/libverifproxy.a` with `startVerifProxy`, `stopVerifProxy`,
`processVerifProxyTasks` and `proxyCall` defined — plus
`$out/include/verifproxy.h`. `default` produces
`$out/lib/verified_proxy_module_plugin.dll`, a `pei-x86-64` plugin, with its
dependency DLLs staged beside it (Qt6Core, Qt6Network, Qt6RemoteObjects,
libmicrohttpd-12, libcurl-4, libwinpthread-1, libstdc++-6 …).

Four things had to be settled to get there, and none of them is the cross
toolchain — the mingw stdenv, the `ar` shim `--app:staticlib` needs, and the
vendored nat-libs all work as written above.

**The compiler version.** `USE_SYSTEM_NIM=1` substitutes a nix-built Nim for
the one nimbus-build-system would otherwise fetch over the network, which the
sandbox forbids. In a cross set that compiler has to be the nixpkgs *wrapper*
carrying the mingw toolchain configuration, so the one the other platforms
build from nimbus' submodule cannot be used as-is. nixpkgs' own 2.2.4 is too
old — nimbus-eth2's beacon-chain sources stop at `state_transition_block.nim`
with `invalid type: 'typeof(SomeBeaconBlockBody)'` — so `logos-nix` overlays
Nim 2.2.10 into `mkWindowsPkgs` (`nix/windows/nim-overlay.nix`), and this flake
re-wraps that compiler at the submodule's version with
`nim-2_2.override { nim-unwrapped-2_2 = …; }`. The tarball hash is pinned, so
a nimbus bump that moves Nim fails that fetch until it is updated.

**mcl assumes llvm-mingw.** `vendor/nim-mcl`'s `when defined(windows)` branch
feeds `src/base64.ll` — LLVM IR — to `$CC`. GCC answers `linker input file
unused because linking not done`, and `ar` then fails on the `.o` that was
never produced. Its Linux branch takes `asm/x86-64.S` instead, so this is a
toolchain assumption rather than anything about cross-compiling. Nimbus ships
the pure-Nim bncurve backend for exactly this case, so the Windows build passes
`-d:enable_mcl_lib=false`.

**libmicrohttpd is `platforms.unix` upstream.** The library itself has real
Windows support — the mingw build picks up Winsock and installs
`libmicrohttpd-12.dll` — but its *optional* closure does not: gnutls pulls
unbound → libevent, whose mingw build dies formatting an int64. `logos-nix`'s
Windows cross-overlay widens the platform list and drops gnutls/curl/libgcrypt,
which are optional to an embedded server (curl is the test client and `doCheck`
is already false; HTTPS on a loopback JSON-RPC endpoint is not what terminates
TLS). Because the *plugin's* package set comes from `logos-module-builder`'s
`logos-nix`, not this repo's, that input has to carry the overlay too.

**The endpoint's socket code was POSIX-only.** `rpc_http_server.cpp` filled a
`sockaddr_in` through `<arpa/inet.h>`/`<netinet/in.h>`/`<sys/socket.h>`; on W32
those are `<winsock2.h>` + `<ws2tcpip.h>` (`inet_pton` lives in the latter), and
winsock2 must precede `microhttpd.h`. `windows.pthreads` also joins the build
inputs on Windows — nixpkgs builds mingw-w64 against mcfgthread, so
`-lwinpthread`, which `libverifproxy.a` needs and the plugin names, resolves
nowhere by default.

**Do not name `stdc++` on the Windows link line.** Upstream says `-lc++` for
llvm-mingw, and the obvious translation is wrong: it puts `libstdc++.dll.a`
ahead of `liblogos_protocol.a`, and the import library's definition of a COMDAT
template body then collides with the archive's own —
`multiple definition of 'std::__cxx11::basic_string<...>::_M_erase'`. g++ is the
link driver for a CXX target and appends the C++ runtime last, which is the
position that resolves. Same reason the macOS branch does not name `-lc++`.

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
