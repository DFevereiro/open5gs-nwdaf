// QOL-01: nwdaf-notify-sink — a notification consumer for testing.
//
// Receives the NWDAF's notifications and prints them: Rel-18
// Nnwdaf_EventsSubscription notifications arrive over HTTP/2 (h2c with prior
// knowledge for http:// URIs, TS 29.500 §5.2), operator-API ones over
// HTTP/1.1 (--http1-port). Every request is answered with --status (default
// 204, the TS 29.520 success response to a notification).
#include "nwdaf_h2_server.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <csignal>
#include <pthread.h>
#include <signal.h>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

struct Options {
    NwdafH2Server::Options h2;
    int  http1_port = 0;
    int  status = 204;
    bool compact = false;
};

std::mutex out_mutex;

std::string nowLocal() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

void print(const Options& o, const std::string& proto, const std::string& method, const std::string& path,
           const std::string& remote, const std::string& callback, const std::string& body) {
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    std::lock_guard<std::mutex> lock(out_mutex);
    if (o.compact) {
        // One JSON line per notification, for jq.
        nlohmann::json line = {{"time", nowLocal()}, {"protocol", proto}, {"method", method}, {"path", path},
                               {"remote", remote}};
        if (!callback.empty()) line["3gpp-Sbi-Callback"] = callback;
        line["body"] = parsed.is_discarded() ? nlohmann::json(body) : parsed;
        std::cout << line.dump() << std::endl;
        return;
    }
    std::cout << "── " << nowLocal() << "  " << proto << "  " << method << " " << path << "  from " << remote;
    if (!callback.empty()) std::cout << "  (3gpp-Sbi-Callback: " << callback << ")";
    std::cout << "\n" << (parsed.is_discarded() ? body : parsed.dump(2)) << "\n" << std::endl;
}

void usage() {
    std::cerr <<
        "usage: nwdaf-notify-sink [options]\n"
        "  --bind ADDR        listen address (default 127.0.0.1)\n"
        "  --port N           HTTP/2 port for Rel-18 notifications (default 9999)\n"
        "  --http1-port N     also listen on HTTP/1.1, for operator-API notifications\n"
        "  --status CODE      response status (default 204)\n"
        "  --compact          one JSON line per notification\n"
#ifdef NWDAF_USE_TLS
        "  --tls CERT KEY     h2 over TLS instead of h2c\n"
        "  --ca FILE          with --tls: require client certificates signed by FILE\n"
#endif
        "Notification URI to give the NWDAF: http://ADDR:PORT/<any path>\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    Options o;
    o.h2.port = 9999;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has = i + 1 < argc;
        if (a == "--bind" && has)            o.h2.bind_address = argv[++i];
        else if (a == "--port" && has)       o.h2.port = std::stoi(argv[++i]);
        else if (a == "--http1-port" && has) o.http1_port = std::stoi(argv[++i]);
        else if (a == "--status" && has)     o.status = std::stoi(argv[++i]);
        else if (a == "--compact")           o.compact = true;
#ifdef NWDAF_USE_TLS
        else if (a == "--tls" && i + 2 < argc) {
            o.h2.tls = true;
            o.h2.cert_file = argv[++i];
            o.h2.key_file = argv[++i];
        } else if (a == "--ca" && has)       o.h2.ca_file = argv[++i];
#endif
        else { usage(); return a == "-h" || a == "--help" ? 0 : 2; }
    }

    // Block the stop signals in every thread; the main thread waits for them.
    sigset_t stop;
    sigemptyset(&stop);
    sigaddset(&stop, SIGINT);
    sigaddset(&stop, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop, nullptr);

    NwdafH2Server h2(o.h2, [&o](const SbiRequest& req, const std::string& remote) {
        const auto cb = req.headers.find("3gpp-sbi-callback");
        print(o, "h2", req.method, req.path, remote, cb == req.headers.end() ? "" : cb->second, req.body);
        SbiResponse res;
        res.status = o.status;
        return res;
    });
    try {
        h2.start();
    } catch (const std::exception& e) {
        std::cerr << "nwdaf-notify-sink: " << e.what() << "\n";
        return 1;
    }
    std::cerr << "HTTP/2 (" << (o.h2.tls ? "h2" : "h2c") << ") on " << o.h2.bind_address << ":" << h2.port()
              << "\n";

    httplib::Server http1;
    std::thread http1_thread;
    if (o.http1_port > 0) {
        http1.Post(".*", [&o](const httplib::Request& req, httplib::Response& res) {
            print(o, "http/1.1", req.method, req.path, req.remote_addr, "", req.body);
            res.status = o.status;
        });
        if (!http1.bind_to_port(o.h2.bind_address, o.http1_port)) {
            std::cerr << "nwdaf-notify-sink: cannot listen on HTTP/1.1 port " << o.http1_port << "\n";
            return 1;
        }
        http1_thread = std::thread([&http1] { http1.listen_after_bind(); });
        std::cerr << "HTTP/1.1 on " << o.h2.bind_address << ":" << o.http1_port << "\n";
    }

    int sig = 0;
    sigwait(&stop, &sig);
    h2.stop();
    if (http1_thread.joinable()) {
        http1.stop();
        http1_thread.join();
    }
    return 0;
}
