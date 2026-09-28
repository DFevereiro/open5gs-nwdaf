// H1.10 — SBI transport security (TS 33.501 §13.1): mutual TLS.
// Fixtures live in tests/data/tls (see the README there). OAuth2 access-token
// validation joins this suite with H1.10's token work.
#include <catch2/catch_test_macros.hpp>

#ifdef NWDAF_USE_TLS

#include "nwdaf_server.hpp"
#include "nwdaf_analytics.hpp"
#include "nwdaf_subscription.hpp"
#include "mock_open5gs.hpp"
#include <httplib.h>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

static const int SEC_PORT = 17781;
static const std::string TLS_DIR = std::string(NWDAF_TEST_DATA_DIR) + "/tls/";
static const char* HEALTH = "/nwdaf-analytics/v1/health";

static NwdafConfig tlsConfig(const std::string& ca_file) {
    NwdafConfig cfg;
    cfg.nf_instance_id = "test-security-uuid";
    cfg.plmn_mcc = "999"; cfg.plmn_mnc = "70";
    cfg.sbi_bind_address = "127.0.0.1"; cfg.sbi_port = SEC_PORT;
    cfg.nf_service_names = {{"AMF","amfd"},{"SMF","smfd"},{"UPF","upfd"}};
    cfg.throughput_interfaces = {"ogstun"};
    cfg.supi_regex = "imsi-(\\d{15})";
    cfg.nrf_register_on_startup = false;
    cfg.model_dir = "/tmp/nwdaf_sec_models";
    cfg.log_level = "warn"; cfg.log_file = "/tmp/nwdaf_sec_test.log";
    cfg.tls_enabled   = true;
    cfg.tls_cert_file = TLS_DIR + "server.pem";
    cfg.tls_key_file  = TLS_DIR + "server.key";
    cfg.tls_ca_file   = ca_file;
    return cfg;
}

struct TlsServer {
    NwdafConfig            cfg;
    MockNwdafCollector     collector;
    NwdafAnalyticsEngine   engine;
    NwdafSubscriptionStore subs;
    NwdafServer            server;
    std::thread            thread;

    explicit TlsServer(const std::string& ca_file)
        : cfg(tlsConfig(ca_file)), collector(cfg), engine(collector, cfg),
          server(engine, subs, cfg)
    {
        thread = std::thread([this]{ server.start(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    ~TlsServer() {
        server.stop();
        if (thread.joinable()) thread.join();
    }
};

// A client that verifies the server against the test CA, optionally
// presenting a client certificate.
static httplib::Result getHealth(const std::string& cert = "", const std::string& key = "") {
    std::unique_ptr<httplib::SSLClient> cli =
        cert.empty() ? std::make_unique<httplib::SSLClient>("127.0.0.1", SEC_PORT)
                     : std::make_unique<httplib::SSLClient>("127.0.0.1", SEC_PORT,
                                                            TLS_DIR + cert, TLS_DIR + key);
    cli->set_ca_cert_path((TLS_DIR + "ca.pem").c_str());
    cli->enable_server_certificate_verification(true);
    cli->set_connection_timeout(3, 0);
    return cli->Get(HEALTH);
}

TEST_CASE("H1.10: with tls_ca_file set, a client certificate from that CA is accepted") {
    TlsServer srv(TLS_DIR + "ca.pem");
    auto res = getHealth("client.pem", "client.key");
    REQUIRE(res);
    REQUIRE(res->status == 200);
}

TEST_CASE("H1.10: with tls_ca_file set, a client without a certificate is refused") {
    TlsServer srv(TLS_DIR + "ca.pem");
    auto res = getHealth();
    REQUIRE_FALSE(res);   // TLS handshake fails: no HTTP response at all
}

TEST_CASE("H1.10: with tls_ca_file set, a certificate from another CA is refused") {
    TlsServer srv(TLS_DIR + "ca.pem");
    auto res = getHealth("rogue-client.pem", "rogue-client.key");
    REQUIRE_FALSE(res);
}

TEST_CASE("H1.10: without tls_ca_file, server-only TLS is unchanged") {
    TlsServer srv("");
    auto res = getHealth();
    REQUIRE(res);
    REQUIRE(res->status == 200);
}

TEST_CASE("H1.10: an unreadable tls_ca_file fails startup instead of rejecting everyone") {
    auto cfg = tlsConfig("/nonexistent/ca.pem");
    MockNwdafCollector collector(cfg);
    NwdafAnalyticsEngine engine(collector, cfg);
    NwdafSubscriptionStore subs;
    REQUIRE_THROWS(NwdafServer(engine, subs, cfg));
}

#endif  // NWDAF_USE_TLS
