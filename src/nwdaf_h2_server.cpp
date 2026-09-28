#include "nwdaf_h2_server.hpp"

#include <nghttp2/nghttp2.h>
#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>

#ifdef NWDAF_USE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace {

// ── Connection I/O over a socket or a TLS session ───────────────────────────

struct Conn {
    int fd = -1;
#ifdef NWDAF_USE_TLS
    SSL* ssl = nullptr;
#endif

    ssize_t read(uint8_t* buf, size_t n) {
#ifdef NWDAF_USE_TLS
        if (ssl) return SSL_read(ssl, buf, static_cast<int>(n));
#endif
        return ::recv(fd, buf, n, 0);
    }
    bool writeAll(const uint8_t* data, size_t n) {
        while (n > 0) {
            ssize_t w;
#ifdef NWDAF_USE_TLS
            if (ssl) w = SSL_write(ssl, data, static_cast<int>(n));
            else
#endif
            w = ::send(fd, data, n, MSG_NOSIGNAL);
            if (w <= 0) return false;
            data += w;
            n -= static_cast<size_t>(w);
        }
        return true;
    }
    bool pending() const {
#ifdef NWDAF_USE_TLS
        return ssl && SSL_pending(ssl) > 0;
#else
        return false;
#endif
    }
};

// ── Per-connection HTTP/2 state ─────────────────────────────────────────────

struct Stream {
    std::string method, path, authority;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string response;   // body being sent
    size_t      sent = 0;
};

struct Session {
    const NwdafH2Server::Handler* handler;
    std::string remote;
    bool tls;
    std::map<int32_t, std::unique_ptr<Stream>> streams;
};

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// RFC 3986 percent-decoding ('+' is a literal character in a URI).
std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexValue(s[i + 1]), lo = hexValue(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

ssize_t readResponse(nghttp2_session*, int32_t, uint8_t* buf, size_t length,
                     uint32_t* data_flags, nghttp2_data_source* source, void*) {
    auto* st = static_cast<Stream*>(source->ptr);
    const size_t n = std::min(length, st->response.size() - st->sent);
    std::memcpy(buf, st->response.data() + st->sent, n);
    st->sent += n;
    if (st->sent == st->response.size()) *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    return static_cast<ssize_t>(n);
}

void respond(nghttp2_session* session, int32_t stream_id, Session& sess, Stream& st) {
    SbiRequest req;
    req.method = st.method;
    const auto q = st.path.find('?');
    req.path = percentDecode(st.path.substr(0, q));
    if (q != std::string::npos) {
        const std::string query = st.path.substr(q + 1);
        size_t pos = 0;
        while (pos <= query.size()) {
            const size_t amp = std::min(query.find('&', pos), query.size());
            const std::string pair = query.substr(pos, amp - pos);
            if (!pair.empty()) {
                const auto eq = pair.find('=');
                req.query.emplace(percentDecode(pair.substr(0, eq)),
                                  eq == std::string::npos ? "" : percentDecode(pair.substr(eq + 1)));
            }
            pos = amp + 1;
        }
    }
    req.headers  = st.headers;
    req.body     = st.body;
    req.api_root = std::string(sess.tls ? "https" : "http") + "://" + st.authority;

    SbiResponse res;
    try {
        res = (*sess.handler)(req, sess.remote);
    } catch (const std::exception& e) {
        spdlog::error("HTTP/2: handler failed for {} {}: {}", req.method, req.path, e.what());
        res = {500, "", "", {}};
    }

    // HTTP/2 header names are lower-case (RFC 9113 §8.2).
    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back(":status", std::to_string(res.status));
    if (!res.content_type.empty()) fields.emplace_back("content-type", res.content_type);
    for (const auto& [k, v] : res.headers) {
        std::string name = k;
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        fields.emplace_back(name, v);
    }
    std::vector<nghttp2_nv> nva;
    for (auto& [k, v] : fields)
        nva.push_back({reinterpret_cast<uint8_t*>(k.data()), reinterpret_cast<uint8_t*>(v.data()),
                       k.size(), v.size(), NGHTTP2_NV_FLAG_NONE});

    st.response = std::move(res.body);
    nghttp2_data_provider provider;
    provider.source.ptr = &st;
    provider.read_callback = readResponse;
    nghttp2_submit_response(session, stream_id, nva.data(), nva.size(),
                            st.response.empty() ? nullptr : &provider);
}

int onBeginHeaders(nghttp2_session*, const nghttp2_frame* frame, void* user) {
    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST)
        static_cast<Session*>(user)->streams[frame->hd.stream_id] = std::make_unique<Stream>();
    return 0;
}

int onHeader(nghttp2_session*, const nghttp2_frame* frame, const uint8_t* name, size_t namelen,
             const uint8_t* value, size_t valuelen, uint8_t, void* user) {
    auto& streams = static_cast<Session*>(user)->streams;
    auto it = streams.find(frame->hd.stream_id);
    if (it == streams.end()) return 0;
    const std::string n(reinterpret_cast<const char*>(name), namelen);
    const std::string v(reinterpret_cast<const char*>(value), valuelen);
    Stream& st = *it->second;
    if (n == ":method")         st.method = v;
    else if (n == ":path")      st.path = v;
    else if (n == ":authority") st.authority = v;
    else if (n[0] != ':')       st.headers[n] = v;
    return 0;
}

int onDataChunk(nghttp2_session*, uint8_t, int32_t stream_id, const uint8_t* data, size_t len,
                void* user) {
    auto& streams = static_cast<Session*>(user)->streams;
    auto it = streams.find(stream_id);
    if (it != streams.end()) it->second->body.append(reinterpret_cast<const char*>(data), len);
    return 0;
}

int onFrameRecv(nghttp2_session* session, const nghttp2_frame* frame, void* user) {
    if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        auto* sess = static_cast<Session*>(user);
        auto it = sess->streams.find(frame->hd.stream_id);
        if (it != sess->streams.end()) respond(session, frame->hd.stream_id, *sess, *it->second);
    }
    return 0;
}

int onStreamClose(nghttp2_session*, int32_t stream_id, uint32_t, void* user) {
    static_cast<Session*>(user)->streams.erase(stream_id);
    return 0;
}

#ifdef NWDAF_USE_TLS
// ALPN: this listener speaks HTTP/2 only.
int selectH2(SSL*, const unsigned char** out, unsigned char* outlen,
             const unsigned char* in, unsigned int inlen, void*) {
    for (unsigned int i = 0; i < inlen; i += 1u + in[i]) {
        if (in[i] == 2 && i + 3 <= inlen && std::memcmp(in + i + 1, "h2", 2) == 0) {
            *out = in + i + 1;
            *outlen = 2;
            return SSL_TLSEXT_ERR_OK;
        }
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}
#endif

}  // namespace

// ── NwdafH2Server ───────────────────────────────────────────────────────────

NwdafH2Server::NwdafH2Server(Options options, Handler handler)
    : options_(std::move(options)), handler_(std::move(handler)) {}

NwdafH2Server::~NwdafH2Server() {
    stop();
#ifdef NWDAF_USE_TLS
    if (ssl_ctx_) SSL_CTX_free(static_cast<SSL_CTX*>(ssl_ctx_));
#endif
}

void NwdafH2Server::start() {
    if (options_.tls) {
#ifdef NWDAF_USE_TLS
        SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        ssl_ctx_ = ctx;
        if (SSL_CTX_use_certificate_chain_file(ctx, options_.cert_file.c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx, options_.key_file.c_str(), SSL_FILETYPE_PEM) != 1)
            throw std::runtime_error("HTTP/2 TLS: cannot load certificate or key");
        if (!options_.ca_file.empty()) {
            if (SSL_CTX_load_verify_locations(ctx, options_.ca_file.c_str(), nullptr) != 1)
                throw std::runtime_error("HTTP/2 TLS: cannot load client CA " + options_.ca_file);
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        }
        SSL_CTX_set_alpn_select_cb(ctx, selectH2, nullptr);
#else
        throw std::runtime_error("HTTP/2 over TLS requires a build with NWDAF_USE_TLS");
#endif
    }

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;
    addrinfo* res = nullptr;
    if (getaddrinfo(options_.bind_address.c_str(), std::to_string(options_.port).c_str(),
                    &hints, &res) != 0 || !res)
        throw std::runtime_error("HTTP/2: cannot resolve " + options_.bind_address);
    listen_fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    const int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    const bool ok = listen_fd_ >= 0 &&
                    ::bind(listen_fd_, res->ai_addr, res->ai_addrlen) == 0 &&
                    ::listen(listen_fd_, 64) == 0;
    freeaddrinfo(res);
    if (!ok) {
        if (listen_fd_ >= 0) ::close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("HTTP/2: cannot listen on " + options_.bind_address + ":" +
                                 std::to_string(options_.port));
    }
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.ss_family == AF_INET6
                      ? reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port
                      : reinterpret_cast<sockaddr_in*>(&addr)->sin_port);

    running_ = true;
    accept_thread_ = std::thread(&NwdafH2Server::acceptLoop, this);
    spdlog::info("HTTP/2 SBI listening on {}:{} ({})", options_.bind_address, port_,
                 options_.tls ? (options_.ca_file.empty() ? "h2/TLS" : "h2/mTLS") : "h2c");
}

void NwdafH2Server::stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
    std::lock_guard<std::mutex> lk(conn_mutex_);
    for (auto& t : conn_threads_) if (t.joinable()) t.join();
    conn_threads_.clear();
}

void NwdafH2Server::acceptLoop() {
    while (running_) {
        pollfd pfd{listen_fd_, POLLIN, 0};
        if (::poll(&pfd, 1, 200) <= 0) continue;
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd < 0) continue;
        char host[INET6_ADDRSTRLEN] = {};
        if (addr.ss_family == AF_INET6)
            inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&addr)->sin6_addr, host, sizeof(host));
        else
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&addr)->sin_addr, host, sizeof(host));
        std::lock_guard<std::mutex> lk(conn_mutex_);
        conn_threads_.emplace_back(&NwdafH2Server::serve, this, fd, std::string(host));
    }
}

void NwdafH2Server::serve(int fd, std::string remote) {
    Conn conn;
    conn.fd = fd;
#ifdef NWDAF_USE_TLS
    if (options_.tls) {
        conn.ssl = SSL_new(static_cast<SSL_CTX*>(ssl_ctx_));
        SSL_set_fd(conn.ssl, fd);
        const unsigned char* proto = nullptr;
        unsigned int proto_len = 0;
        if (SSL_accept(conn.ssl) == 1)
            SSL_get0_alpn_selected(conn.ssl, &proto, &proto_len);
        if (proto_len != 2 || std::memcmp(proto, "h2", 2) != 0) {
            spdlog::debug("HTTP/2: TLS handshake or ALPN h2 failed from {}", remote);
            SSL_free(conn.ssl);
            ::close(fd);
            return;
        }
    }
#endif

    Session sess{&handler_, remote, options_.tls, {}};
    nghttp2_session_callbacks* cbs = nullptr;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, onBeginHeaders);
    nghttp2_session_callbacks_set_on_header_callback(cbs, onHeader);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, onDataChunk);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, onFrameRecv);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, onStreamClose);
    nghttp2_session* session = nullptr;
    nghttp2_session_server_new(&session, cbs, &sess);
    nghttp2_session_callbacks_del(cbs);

    nghttp2_settings_entry settings[] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, settings, 1);

    uint8_t buf[16384];
    while (running_) {
        const uint8_t* out = nullptr;
        ssize_t n;
        bool write_ok = true;
        while ((n = nghttp2_session_mem_send(session, &out)) > 0)
            if (!conn.writeAll(out, static_cast<size_t>(n))) { write_ok = false; break; }
        if (!write_ok || n < 0) break;
        if (!nghttp2_session_want_read(session) && !nghttp2_session_want_write(session)) break;

        if (!conn.pending()) {
            pollfd pfd{fd, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, 200);
            if (pr == 0) continue;
            if (pr < 0) break;
        }
        const ssize_t r = conn.read(buf, sizeof(buf));
        if (r <= 0) break;
        if (nghttp2_session_mem_recv(session, buf, static_cast<size_t>(r)) < 0) break;
    }

    nghttp2_session_del(session);
#ifdef NWDAF_USE_TLS
    if (conn.ssl) {
        SSL_shutdown(conn.ssl);
        SSL_free(conn.ssl);
    }
#endif
    ::close(fd);
}
