#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>
#include <logos_result.h>

/// A JSON-RPC 2.0 endpoint in front of the verified proxy.
///
/// `libverifproxy` deliberately ships NO server: `library/verifproxy.nim`
/// imports `json_rpc_backend` (the client it calls providers with) and the
/// in-process `engine/rpc_frontend`, but never `json_rpc_frontend` — the
/// HTTP/WS server lives only in the standalone `nimbus_verified_proxy` binary,
/// and its symbols are absent from the archive we link. So the endpoint has to
/// be ours.
///
/// This is a thin adapter, not a second implementation: every request is
/// forwarded to the same `proxyCall` path the typed module methods use, so
/// there is exactly one verification path and one place errors are shaped.
///
/// Binds to loopback by default. This endpoint answers *state* queries, so an
/// accidental 0.0.0.0 bind is a different order of mistake than it would be for
/// a metrics port.
class RpcHttpServer {
public:
    /// Forwards one verified call. Returns the bare result value, exactly as
    /// the library produces it — this class owns the JSON-RPC framing.
    using Dispatch = std::function<StdLogosResult(const std::string& method,
                                                  const nlohmann::json& params)>;

    explicit RpcHttpServer(Dispatch dispatch);
    ~RpcHttpServer();

    RpcHttpServer(const RpcHttpServer&) = delete;
    RpcHttpServer& operator=(const RpcHttpServer&) = delete;

    /// Bind and serve. Returns false with a reason on failure (port in use,
    /// bad host, libmicrohttpd refused).
    bool start(const std::string& host, uint16_t port, std::string& err);
    void stop();

    bool running() const;
    /// e.g. "http://127.0.0.1:8545", or "" when not running.
    std::string endpoint() const;

    /// Handle one request body (one object, or a batch array) and produce the
    /// response body. Exposed for unit tests: it is pure apart from `dispatch`,
    /// so the whole JSON-RPC surface is testable without binding a socket.
    static std::string handleBody(const std::string& body, const Dispatch& dispatch);

    /// Public only because libmicrohttpd's callbacks are free functions and
    /// receive this as their `cls`. Defined in the .cpp; opaque to callers.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};
