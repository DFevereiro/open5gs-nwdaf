#pragma once
#include "nwdaf_config.hpp"
#include <map>
#include <string>
#include <utility>
#include <vector>

// H1.8: outbound SBI requests (NRF, Rel-18 notifications).
//
// With NWDAF_USE_HTTP2 the request is sent over HTTP/2 (TS 29.500 V18.10.0
// §5.2): h2c with prior knowledge for http:// URIs, h2 negotiated with ALPN
// for https:// URIs — the forms the Open5GS SBI accepts (its NRF rejects
// HTTP/1.1). Without it the request falls back to HTTP/1.1, and the build is
// the explicitly transport non-compliant dev-legacy profile.
//
// For https:// URIs the server certificate is verified against tls_ca_file
// (or the system trust store), and this NF's certificate is presented when
// tls_enabled is set.
struct NwdafHttpResponse {
    long        status = 0;        // 0 when no HTTP response was received
    std::string body;
    std::map<std::string, std::string> headers;   // lower-case names
    int         http_version = 0;  // 1 or 2
    std::string error;             // transport error, when status == 0

    explicit operator bool() const { return status != 0; }
    std::string header(const std::string& lower_name) const {
        auto it = headers.find(lower_name);
        return it == headers.end() ? std::string() : it->second;
    }
};

class NwdafHttpClient {
public:
    explicit NwdafHttpClient(const NwdafConfig& config) : config_(config) {}

    NwdafHttpResponse request(const std::string& method,
                              const std::string& url,
                              const std::string& body = "",
                              const std::string& content_type = "",
                              const std::vector<std::pair<std::string, std::string>>& headers = {},
                              int timeout_seconds = 5) const;

    // True when requests go over HTTP/2 (the rel18-sbi transport profile).
    static bool http2();

private:
    NwdafConfig config_;
};
