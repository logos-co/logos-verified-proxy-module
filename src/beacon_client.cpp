#include "beacon_client.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace beacon_client {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool isHttpUrl(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

std::string finalizedHeaderUrl(const std::string& base) {
    std::string b = base;
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b + "/eth/v1/beacon/headers/finalized";
}

namespace {

size_t appendBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t n = size * nmemb;
    static_cast<std::string*>(userdata)->append(ptr, n);
    return n;
}

// curl_global_init is not thread-safe, and every curl_easy_init after the
// first would otherwise race it. One init per process, on first use.
void ensureGlobalInit() {
    static const bool once = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)once;
}

}  // namespace

HttpResponse httpGet(const std::string& url, long timeoutSeconds) {
    ensureGlobalInit();

    HttpResponse out;
    CURL* curl = curl_easy_init();
    if (!curl) {
        out.error = "could not initialise HTTP client";
        return out;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // A beacon node redirecting us to a different scheme is not a redirect we
    // want to follow silently.
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "logos-verified-proxy-module");

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        out.error = curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
    }
    curl_easy_cleanup(curl);
    return out;
}

std::string parseFinalizedRoot(const std::string& body, std::string& slotOut) {
    slotOut.clear();
    json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) return {};

    const auto data = doc.find("data");
    if (data == doc.end() || !data->is_object()) return {};

    const auto root = data->find("root");
    if (root == data->end() || !root->is_string()) return {};

    // The slot is nested two levels further down and is purely informational,
    // so a missing one is not a failure.
    const auto header = data->find("header");
    if (header != data->end() && header->is_object()) {
        const auto message = header->find("message");
        if (message != header->end() && message->is_object()) {
            const auto slot = message->find("slot");
            if (slot != message->end() && slot->is_string()) slotOut = slot->get<std::string>();
        }
    }
    return root->get<std::string>();
}

}  // namespace beacon_client
