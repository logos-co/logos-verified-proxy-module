#include "rpc_http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include <microhttpd.h>

using json = nlohmann::json;

namespace {

// JSON-RPC 2.0 reserved codes.
constexpr int kParseError     = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kInvalidParams  = -32602;
constexpr int kInternalError  = -32603;
// Application range, for a verification/backend failure. -32000 is the
// conventional "server error" slot and is what other Ethereum endpoints use.
constexpr int kServerError    = -32000;

/// Methods whose upstream signature carries ONE MORE positional parameter than
/// the JSON-RPC spec: `optimisticStateFetch`.
///
/// This is the single adaptation without which no standard client works.
/// ethers, viem, cast and `eth_rpc_module` all send `eth_call` with two params;
/// the library requires three and answers "parameters missing" otherwise. We
/// append the default rather than reject, so a stock client Just Works while a
/// caller who knows about the flag can still pass it explicitly.
bool takesOptimisticStateFetch(const std::string& m) {
    static const std::set<std::string> v{
        "eth_call", "eth_estimateGas", "eth_createAccessList",
        "op_call",  "op_estimateGas",  "op_createAccessList",
    };
    return v.count(m) != 0;
}

/// Render a JSON-RPC QUANTITY.
///
/// Upstream's encoding is not uniform: `eth_chainId` and `eth_gasPrice` answer
/// hex strings, but `eth_blockNumber` answers a bare JSON number — which no
/// client expects, because the spec says every QUANTITY is a hex string. Since
/// no standard eth_/op_ method legitimately returns a bare number, promoting
/// one to hex here is unambiguous and makes the endpoint spec-conformant.
///
/// Deliberately confined to this layer. The module's own `rpc()` and typed
/// methods keep returning exactly what the library produced — documented as
/// such — so this translation cannot hide an upstream change from a caller who
/// is talking to the module directly rather than over HTTP.
json normalizeResult(json v) {
    if (v.is_number_unsigned()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%llx",
                      static_cast<unsigned long long>(v.get<uint64_t>()));
        return json(buf);
    }
    return v;
}

json errorObject(int code, const std::string& message) {
    return json{ { "code", code }, { "message", message } };
}

json responseEnvelope(const json& id, json result) {
    return json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move(result) } };
}

json errorEnvelope(const json& id, int code, const std::string& message) {
    return json{ { "jsonrpc", "2.0" }, { "id", id }, { "error", errorObject(code, message) } };
}

/// One request object -> one response object, or nothing for a notification.
/// Returns false when the request was a notification (no `id`).
bool handleOne(const json& req, const RpcHttpServer::Dispatch& dispatch, json& out) {
    const bool isNotification = !req.is_object() || !req.contains("id") || req["id"].is_null();
    const json id = (req.is_object() && req.contains("id")) ? req["id"] : json(nullptr);

    auto fail = [&](int code, const std::string& msg) {
        if (isNotification) return false;
        out = errorEnvelope(id, code, msg);
        return true;
    };

    if (!req.is_object())
        return fail(kInvalidRequest, "request must be a JSON object");
    if (!req.contains("method") || !req["method"].is_string())
        return fail(kInvalidRequest, "'method' is required and must be a string");

    const std::string method = req["method"].get<std::string>();

    json params = json::array();
    if (req.contains("params") && !req["params"].is_null()) {
        if (!req["params"].is_array())
            // The proxy's own dispatcher does parseJson(params).getElems, which
            // yields an empty list for a non-array and then reports the
            // unhelpful "parameters missing". Say what is actually wrong.
            return fail(kInvalidParams, "'params' must be an array (named parameters are not supported)");
        params = req["params"];
    }

    if (takesOptimisticStateFetch(method) && params.size() == 2)
        params.push_back(false);

    const StdLogosResult r = dispatch(method, params);
    if (isNotification) return false;

    if (!r.success) {
        // "unknown method" is the library's own wording for an unrecognised
        // name; map it to the code clients special-case.
        const bool unknown = r.error.find("unknown method") != std::string::npos;
        out = errorEnvelope(id, unknown ? -32601 : kServerError, r.error);
        return true;
    }
    out = responseEnvelope(id, normalizeResult(r.value));
    return true;
}

} // namespace

std::string RpcHttpServer::handleBody(const std::string& body, const Dispatch& dispatch) {
    json req;
    try {
        req = json::parse(body);
    } catch (const std::exception& e) {
        return errorEnvelope(json(nullptr), kParseError, std::string("invalid JSON: ") + e.what()).dump();
    }

    if (req.is_array()) {
        if (req.empty())
            return errorEnvelope(json(nullptr), kInvalidRequest, "empty batch").dump();
        json out = json::array();
        for (const auto& one : req) {
            json resp;
            if (handleOne(one, dispatch, resp)) out.push_back(std::move(resp));
        }
        // A batch of nothing but notifications gets no response body at all.
        return out.empty() ? std::string() : out.dump();
    }

    json resp;
    if (!handleOne(req, dispatch, resp)) return std::string();
    return resp.dump();
}

// ---------------------------------------------------------------------------

struct RpcHttpServer::Impl {
    Dispatch dispatch;
    MHD_Daemon* daemon = nullptr;
    std::string host;
    uint16_t port = 0;
    mutable std::mutex mu;

    explicit Impl(Dispatch d) : dispatch(std::move(d)) {}
};

namespace {

/// Per-connection body accumulator. MHD delivers a POST body in chunks, so the
/// first callback only establishes the connection.
struct ConnState { std::string body; };

MHD_Result sendResponse(MHD_Connection* c, unsigned status, const std::string& payload) {
    MHD_Response* r = MHD_create_response_from_buffer(
        payload.size(), const_cast<char*>(payload.data()), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(r, "Content-Type", "application/json");
    const MHD_Result ret = MHD_queue_response(c, status, r);
    MHD_destroy_response(r);
    return ret;
}

MHD_Result onRequest(void* cls, MHD_Connection* connection, const char* /*url*/,
                     const char* method, const char* /*version*/,
                     const char* upload_data, size_t* upload_data_size,
                     void** con_cls) {
    auto* impl = static_cast<RpcHttpServer::Impl*>(cls);

    if (std::strcmp(method, "POST") != 0) {
        const std::string body =
            errorEnvelope(json(nullptr), kInvalidRequest, "JSON-RPC requires POST").dump();
        return sendResponse(connection, MHD_HTTP_METHOD_NOT_ALLOWED, body);
    }

    if (*con_cls == nullptr) {          // first call: no data yet
        *con_cls = new ConnState();
        return MHD_YES;
    }

    auto* state = static_cast<ConnState*>(*con_cls);
    if (*upload_data_size != 0) {       // a chunk of body
        state->body.append(upload_data, *upload_data_size);
        *upload_data_size = 0;
        return MHD_YES;
    }

    // Body complete. This blocks until the verified call returns — MHD's
    // internal thread pool is what makes that acceptable, and ProxyRuntime's
    // maxInFlight is what bounds it.
    std::string out;
    try {
        out = RpcHttpServer::handleBody(state->body, impl->dispatch);
    } catch (const std::exception& e) {
        out = errorEnvelope(json(nullptr), kInternalError, e.what()).dump();
    } catch (...) {
        out = errorEnvelope(json(nullptr), kInternalError, "unknown error").dump();
    }

    // Notifications produce no body; 204 is the honest status for that.
    const unsigned status = out.empty() ? MHD_HTTP_NO_CONTENT : MHD_HTTP_OK;
    return sendResponse(connection, status, out);
}

void onRequestCompleted(void* /*cls*/, MHD_Connection* /*c*/, void** con_cls,
                        enum MHD_RequestTerminationCode /*toe*/) {
    delete static_cast<ConnState*>(*con_cls);
    *con_cls = nullptr;
}

} // namespace

RpcHttpServer::RpcHttpServer(Dispatch dispatch)
    : m_impl(std::make_unique<Impl>(std::move(dispatch))) {}

RpcHttpServer::~RpcHttpServer() { stop(); }

bool RpcHttpServer::start(const std::string& host, uint16_t port, std::string& err) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (m_impl->daemon) { err = "http server already running"; return false; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        err = "httpServer.host must be a literal IPv4 address (got '" + host + "')";
        return false;
    }

    // MHD_OPTION_SOCK_ADDR rather than letting MHD pick: without it the daemon
    // binds every interface, which for an endpoint serving chain state is a
    // different class of mistake than it is for a metrics port.
    MHD_Daemon* d = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION,
        port, nullptr, nullptr,
        &onRequest, m_impl.get(),
        MHD_OPTION_SOCK_ADDR, reinterpret_cast<sockaddr*>(&addr),
        MHD_OPTION_NOTIFY_COMPLETED, &onRequestCompleted, nullptr,
        MHD_OPTION_END);
    if (!d) {
        err = "failed to bind " + host + ":" + std::to_string(port)
            + " (port in use, or not permitted)";
        return false;
    }

    m_impl->daemon = d;
    m_impl->host = host;
    m_impl->port = port;
    return true;
}

void RpcHttpServer::stop() {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (!m_impl->daemon) return;
    MHD_stop_daemon(m_impl->daemon);   // joins its threads; in-flight requests finish
    m_impl->daemon = nullptr;
    m_impl->port = 0;
}

bool RpcHttpServer::running() const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->daemon != nullptr;
}

std::string RpcHttpServer::endpoint() const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (!m_impl->daemon) return {};
    return "http://" + m_impl->host + ":" + std::to_string(m_impl->port);
}
