#include "nwdaf_http_client.hpp"
#include <algorithm>
#include <cctype>

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

#ifdef NWDAF_USE_HTTP2
#include <curl/curl.h>
#include <mutex>

namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

bool hasBody(const std::string& method) {
    return method == "POST" || method == "PUT" || method == "PATCH";
}

size_t onBody(char* data, size_t size, size_t n, void* user) {
    static_cast<std::string*>(user)->append(data, size * n);
    return size * n;
}

size_t onHeader(char* data, size_t size, size_t n, void* user) {
    const std::string line(data, size * n);
    const auto colon = line.find(':');
    if (colon != std::string::npos)
        (*static_cast<std::map<std::string, std::string>*>(user))[lower(trim(line.substr(0, colon)))] =
            trim(line.substr(colon + 1));
    return size * n;
}

}  // namespace

bool NwdafHttpClient::http2() { return true; }

NwdafHttpResponse NwdafHttpClient::request(
    const std::string& method, const std::string& url, const std::string& body,
    const std::string& content_type,
    const std::vector<std::pair<std::string, std::string>>& headers,
    int timeout_seconds) const
{
    static std::once_flag init;
    std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    NwdafHttpResponse out;
    CURL* c = curl_easy_init();
    if (!c) { out.error = "curl_easy_init failed"; return out; }

    curl_slist* hdrs = nullptr;
    if (!content_type.empty()) hdrs = curl_slist_append(hdrs, ("Content-Type: " + content_type).c_str());
    for (const auto& [k, v] : headers) hdrs = curl_slist_append(hdrs, (k + ": " + v).c_str());
    hdrs = curl_slist_append(hdrs, "Expect:");   // no 100-continue round trip

    const bool https = url.rfind("https://", 0) == 0;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (hasBody(method)) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION,
                     https ? CURL_HTTP_VERSION_2TLS : CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);
    if (https) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
        if (!config_.tls_ca_file.empty()) curl_easy_setopt(c, CURLOPT_CAINFO, config_.tls_ca_file.c_str());
        if (config_.tls_enabled) {
            curl_easy_setopt(c, CURLOPT_SSLCERT, config_.tls_cert_file.c_str());
            curl_easy_setopt(c, CURLOPT_SSLKEY, config_.tls_key_file.c_str());
        }
    }
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out.body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &out.headers);

    const CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out.status);
        long version = 0;
        curl_easy_getinfo(c, CURLINFO_HTTP_VERSION, &version);
        out.http_version = version == CURL_HTTP_VERSION_2_0 ? 2 : 1;
    } else {
        out.error = curl_easy_strerror(rc);
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return out;
}

#else  // dev-legacy profile: HTTP/1.1
#include <httplib.h>

bool NwdafHttpClient::http2() { return false; }

NwdafHttpResponse NwdafHttpClient::request(
    const std::string& method, const std::string& url, const std::string& body,
    const std::string& content_type,
    const std::vector<std::pair<std::string, std::string>>& headers,
    int timeout_seconds) const
{
    NwdafHttpResponse out;
    const auto scheme_end = url.find("://");
    const auto path_start = scheme_end == std::string::npos ? std::string::npos
                                                            : url.find('/', scheme_end + 3);
    const std::string base = url.substr(0, path_start);
    const std::string path = path_start == std::string::npos ? "/" : url.substr(path_start);
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    if (url.rfind("https://", 0) == 0) {
        out.error = "https requires a build with NWDAF_USE_TLS";
        return out;
    }
    httplib::Client cli(base);
#else
    auto cli = (url.rfind("https://", 0) == 0 && config_.tls_enabled)
        ? httplib::Client(base, config_.tls_cert_file, config_.tls_key_file)
        : httplib::Client(base);
    cli.enable_server_certificate_verification(true);
    if (!config_.tls_ca_file.empty()) cli.set_ca_cert_path(config_.tls_ca_file.c_str());
#endif
    cli.set_connection_timeout(3, 0);
    cli.set_read_timeout(timeout_seconds, 0);
    httplib::Headers h;
    for (const auto& [k, v] : headers) h.emplace(k, v);

    httplib::Result r;
    if (method == "GET")         r = cli.Get(path, h);
    else if (method == "DELETE") r = cli.Delete(path, h);
    else if (method == "POST")   r = cli.Post(path, h, body, content_type);
    else if (method == "PUT")    r = cli.Put(path, h, body, content_type);
    else if (method == "PATCH")  r = cli.Patch(path, h, body, content_type);
    else { out.error = "unsupported method " + method; return out; }

    if (!r) { out.error = httplib::to_string(r.error()); return out; }
    out.status = r->status;
    out.body = r->body;
    for (const auto& [k, v] : r->headers) out.headers[lower(k)] = v;
    out.http_version = 1;
    return out;
}

#endif
