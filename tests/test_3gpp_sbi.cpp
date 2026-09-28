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
#include "nwdaf_http_client.hpp"
#ifdef NWDAF_USE_HTTP2
#include "nwdaf_h2_server.hpp"
#endif
#include <httplib.h>
#include <chrono>
#include <memory>
#include <thread>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <cctype>
#include <ctime>

using json = nlohmann::json;

static const int SBI_PORT = 17782;
static const int H2_PORT  = 17784;   // H1.8: HTTP/2 listener of the test servers
static const char* ANALYTICS = "/nnwdaf-analyticsinfo/v1/analytics";
static const char* SUBS      = "/nnwdaf-eventssubscription/v1/subscriptions";

static NwdafConfig sbiConfig(const std::string& openapi_dir = NWDAF_3GPP_OPENAPI_DIR) {
    NwdafConfig cfg;
    cfg.nf_instance_id = "sbi-test-uuid";
    cfg.plmn_mcc = "999"; cfg.plmn_mnc = "70";
    cfg.sbi_bind_address = "127.0.0.1"; cfg.sbi_port = SBI_PORT;
    cfg.sbi_h2_port = H2_PORT;
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

    explicit SbiServer(NwdafConfig c, const std::vector<NfMetric>& metrics = {})
        : cfg(std::move(c)), collector(cfg), engine(collector, cfg),
          server(engine, subs, cfg)
    {
        if (!metrics.empty()) {
            // Let the background loop cache the measurements, as in production.
            collector.setNfMetrics(metrics);
            collector.startBackgroundCollection();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            collector.stopBackgroundCollection();
        }
        thread = std::thread([this]{ server.start(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    ~SbiServer() {
        server.stop();
        if (thread.joinable()) thread.join();
    }
};

// No nf_instance_ids: NF_LOAD is implemented but not advertised here.
static SbiServer& server() {
    static SbiServer s(sbiConfig());
    return s;
}

static const char* AMF_ID = "11111111-1111-4111-8111-111111111111";
static const char* SMF_ID = "22222222-2222-4222-8222-222222222222";
static const char* UPF_ID = "33333333-3333-4333-8333-333333333333";

// nf_instance_ids configured: NF_LOAD is advertised. The metrics cover a
// running NF with an ID (AMF, UPF), a stopped NF with an ID (SMF) and a
// running NF without one (UDM) — only the first kind may be reported.
static SbiServer& nfLoadServer() {
    static SbiServer s([] {
        NwdafConfig cfg = sbiConfig();
        cfg.nf_instance_ids = {{"AMF", AMF_ID}, {"SMF", SMF_ID}, {"UPF", UPF_ID}};
        return cfg;
    }(), {
        {"AMF", "active",   101, 12.0, 51200, 22.0, "LOW"},
        {"SMF", "inactive", 0,    0.0,     0,  0.0, "LOW"},
        {"UPF", "active",   103, 30.0, 96000, 41.4, "MEDIUM"},
        {"UDM", "active",   104,  2.0, 20000,  5.0, "LOW"},
    });
    return s;
}

// Talks to the 3GPP interfaces as a Rel-18 consumer does: over HTTP/2 to the
// h2 listener when the build has it (rel18-sbi profile), else HTTP/1.1
// (dev-legacy). Every response asserts the protocol actually used.
struct TestResponse {
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;   // lower-case names
    int http_version = 0;
    std::string get_header_value(std::string name) const {
        for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    }
};
using Result = std::optional<TestResponse>;

struct TestClient {
    Result Get(const std::string& path) { return send("GET", path, "", ""); }
    Result Delete(const std::string& path) { return send("DELETE", path, "", ""); }
    Result Post(const std::string& path, const std::string& body, const std::string& ct) {
        return send("POST", path, body, ct);
    }
    Result Put(const std::string& path, const std::string& body, const std::string& ct) {
        return send("PUT", path, body, ct);
    }
    Result send(const std::string& method, const std::string& path,
                const std::string& body, const std::string& ct) {
        const bool h2 = NwdafHttpClient::http2();
        const std::string url = "http://127.0.0.1:" + std::to_string(h2 ? H2_PORT : SBI_PORT) + path;
        auto r = NwdafHttpClient(NwdafConfig()).request(method, url, body, ct);
        if (!r) return std::nullopt;
        REQUIRE(r.http_version == (h2 ? 2 : 1));
        return TestResponse{static_cast<int>(r.status), r.body, r.headers, r.http_version};
    }
};

static TestClient client() { return {}; }

// A 3GPP error response: status, problem+json, schema-valid, and the cause.
static json requireProblem(const Result& res, int status, const std::string& cause) {
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
    REQUIRE(res->get_header_value("Allow") == "GET");
}

TEST_CASE("H1.7: the deprecated JSON-body POST on the AnalyticsInfo path still works (operator port)") {
    (void)server();
    httplib::Client legacy("127.0.0.1", SBI_PORT);
    auto res = legacy.Post(ANALYTICS, R"({"analyticsId":"NF_LOAD"})", "application/json");
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
    SbiServer degraded(sbiConfig("/nonexistent/3gpp-openapi"));
    auto res = client().Get(std::string(ANALYTICS) + "?event-id=NF_LOAD");
    REQUIRE(res);
    REQUIRE(res->status == 500);
    REQUIRE(json::parse(res->body)["cause"] == "SYSTEM_FAILURE");
}

// ── NF_LOAD on Nnwdaf_AnalyticsInfo (H1.7 Step 3) ───────────────────────────

static const std::string ANY_UE = R"({"anyUe":true})";

static Result nfLoad(const std::string& extra = "", bool with_target = true) {
    std::string path = std::string(ANALYTICS) + "?event-id=NF_LOAD";
    if (with_target) path += "&tgt-ue=" + q(ANY_UE);
    return client().Get(path + extra);
}

// A 200 AnalyticsData response, validated against the official schema.
static json requireAnalyticsData(const Result& res) {
    REQUIRE(res);
    INFO(res->body);
    REQUIRE(res->status == 200);
    REQUIRE(res->get_header_value("Content-Type") == "application/json");
    json body = json::parse(res->body);
    static NwdafSchemaValidator official(NWDAF_3GPP_OPENAPI_DIR);
    auto v = official.validate(body, "TS29520_Nnwdaf_AnalyticsInfo.yaml#/components/schemas/AnalyticsData");
    INFO((v.empty() ? std::string() : v.front().pointer + " " + v.front().reason));
    REQUIRE(v.empty());
    return body;
}

TEST_CASE("H1.7: NF_LOAD is served when nf_instance_ids is configured") {
    (void)nfLoadServer();
    json body = requireAnalyticsData(nfLoad());
    const json& infos = body["nfLoadLevelInfos"];
    // Only running NFs with a configured instance ID: AMF and UPF. The stopped
    // SMF and the unidentified UDM are not reported.
    REQUIRE(infos.size() == 2);
    REQUIRE(infos[0]["nfType"] == "AMF");
    REQUIRE(infos[0]["nfInstanceId"] == AMF_ID);
    REQUIRE(infos[0]["nfCpuUsage"] == 22);
    REQUIRE(infos[1]["nfType"] == "UPF");
    REQUIRE(infos[1]["nfCpuUsage"] == 41);
    // Registration status is not observed, so it is not claimed.
    REQUIRE_FALSE(infos[0].contains("nfStatus"));
    REQUIRE(body.contains("timeStampGen"));
    REQUIRE(body.contains("expiry"));
    REQUIRE_FALSE(body.contains("suppFeat"));   // consumer sent no supported-features
}

TEST_CASE("H1.7: NF_LOAD returns the negotiated supported features as suppFeat") {
    (void)nfLoadServer();
    // NWDAF supports NfLoad (AnalyticsInfo bit 8) only.
    REQUIRE(requireAnalyticsData(nfLoad("&supported-features=FF"))["suppFeat"] == "80");
    REQUIRE(requireAnalyticsData(nfLoad("&supported-features=1"))["suppFeat"] == "0");
}

TEST_CASE("H1.7: NF_LOAD honours nfTypes, nfInstanceIds and maxObjectNbr") {
    (void)nfLoadServer();
    json b = requireAnalyticsData(nfLoad("&event-filter=" + q(R"({"nfTypes":["UPF"]})")));
    REQUIRE(b["nfLoadLevelInfos"].size() == 1);
    REQUIRE(b["nfLoadLevelInfos"][0]["nfType"] == "UPF");

    b = requireAnalyticsData(nfLoad("&event-filter=" +
                                    q(std::string(R"({"nfInstanceIds":[")") + AMF_ID + "\"]}")));
    REQUIRE(b["nfLoadLevelInfos"].size() == 1);
    REQUIRE(b["nfLoadLevelInfos"][0]["nfInstanceId"] == AMF_ID);

    b = requireAnalyticsData(nfLoad("&ana-req=" + q(R"({"maxObjectNbr":1})")));
    REQUIRE(b["nfLoadLevelInfos"].size() == 1);
}

TEST_CASE("H1.7: NF_LOAD with no matching data is 204 No Content") {
    (void)nfLoadServer();
    auto res = nfLoad("&event-filter=" + q(R"({"nfTypes":["NRF"]})"));
    REQUIRE(res);
    REQUIRE(res->status == 204);
    REQUIRE(res->body.empty());
}

TEST_CASE("H1.7: NF_LOAD target UE rules") {
    (void)nfLoadServer();
    auto b = requireProblem(nfLoad("", false), 400, "MANDATORY_QUERY_PARAM_MISSING");
    REQUIRE(b["invalidParams"][0]["param"] == "tgt-ue");

    auto cli = client();
    const std::string base = std::string(ANALYTICS) + "?event-id=NF_LOAD&tgt-ue=";
    requireProblem(cli.Get(base + q(R"({"supis":["imsi-999700000000001"]})")),
                   400, "MANDATORY_QUERY_PARAM_INCORRECT");
    requireProblem(cli.Get(base + q(R"({"anyUe":false})")),
                   400, "MANDATORY_QUERY_PARAM_INCORRECT");
}

TEST_CASE("H1.7: NF_LOAD rejects relevant filters it does not implement") {
    (void)nfLoadServer();
    auto b = requireProblem(nfLoad("&event-filter=" + q(R"({"snssais":[{"sst":1}]})")),
                            400, "OPTIONAL_QUERY_PARAM_INCORRECT");
    REQUIRE(b["invalidParams"][0]["param"] == "event-filter");
    REQUIRE(b["supportedFeatures"] == "80");

    requireProblem(nfLoad("&ana-req=" + q(R"({"sampRatio":50})")),
                   400, "OPTIONAL_QUERY_PARAM_INCORRECT");
}

TEST_CASE("H1.7: NF_LOAD ignores networkArea, which belongs to NfLoadExt (I-2)") {
    (void)nfLoadServer();
    const std::string area =
        R"({"networkArea":{"tais":[{"plmnId":{"mcc":"999","mnc":"70"},"tac":"000001"}]}})";
    auto b = requireAnalyticsData(nfLoad("&event-filter=" + q(area)));
    REQUIRE(b["nfLoadLevelInfos"].size() == 2);
}

TEST_CASE("H1.7: NF_LOAD past statistics are UNAVAILABLE_DATA; predictions are unsupported") {
    (void)nfLoadServer();
    requireProblem(nfLoad("&ana-req=" +
                          q(R"({"startTs":"2020-01-01T00:00:00Z","endTs":"2020-01-02T00:00:00Z"})")),
                   500, "UNAVAILABLE_DATA");
    requireProblem(nfLoad("&ana-req=" +
                          q(R"({"startTs":"2098-01-01T00:00:00Z","endTs":"2099-01-01T00:00:00Z"})")),
                   400, "OPTIONAL_QUERY_PARAM_INCORRECT");
}

// ── NF_LOAD on Nnwdaf_EventsSubscription (H1.7 Step 3) ──────────────────────

static json nfLoadSub(json evt_req = json(), json extra = json::object()) {
    json es = {{"event", "NF_LOAD"}, {"tgtUe", {{"anyUe", true}}},
               {"notificationMethod", "PERIODIC"}, {"repetitionPeriod", 60}};
    es.update(extra);
    json sub = {{"eventSubscriptions", json::array({es})},
                {"notificationURI", "http://127.0.0.1:17790/notify"}};
    if (!evt_req.is_null()) sub["evtReq"] = evt_req;
    return sub;
}

// A 200/201 NnwdafEventsSubscription body, validated against the official schema.
static json requireSubscription(const Result& res, int status) {
    REQUIRE(res);
    INFO(res->body);
    REQUIRE(res->status == status);
    json body = json::parse(res->body);
    static NwdafSchemaValidator official(NWDAF_3GPP_OPENAPI_DIR);
    auto v = official.validate(body, "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/NnwdafEventsSubscription");
    INFO((v.empty() ? std::string() : v.front().pointer + " " + v.front().reason));
    REQUIRE(v.empty());
    return body;
}

TEST_CASE("H1.7: Subscribe to NF_LOAD → 201 with Location and the negotiated features") {
    auto& srv = nfLoadServer();
    auto res = client().Post(SUBS, nfLoadSub().dump(), "application/json");
    json body = requireSubscription(res, 201);
    const std::string loc = res->get_header_value("Location");
    // {apiRoot} as the consumer addressed the NWDAF — the HTTP/2 port under H1.8.
    const int port = NwdafHttpClient::http2() ? H2_PORT : SBI_PORT;
    const std::string prefix = "http://127.0.0.1:" + std::to_string(port) + SUBS + "/";
    REQUIRE(loc.rfind(prefix, 0) == 0);
    const std::string id = loc.substr(prefix.size());
    REQUIRE(srv.subs.exists(id));
    REQUIRE(srv.subs.get(id).kind == "rel18");
    REQUIRE(body["supportedFeatures"] == "0");   // consumer declared none
    REQUIRE_FALSE(body.contains("failEventReports"));

    json with_features = nfLoadSub();
    with_features["supportedFeatures"] = "C0";   // bits 7, 8 — NfLoad is bit 7 here
    body = requireSubscription(client().Post(SUBS, with_features.dump(), "application/json"), 201);
    REQUIRE(body["supportedFeatures"] == "40");
}

TEST_CASE("H1.7: immRep returns the available report in the 201 body") {
    (void)nfLoadServer();
    json body = requireSubscription(
        client().Post(SUBS, nfLoadSub({{"immRep", true}}).dump(), "application/json"), 201);
    REQUIRE(body["eventNotifications"].size() == 1);
    REQUIRE(body["eventNotifications"][0]["event"] == "NF_LOAD");
    REQUIRE(body["eventNotifications"][0]["nfLoadLevelInfos"].size() == 2);
}

TEST_CASE("H1.7: events that cannot be served are reported in failEventReports") {
    (void)nfLoadServer();
    json sub = nfLoadSub();
    sub["eventSubscriptions"].push_back({{"event", "SERVICE_EXPERIENCE"},
                                         {"notificationMethod", "PERIODIC"},
                                         {"repetitionPeriod", 60}});
    json body = requireSubscription(client().Post(SUBS, sub.dump(), "application/json"), 201);
    REQUIRE(body["eventSubscriptions"].size() == 1);
    REQUIRE(body["failEventReports"].size() == 1);
    REQUIRE(body["failEventReports"][0]["event"] == "SERVICE_EXPERIENCE");
    REQUIRE(body["failEventReports"][0]["failureCode"] == "OTHER");
}

TEST_CASE("H1.7: NF_LOAD subscription input rules") {
    (void)nfLoadServer();
    auto cli = client();
    json no_target = nfLoadSub();
    no_target["eventSubscriptions"][0].erase("tgtUe");
    auto b = requireProblem(cli.Post(SUBS, no_target.dump(), "application/json"),
                            400, "MANDATORY_IE_MISSING");
    REQUIRE(b["invalidParams"][0]["param"] == "/eventSubscriptions/0/tgtUe");

    // An unimplemented relevant filter fails the event; with no other event
    // the request is rejected (I-3).
    requireProblem(cli.Post(SUBS, nfLoadSub(json(), {{"snssais", {{{"sst", 1}}}}}).dump(),
                            "application/json"),
                   400, "MANDATORY_IE_INCORRECT");

    // Default notification method is THRESHOLD, which is not supported yet.
    json threshold = nfLoadSub();
    threshold["eventSubscriptions"][0].erase("notificationMethod");
    requireProblem(cli.Post(SUBS, threshold.dump(), "application/json"),
                   400, "MANDATORY_IE_INCORRECT");
}

TEST_CASE("H1.7: modify and delete an NF_LOAD subscription") {
    (void)nfLoadServer();
    auto cli = client();
    auto created = cli.Post(SUBS, nfLoadSub().dump(), "application/json");
    REQUIRE(created);
    REQUIRE(created->status == 201);
    const std::string loc = created->get_header_value("Location");
    const std::string path = loc.substr(loc.find(SUBS));

    json changed = nfLoadSub(json(), {{"repetitionPeriod", 120}});
    json body = requireSubscription(cli.Put(path, changed.dump(), "application/json"), 200);
    REQUIRE(body["eventSubscriptions"][0]["repetitionPeriod"] == 120);

    auto del = cli.Delete(path);
    REQUIRE(del);
    REQUIRE(del->status == 204);
    requireProblem(cli.Delete(path), 404, "SUBSCRIPTION_NOT_FOUND");
}

TEST_CASE("H1.7: operator-API subscriptions are not Rel-18 resources") {
    auto& srv = nfLoadServer();
    const std::string legacy = srv.subs.create({{"analyticsId", "NF_LOAD"},
                                                {"notifUri", "http://127.0.0.1:9/x"}});
    requireProblem(client().Delete(std::string(SUBS) + "/" + legacy), 404, "SUBSCRIPTION_NOT_FOUND");
    REQUIRE(srv.subs.exists(legacy));
}

#ifdef NWDAF_ENABLE_PUSH_DELIVERY
#include "nwdaf_notifier.hpp"

// A notification consumer on port 17790 recording what it receives. It speaks
// HTTP/2 when the build does (H1.8: Rel-18 notifications go over HTTP/2), and
// HTTP/1.1 otherwise.
struct MockConsumer {
    std::mutex m;
    std::condition_variable cv;
    std::vector<json> received;
    std::string callback, content_type;   // of the first notification

    void record(const std::string& body, const std::string& cb, const std::string& ct) {
        std::lock_guard<std::mutex> lk(m);
        if (received.empty()) { callback = cb; content_type = ct; }
        received.push_back(json::parse(body));
        cv.notify_all();
    }
#ifdef NWDAF_USE_HTTP2
    NwdafH2Server server{[] { NwdafH2Server::Options o; o.port = 17790; return o; }(),
        [this](const SbiRequest& req, const std::string&) {
            auto h = [&](const char* k) { auto it = req.headers.find(k);
                                          return it == req.headers.end() ? std::string() : it->second; };
            record(req.body, h("3gpp-sbi-callback"), h("content-type"));
            return SbiResponse{204, "", "", {}};
        }};
    MockConsumer() { server.start(); }
    ~MockConsumer() { server.stop(); }
#else
    httplib::Server server;
    std::thread thread;
    MockConsumer() {
        server.Post("/notify", [this](const httplib::Request& req, httplib::Response& res) {
            record(req.body, req.get_header_value("3gpp-Sbi-Callback"),
                   req.get_header_value("Content-Type"));
            res.status = 204;
        });
        thread = std::thread([this] { server.listen("127.0.0.1", 17790); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ~MockConsumer() {
        server.stop();
        thread.join();
    }
#endif
    // Waits until at least n notifications arrived or the timeout passed.
    size_t waitFor(size_t n, std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait_for(lk, timeout, [&] { return received.size() >= n; });
        return received.size();
    }
    size_t count() {
        std::lock_guard<std::mutex> lk(m);
        return received.size();
    }
};

TEST_CASE("H1.7: NF_LOAD notifications are Rel-18 NnwdafEventsSubscriptionNotification") {
    auto& srv = nfLoadServer();
    MockConsumer consumer;

    // ONE_TIME: one report, then the subscription ends.
    auto created = client().Post(SUBS, nfLoadSub({{"notifMethod", "ONE_TIME"}}).dump(),
                                 "application/json");
    REQUIRE(created);
    REQUIRE(created->status == 201);
    const std::string loc = created->get_header_value("Location");
    const std::string id = loc.substr(loc.rfind('/') + 1);

    NwdafNotifier notifier(srv.subs, srv.engine, 1, nullptr, nullptr, srv.cfg);
    notifier.start();
    REQUIRE(consumer.waitFor(1, std::chrono::seconds(8)) == 1);
    notifier.stop();

    REQUIRE(consumer.callback == "Nnwdaf_EventsSubscription_Notify");
    REQUIRE(consumer.content_type == "application/json");
    const json n = consumer.received.front();
    static NwdafSchemaValidator official(NWDAF_3GPP_OPENAPI_DIR);
    auto v = official.validate(
        n, "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/NnwdafEventsSubscriptionNotification");
    INFO((v.empty() ? std::string() : v.front().pointer + " " + v.front().reason));
    REQUIRE(v.empty());
    REQUIRE(n["subscriptionId"] == id);
    REQUIRE(n["eventNotifications"][0]["event"] == "NF_LOAD");
    REQUIRE(n["eventNotifications"][0]["nfLoadLevelInfos"].size() == 2);
    REQUIRE_FALSE(srv.subs.exists(id));   // ONE_TIME ended it
}
#endif

TEST_CASE("H1.7: Subscribe asking for past NF_LOAD statistics is UNAVAILABLE_DATA") {
    (void)nfLoadServer();
    json sub = nfLoadSub(json(), {{"extraReportReq",
        {{"startTs", "2020-01-01T00:00:00Z"}, {"endTs", "2020-01-02T00:00:00Z"}}}});
    requireProblem(client().Post(SUBS, sub.dump(), "application/json"), 500, "UNAVAILABLE_DATA");
}

#ifdef NWDAF_ENABLE_PUSH_DELIVERY
static std::string createdId(const Result& res) {
    REQUIRE(res);
    INFO(res->body);
    REQUIRE(res->status == 201);
    const std::string loc = res->get_header_value("Location");
    return loc.substr(loc.rfind('/') + 1);
}

TEST_CASE("H1.7: PERIODIC notifications repeat and stop at maxReportNbr") {
    auto& srv = nfLoadServer();
    MockConsumer consumer;
    json sub = nfLoadSub({{"notifMethod", "PERIODIC"}, {"repPeriod", 1}, {"maxReportNbr", 2}});
    const std::string id = createdId(client().Post(SUBS, sub.dump(), "application/json"));

    NwdafNotifier notifier(srv.subs, srv.engine, 1, nullptr, nullptr, srv.cfg);
    notifier.start();
    REQUIRE(consumer.waitFor(2, std::chrono::seconds(10)) >= 2);
    std::this_thread::sleep_for(std::chrono::seconds(3));   // would-be third period
    notifier.stop();

    REQUIRE(consumer.count() == 2);            // maxReportNbr reached …
    REQUIRE_FALSE(srv.subs.exists(id));        // … and the subscription ended
}

TEST_CASE("H1.7: a subscription ends when monDur elapses") {
    auto& srv = nfLoadServer();
    MockConsumer consumer;
    // monDur has one-second resolution: allow for truncation plus the
    // notifier's first poll, so at least one report precedes expiry.
    const auto end = std::chrono::system_clock::now() + std::chrono::seconds(4);
    const auto t = std::chrono::system_clock::to_time_t(end);
    char mon_dur[32];
    std::strftime(mon_dur, sizeof(mon_dur), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    json sub = nfLoadSub({{"notifMethod", "PERIODIC"}, {"repPeriod", 1}, {"monDur", mon_dur}});
    const std::string id = createdId(client().Post(SUBS, sub.dump(), "application/json"));

    NwdafNotifier notifier(srv.subs, srv.engine, 1, nullptr, nullptr, srv.cfg);
    notifier.start();
    std::this_thread::sleep_for(std::chrono::seconds(7));
    notifier.stop();

    REQUIRE(consumer.count() >= 1);            // reported while monitoring …
    REQUIRE_FALSE(srv.subs.exists(id));        // … and ended after monDur
}

TEST_CASE("H1.7: notifications apply the subscription's filters and omit events without data") {
    auto& srv = nfLoadServer();
    MockConsumer consumer;
    // Two NF_LOAD events: UPF (measured) and NRF (no measurement).
    json sub = nfLoadSub({{"notifMethod", "ONE_TIME"}}, {{"nfTypes", {"UPF"}}});
    json nrf = sub["eventSubscriptions"][0];
    nrf["nfTypes"] = {"NRF"};
    sub["eventSubscriptions"].push_back(nrf);
    (void)createdId(client().Post(SUBS, sub.dump(), "application/json"));

    NwdafNotifier notifier(srv.subs, srv.engine, 1, nullptr, nullptr, srv.cfg);
    notifier.start();
    REQUIRE(consumer.waitFor(1, std::chrono::seconds(8)) == 1);
    notifier.stop();

    const json n = consumer.received.front();
    REQUIRE(n["eventNotifications"].size() == 1);   // the NRF event had no data
    const json& infos = n["eventNotifications"][0]["nfLoadLevelInfos"];
    REQUIRE(infos.size() == 1);
    REQUIRE(infos[0]["nfType"] == "UPF");
    REQUIRE_FALSE(n["eventNotifications"][0].contains("failNotifyCode"));
}
#endif

// ── H1.8: transport ─────────────────────────────────────────────────────────

TEST_CASE("H1.8: /health reports the SBI transport profile") {
    (void)server();
    httplib::Client op("127.0.0.1", SBI_PORT);
    auto res = op.Get("/nwdaf-analytics/v1/health");
    REQUIRE(res);
    REQUIRE(json::parse(res->body)["sbiTransportProfile"] ==
            (NwdafHttpClient::http2() ? "rel18-sbi" : "dev-legacy"));
}

#ifdef NWDAF_USE_HTTP2
TEST_CASE("H1.8: the 3GPP interfaces are served over HTTP/2 (h2c prior knowledge)") {
    (void)server();
    auto res = client().Get(std::string(ANALYTICS) + "?event-id=NF_LOAD");
    REQUIRE(res);
    REQUIRE(res->http_version == 2);
    REQUIRE(res->status == 400);
}

TEST_CASE("H1.8: the HTTP/2 listener does not accept HTTP/1.1") {
    (void)server();
    httplib::Client h1("127.0.0.1", H2_PORT);
    h1.set_connection_timeout(2, 0);
    h1.set_read_timeout(2, 0);
    auto res = h1.Get(std::string(ANALYTICS) + "?event-id=NF_LOAD");
    REQUIRE_FALSE(res);
}
#endif
