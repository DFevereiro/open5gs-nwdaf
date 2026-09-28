#pragma once
#ifdef NWDAF_USE_HTTP2

#include "nwdaf_sbi.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// H1.8: HTTP/2 server for the 3GPP SBI (TS 29.500 V18.10.0 §5.2).
//
// Serves h2c with prior knowledge or, with TLS, h2 negotiated by ALPN
// (mutual TLS when a client CA is given). HTTP/1.1 is not accepted on this
// listener. One thread per connection: SBI peers are few, long-lived NF
// connections. The handler is transport-neutral, so the same server backs the
// NWDAF's 3GPP interfaces and the HTTP/2 peers the tests need.
class NwdafH2Server {
public:
    using Handler = std::function<SbiResponse(const SbiRequest&, const std::string& remote_addr)>;

    struct Options {
        std::string bind_address = "127.0.0.1";
        int         port = 0;          // 0 = pick a free port (tests)
        bool        tls = false;       // requires NWDAF_USE_TLS
        std::string cert_file, key_file;
        std::string ca_file;           // non-empty = require client certificates
    };

    NwdafH2Server(Options options, Handler handler);
    ~NwdafH2Server();
    NwdafH2Server(const NwdafH2Server&) = delete;
    NwdafH2Server& operator=(const NwdafH2Server&) = delete;

    // Bind and start accepting. Throws std::runtime_error on failure.
    void start();
    void stop();
    int  port() const { return port_; }

private:
    void acceptLoop();
    void serve(int fd, std::string remote);

    Options           options_;
    Handler           handler_;
    int               listen_fd_ = -1;
    int               port_ = 0;
    std::atomic<bool> running_{false};
    std::thread       accept_thread_;
    std::mutex        conn_mutex_;
    std::vector<std::thread> conn_threads_;
    void*             ssl_ctx_ = nullptr;   // SSL_CTX* when TLS is enabled
};

#endif  // NWDAF_USE_HTTP2
