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

static NwdafConfig tlsConfig(const std::string& ca_file, int h2_port = 0) {
    NwdafConfig cfg;
    cfg.nf_instance_id = "test-security-uuid";
    cfg.plmn_mcc = "999"; cfg.plmn_mnc = "70";
    cfg.sbi_bind_address = "127.0.0.1"; cfg.sbi_port = SEC_PORT;
    cfg.sbi_h2_port = h2_port;
    cfg.openapi_3gpp_dir = NWDAF_3GPP_OPENAPI_DIR;
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

    explicit TlsServer(const std::string& ca_file, int h2_port = 0)
        : cfg(tlsConfig(ca_file, h2_port)), collector(cfg), engine(collector, cfg),
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

#if defined(NWDAF_USE_TLS) && defined(NWDAF_USE_HTTP2)
#include "nwdaf_http_client.hpp"

// H1.8 + H1.10: h2 over TLS, negotiated with ALPN, with mutual TLS.
static const int SEC_H2_PORT = 17785;

struct TlsH2Server : TlsServer {
    TlsH2Server() : TlsServer(TLS_DIR + "ca.pem", SEC_H2_PORT) {}
};

static NwdafHttpResponse h2Get(bool present_cert) {
    NwdafConfig client_cfg;
    client_cfg.tls_ca_file = TLS_DIR + "ca.pem";           // verify the server
    client_cfg.tls_enabled = present_cert;                  // present a client cert
    client_cfg.tls_cert_file = TLS_DIR + "client.pem";
    client_cfg.tls_key_file  = TLS_DIR + "client.key";
    return NwdafHttpClient(client_cfg).request(
        "GET", "https://127.0.0.1:" + std::to_string(SEC_H2_PORT) +
               "/nnwdaf-analyticsinfo/v1/analytics?event-id=NF_LOAD");
}

TEST_CASE("H1.8: h2 over TLS (ALPN) with a client certificate from the CA") {
    TlsH2Server srv;
    auto res = h2Get(true);
    INFO(res.error);
    REQUIRE(res);
    REQUIRE(res.http_version == 2);
    REQUIRE(res.status == 400);   // served: NF_LOAD is not advertised here
}

TEST_CASE("H1.8: h2 over TLS refuses a client without a certificate") {
    TlsH2Server srv;
    REQUIRE_FALSE(h2Get(false));
}
#endif

#if defined(NWDAF_USE_TLS)
#include "jwt_test_util.hpp"
#include "nwdaf_http_client.hpp"

// H1.10: OAuth 2.0 on the 3GPP interfaces, end to end (TS 29.500 §6.7.3).
static const int OAUTH_PORT = 17787, OAUTH_H2_PORT = 17788;
static const std::string OAUTH_NWDAF_ID = "198e9234-b849-42a2-9f70-d9a587616c2b";

struct OAuthServer {
    jwt_test::Key          key{"RSA"};
    NwdafConfig            cfg;
    MockNwdafCollector     collector;
    NwdafAnalyticsEngine   engine;
    NwdafSubscriptionStore subs;
    NwdafServer            server;
    std::thread            thread;

    static NwdafConfig config(const jwt_test::Key& k) {
        NwdafConfig c = tlsConfig("", 0);
        c.tls_enabled = false;                      // tokens over plain h2c / HTTP/1.1
        c.nf_instance_id = OAUTH_NWDAF_ID;
        c.sbi_port = OAUTH_PORT;
        c.sbi_h2_port = OAUTH_H2_PORT;
        c.oauth_enabled = true;
        c.oauth_nrf_public_key_file = "/tmp/nwdaf_oauth_srv_pub.pem";
        k.writePublicPem(c.oauth_nrf_public_key_file);
        return c;
    }
    OAuthServer() : cfg(config(key)), collector(cfg), engine(collector, cfg), server(engine, subs, cfg) {
        thread = std::thread([this]{ server.start(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    ~OAuthServer() { server.stop(); if (thread.joinable()) thread.join(); }

    NwdafHttpResponse get(const std::string& path, const std::string& bearer = "") {
        const int port = NwdafHttpClient::http2() ? OAUTH_H2_PORT : OAUTH_PORT;
        std::vector<std::pair<std::string, std::string>> h;
        if (!bearer.empty()) h.emplace_back("Authorization", "Bearer " + bearer);
        return NwdafHttpClient(NwdafConfig()).request("GET", "http://127.0.0.1:" + std::to_string(port) + path,
                                                      "", "", h);
    }
    std::string realm() const {
        return "http://127.0.0.1:" +
               std::to_string(NwdafHttpClient::http2() ? OAUTH_H2_PORT : OAUTH_PORT) +
               "/nnwdaf-analyticsinfo/v1";
    }
};

static const std::string NF_LOAD_Q = "/nnwdaf-analyticsinfo/v1/analytics?event-id=NF_LOAD";

TEST_CASE("H1.10: a request without an access token is 401 with a Bearer challenge") {
    OAuthServer srv;
    auto res = srv.get(NF_LOAD_Q);
    REQUIRE(res.status == 401);
    // TS 29.500 §6.7.3: realm = the API URI; no error attribute without a token.
    REQUIRE(res.header("www-authenticate") == "Bearer realm=\"" + srv.realm() + "\"");
}

TEST_CASE("H1.10: an invalid token is 401 invalid_token; missing claims add a ProblemDetails") {
    OAuthServer srv;
    json c = jwt_test::claimsFor(OAUTH_NWDAF_ID);
    c["exp"] = jwt_test::inSeconds(-5);
    auto res = srv.get(NF_LOAD_Q, jwt_test::token("RS256", c, &srv.key));
    REQUIRE(res.status == 401);
    REQUIRE(res.header("www-authenticate") ==
            "Bearer realm=\"" + srv.realm() + "\", error=\"invalid_token\"");

    c = jwt_test::claimsFor(OAUTH_NWDAF_ID);
    c.erase("sub");
    res = srv.get(NF_LOAD_Q, jwt_test::token("RS256", c, &srv.key));
    REQUIRE(res.status == 401);
    const json body = json::parse(res.body);
    REQUIRE(body["cause"] == "ACCESS_TOKEN_CLAIM_MISSING");
    REQUIRE(body["invalidParams"][0]["param"] == "sub");
}

TEST_CASE("H1.10: a token without the service's scope is 403 insufficient_scope") {
    OAuthServer srv;
    auto res = srv.get(NF_LOAD_Q, jwt_test::token("RS256",
        jwt_test::claimsFor(OAUTH_NWDAF_ID, "nnwdaf-eventssubscription"), &srv.key));
    REQUIRE(res.status == 403);
    REQUIRE(res.header("www-authenticate") == "Bearer realm=\"" + srv.realm() +
            "\", error=\"insufficient_scope\", scope=\"nnwdaf-analyticsinfo\"");
}

TEST_CASE("H1.10: a valid token reaches the service") {
    OAuthServer srv;
    auto res = srv.get(NF_LOAD_Q, jwt_test::token("RS256", jwt_test::claimsFor(OAUTH_NWDAF_ID), &srv.key));
    REQUIRE(res.status == 400);   // served: NF_LOAD is not advertised by this instance
    REQUIRE(json::parse(res.body)["cause"] == "MANDATORY_QUERY_PARAM_INCORRECT");
}

TEST_CASE("H1.10: analyticsIdList limits the analytics a token grants (I-6)") {
    OAuthServer srv;
    json c = jwt_test::claimsFor(OAUTH_NWDAF_ID);
    c["analyticsIdList"] = json::array({"SERVICE_EXPERIENCE"});
    auto res = srv.get(NF_LOAD_Q, jwt_test::token("RS256", c, &srv.key));
    REQUIRE(res.status == 403);
    REQUIRE(res.header("www-authenticate").find("insufficient_scope") != std::string::npos);
}

TEST_CASE("H1.10: the operator health probe stays open") {
    OAuthServer srv;
    httplib::Client op("127.0.0.1", OAUTH_PORT);
    auto res = op.Get(HEALTH);
    REQUIRE(res);
    REQUIRE(res->status == 200);
}

TEST_CASE("SEC-01: the deprecated POST /nnwdaf-analyticsinfo/v1/analytics also needs a token") {
    OAuthServer srv;
    httplib::Client op("127.0.0.1", OAUTH_PORT);
    const std::string body = R"({"analyticsId":"NF_LOAD"})";
    auto res = op.Post("/nnwdaf-analyticsinfo/v1/analytics", body, "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 401);
    httplib::Headers bearer = {{"Authorization", "Bearer any-token"}};
    res = op.Post("/nnwdaf-analyticsinfo/v1/analytics", bearer, body, "application/json");
    REQUIRE(res);
    REQUIRE(res->status != 401);   // the operator API's Bearer check passes
}

TEST_CASE("H1.10: OAuth enabled without a key source stops startup") {
    NwdafConfig cfg = tlsConfig("", 0);
    cfg.oauth_enabled = true;
    MockNwdafCollector collector(cfg);
    NwdafAnalyticsEngine engine(collector, cfg);
    NwdafSubscriptionStore subs;
    REQUIRE_THROWS(NwdafServer(engine, subs, cfg));
}
#endif
