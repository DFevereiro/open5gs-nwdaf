// H1.7 — the 3GPP Nnwdaf_AnalyticsInfo and Nnwdaf_EventsSubscription resources
// (TS 29.520 V18.14.0), exercised end to end over the SBI. Every error body is
// validated against the official TS 29.571 ProblemDetails schema.
//
// No analytics ID is advertised yet (REL18_IMPLEMENTED is empty), so these
// tests pin the request-validation layers and the truthful "not supported"
// outcomes; per-ID success paths join as each Rel-18 mapping lands.
#include <catch2/catch_test_macros.hpp>
#include "nwdaf_server.hpp"
#include "nwdaf_analytics.hpp"
#include "nwdaf_sbi.hpp"
#include "nwdaf_schema_validator.hpp"
#include "nwdaf_subscription.hpp"
#include "mock_open5gs.hpp"
#include <httplib.h>
#include <chrono>
#include <memory>
#include <thread>
#include <cctype>

using json = nlohmann::json;

static const int SBI_PORT = 17782;
static const char* ANALYTICS = "/nnwdaf-analyticsinfo/v1/analytics";
static const char* SUBS      = "/nnwdaf-eventssubscription/v1/subscriptions";

static NwdafConfig sbiConfig(const std::string& openapi_dir = NWDAF_3GPP_OPENAPI_DIR) {
    NwdafConfig cfg;
    cfg.nf_instance_id = "sbi-test-uuid";
    cfg.plmn_mcc = "999"; cfg.plmn_mnc = "70";
    cfg.sbi_bind_address = "127.0.0.1"; cfg.sbi_port = SBI_PORT;
    cfg.nf_service_names = {{"AMF","amfd"},{"SMF","smfd"},{"UPF","upfd"}};
    cfg.throughput_interfaces = {"ogstun"};
    cfg.supi_regex = "imsi-(\\d{15})";
    cfg.nrf_register_on_startup = false;
    cfg.model_dir = "/tmp/nwdaf_sbi_models";
    cfg.rate_limit_per_ip_rps = 0; cfg.rate_limit_global_rps = 0;
    cfg.openapi_3gpp_dir = openapi_dir;
    cfg.log_level = "warn"; cfg.log_file = "/tmp/nwdaf_sbi_test.log";
    return cfg;
}

struct SbiServer {
    NwdafConfig            cfg;
    MockNwdafCollector     collector;
    NwdafAnalyticsEngine   engine;
    NwdafSubscriptionStore subs;
    NwdafServer            server;
    std::thread            thread;

    explicit SbiServer(const std::string& openapi_dir = NWDAF_3GPP_OPENAPI_DIR)
        : cfg(sbiConfig(openapi_dir)), collector(cfg), engine(collector, cfg),
          server(engine, subs, cfg)
    {
        thread = std::thread([this]{ server.start(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    ~SbiServer() {
        server.stop();
        if (thread.joinable()) thread.join();
    }
};

static SbiServer& server() {
    static SbiServer s;
    return s;
}

static httplib::Client client() {
    httplib::Client c("127.0.0.1", SBI_PORT);
    c.set_connection_timeout(3, 0);
    return c;
}

// A 3GPP error response: status, problem+json, schema-valid, and the cause.
static json requireProblem(const httplib::Result& res, int status, const std::string& cause) {
    REQUIRE(res);
    REQUIRE(res->status == status);
    REQUIRE(res->get_header_value("Content-Type") == "application/problem+json");
    json body = json::parse(res->body);
    static NwdafSchemaValidator official(NWDAF_3GPP_OPENAPI_DIR);
    auto v = official.validate(body, "TS29571_CommonData.yaml#/components/schemas/ProblemDetails");
    INFO((v.empty() ? std::string() : v.front().pointer + " " + v.front().reason));
    REQUIRE(v.empty());
    REQUIRE(body["cause"] == cause);
    return body;
}

// RFC 3986 percent-encoding of a query value (everything but unreserved).
static std::string q(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

// ── RFC 3339 DateTime (TS 29.571) ───────────────────────────────────────────

TEST_CASE("H1.7: DateTime parsing honours offsets and fractional seconds") {
    auto utc = NwdafSbiService::parseDateTime("2026-09-28T10:00:00Z");
    auto off = NwdafSbiService::parseDateTime("2026-09-28T12:00:00+02:00");
    auto frac = NwdafSbiService::parseDateTime("2026-09-28T10:00:00.500Z");
    REQUIRE(utc); REQUIRE(off); REQUIRE(frac);
    REQUIRE(*utc == *off);
    REQUIRE(*frac - *utc == std::chrono::milliseconds(500));
    REQUIRE_FALSE(NwdafSbiService::parseDateTime("2026-09-28 10:00:00"));
    REQUIRE_FALSE(NwdafSbiService::parseDateTime("2026-13-28T10:00:00Z"));
}

// ── Nnwdaf_AnalyticsInfo ────────────────────────────────────────────────────

TEST_CASE("H1.7: AnalyticsInfo without event-id is MANDATORY_QUERY_PARAM_MISSING") {
    (void)server();
    auto body = requireProblem(client().Get(ANALYTICS), 400, "MANDATORY_QUERY_PARAM_MISSING");
    REQUIRE(body["invalidParams"][0]["param"] == "event-id");
}

TEST_CASE("H1.7: AnalyticsInfo for an analytics ID this NWDAF does not advertise") {
    (void)server();
    auto body = requireProblem(client().Get(std::string(ANALYTICS) + "?event-id=NF_LOAD"),
                               400, "MANDATORY_QUERY_PARAM_INCORRECT");
    REQUIRE(body["invalidParams"][0]["param"] == "event-id");
    // The NWDAF states what it does support (none yet).
    REQUIRE(body["supportedFeatures"] == "0");
}

TEST_CASE("H1.7: legacy ID spellings are not accepted on the 3GPP interface") {
    (void)server();
    // Schema-valid (EventId is an extensible enum) but not a Rel-18 value.
    requireProblem(client().Get(std::string(ANALYTICS) + "?event-id=QoS_SUSTAINABILITY"),
                   400, "MANDATORY_QUERY_PARAM_INCORRECT");
}

TEST_CASE("H1.7: malformed or schema-invalid optional query parameters are rejected") {
    (void)server();
    auto cli = client();
    std::string base = std::string(ANALYTICS) + "?event-id=NF_LOAD&";

    auto b1 = requireProblem(cli.Get(base + "ana-req=" + q("{not json")),
                             400, "OPTIONAL_QUERY_PARAM_INCORRECT");
    REQUIRE(b1["invalidParams"][0]["param"] == "ana-req");

    requireProblem(cli.Get(base + "ana-req=" + q(R"({"startTs":5})")),
                   400, "OPTIONAL_QUERY_PARAM_INCORRECT");
    requireProblem(cli.Get(base + "supported-features=xyz"),
                   400, "OPTIONAL_QUERY_PARAM_INCORRECT");
    requireProblem(cli.Get(base + "tgt-ue=" + q(R"({"anyUe":"yes"})")),
                   400, "OPTIONAL_QUERY_PARAM_INCORRECT");
}

TEST_CASE("H1.7: a target period spanning now is BOTH_STAT_PRED_NOT_ALLOWED") {
    (void)server();
    const std::string ana_req = R"({"startTs":"2020-01-01T00:00:00Z","endTs":"2099-01-01T00:00:00Z"})";
    requireProblem(client().Get(std::string(ANALYTICS) + "?event-id=NF_LOAD&ana-req=" + q(ana_req)),
                   400, "BOTH_STAT_PRED_NOT_ALLOWED");
}

TEST_CASE("H1.7: unsupported query parameters of GET are ignored (TS 29.500 §5.2.9)") {
    (void)server();
    // Outcome is decided by event-id alone, not INVALID_QUERY_PARAM.
    requireProblem(client().Get(std::string(ANALYTICS) + "?event-id=NF_LOAD&foo=bar"),
                   400, "MANDATORY_QUERY_PARAM_INCORRECT");
}

TEST_CASE("H1.7: unsupported methods on AnalyticsInfo are 405 with Allow") {
    (void)server();
    auto res = client().Put(ANALYTICS, "{}", "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 405);
    REQUIRE(res->get_header_value("Allow") == "GET, POST");
}

TEST_CASE("H1.7: the deprecated JSON-body POST on the AnalyticsInfo path still works") {
    (void)server();
    auto res = client().Post(ANALYTICS, R"({"analyticsId":"NF_LOAD"})", "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 200);
}

// ── Nnwdaf_EventsSubscription ───────────────────────────────────────────────

static const json validSub = {
    {"eventSubscriptions", {{{"event", "NF_LOAD"},
                             {"notificationMethod", "PERIODIC"},
                             {"repetitionPeriod", 60}}}},
    {"notificationURI", "http://127.0.0.1:9999/notify"},
};

TEST_CASE("H1.7: Subscribe requires application/json") {
    (void)server();
    auto res = client().Post(SUBS, validSub.dump(), "text/plain");
    REQUIRE(res);
    REQUIRE(res->status == 415);
}

TEST_CASE("H1.7: Subscribe with a malformed body is INVALID_MSG_FORMAT") {
    (void)server();
    requireProblem(client().Post(SUBS, "{nope", "application/json"), 400, "INVALID_MSG_FORMAT");
}

TEST_CASE("H1.7: Subscribe without eventSubscriptions is MANDATORY_IE_MISSING") {
    (void)server();
    auto body = requireProblem(client().Post(SUBS, "{}", "application/json"),
                               400, "MANDATORY_IE_MISSING");
    REQUIRE(body["invalidParams"][0]["param"] == "/eventSubscriptions");
}

TEST_CASE("H1.7: Subscribe without notificationURI is MANDATORY_IE_MISSING") {
    (void)server();
    json sub = validSub; sub.erase("notificationURI");
    auto body = requireProblem(client().Post(SUBS, sub.dump(), "application/json"),
                               400, "MANDATORY_IE_MISSING");
    REQUIRE(body["invalidParams"][0]["param"] == "/notificationURI");
}

TEST_CASE("H1.7: unsupported reporting requirements are rejected, not ignored") {
    (void)server();
    auto cli = client();
    json sub = validSub; sub["evtReq"] = {{"sampRatio", 50}};
    auto body = requireProblem(cli.Post(SUBS, sub.dump(), "application/json"),
                               400, "OPTIONAL_IE_INCORRECT");
    REQUIRE(body["invalidParams"][0]["param"] == "/evtReq/sampRatio");

    sub["evtReq"] = {{"notifMethod", "ON_EVENT_DETECTION"}};
    body = requireProblem(cli.Post(SUBS, sub.dump(), "application/json"),
                          400, "OPTIONAL_IE_INCORRECT");
    REQUIRE(body["invalidParams"][0]["param"] == "/evtReq/notifMethod");
}

TEST_CASE("H1.7: an EneNA-only attribute is ignored when EneNA is not supported (I-2)") {
    (void)server();
    json sub = validSub; sub["evtReq"] = {{"notifFlag", "DEACTIVATE"}};
    // Not OPTIONAL_IE_INCORRECT: notifFlag was ignored; the outcome is decided
    // by the (unadvertised) analytics ID.
    requireProblem(client().Post(SUBS, sub.dump(), "application/json"),
                   400, "MANDATORY_IE_INCORRECT");
}

TEST_CASE("H1.7: Subscribe for analytics this NWDAF does not advertise") {
    (void)server();
    auto body = requireProblem(client().Post(SUBS, validSub.dump(), "application/json"),
                               400, "MANDATORY_IE_INCORRECT");
    REQUIRE(body["invalidParams"][0]["param"] == "/eventSubscriptions/0/event");
    REQUIRE(body["supportedFeatures"] == "0");
}

TEST_CASE("H1.7: Subscribe with a target period spanning now is BOTH_STAT_PRED_NOT_ALLOWED") {
    (void)server();
    json sub = validSub;
    sub["eventSubscriptions"][0]["extraReportReq"] = {
        {"startTs", "2020-01-01T00:00:00Z"}, {"endTs", "2099-01-01T00:00:00Z"}};
    requireProblem(client().Post(SUBS, sub.dump(), "application/json"),
                   400, "BOTH_STAT_PRED_NOT_ALLOWED");
}

TEST_CASE("H1.7: modify or delete an unknown subscription is SUBSCRIPTION_NOT_FOUND") {
    (void)server();
    auto cli = client();
    requireProblem(cli.Put(std::string(SUBS) + "/nope", validSub.dump(), "application/json"),
                   404, "SUBSCRIPTION_NOT_FOUND");
    requireProblem(cli.Delete(std::string(SUBS) + "/nope"), 404, "SUBSCRIPTION_NOT_FOUND");
}

TEST_CASE("H1.7: unsupported methods on the subscription resources are 405 with Allow") {
    (void)server();
    auto cli = client();
    auto res = cli.Get(SUBS);
    REQUIRE(res);
    REQUIRE(res->status == 405);
    REQUIRE(res->get_header_value("Allow") == "POST");
    res = cli.Get(std::string(SUBS) + "/some-id");
    REQUIRE(res);
    REQUIRE(res->status == 405);
    REQUIRE(res->get_header_value("Allow") == "PUT, DELETE");
}

TEST_CASE("H1.7: without the official artifacts the 3GPP interfaces fail closed") {
    SbiServer degraded("/nonexistent/3gpp-openapi");
    auto res = client().Get(std::string(ANALYTICS) + "?event-id=NF_LOAD");
    REQUIRE(res);
    REQUIRE(res->status == 500);
    REQUIRE(json::parse(res->body)["cause"] == "SYSTEM_FAILURE");
}
