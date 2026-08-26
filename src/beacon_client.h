#pragma once

#include <string>

// A single-purpose HTTP client for one job: asking a beacon node for its
// current finalized block root, so an operator has a trusted-root candidate to
// paste into configure().
//
// This lives in the module rather than in the UI because Basecamp sandboxes
// ui_qml plugins away from the network — a core module is the only component
// permitted to make the request. See fetchFinalizedRoot() in the impl header
// for why this is a convenience and not part of the trust model.
namespace beacon_client {

// Strip surrounding whitespace.
std::string trim(const std::string& s);

// True when `url` is http:// or https://. Deliberately stricter than the
// config's own URL check, which also admits ws:// and wss:// — those are valid
// beacon transports for the library but cannot serve a REST GET.
bool isHttpUrl(const std::string& url);

// Join `base` with the finalized-headers path, tolerating a trailing slash.
// Pure and side-effect free so the URL shape is unit-testable without a server.
std::string finalizedHeaderUrl(const std::string& base);

struct HttpResponse {
    long        status = 0;   // 0 when the request never completed
    std::string body;
    std::string error;        // transport-level failure, empty on success
};

// Blocking GET. Called from the module's own dispatch thread, never from the
// proxy thread — a stalled beacon node must not be able to hold up the pump.
HttpResponse httpGet(const std::string& url, long timeoutSeconds = 15);

// Pull `data.root` out of a beacon headers response. Returns "" when the
// payload is not the expected shape.
std::string parseFinalizedRoot(const std::string& body, std::string& slotOut);

}  // namespace beacon_client
