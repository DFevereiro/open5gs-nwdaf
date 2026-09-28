// ─────────────────────────────────────────────────────────────────────────────
// H1.6 — OpenAPI contract test.
//
// Validates live SBI responses against docs/openapi/nwdaf-analytics-v1.yaml so
// the published contract cannot drift from the implementation: change a
// response shape without updating the spec (or vice versa) and CI fails.
//
// The validator covers the JSON Schema subset the spec actually uses — $ref,
// type, required, properties, items, enum, minimum, maximum — implemented on
// yaml-cpp, which is already a dependency. No new third-party library, in
// keeping with the project's dependency-light constraint.
// ─────────────────────────────────────────────────────────────────────────────
#include <catch2/catch_test_macros.hpp>
#include "nwdaf_server.hpp"
#include "nwdaf_analytics.hpp"
#include "nwdaf_analytics_catalogue.hpp"
#include "nwdaf_schema_validator.hpp"
#include "nwdaf_subscription.hpp"
#include "mock_open5gs.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef NWDAF_OPENAPI_SPEC
#error "NWDAF_OPENAPI_SPEC must point at the OpenAPI document"
#endif

static const int CONF_PORT = 17780;

// ── The operator-API contract, checked with the library validator ─────────────
//
// H1.7: the same NwdafSchemaValidator that validates 3GPP-interface requests
// against the official artifacts. `yaml` is kept for read-only structural
// checks of the document (enums, the analytics-schema mapping).

struct OperatorSpec {
    NwdafSchemaValidator validator;
    YAML::Node           yaml;
    std::string          file;

    explicit OperatorSpec(const std::string& path)
        : validator(std::filesystem::path(path).parent_path().string()),
          yaml(YAML::LoadFile(path)),
          file(std::filesystem::path(path).filename().string()) {}

    // "#/components/schemas/X" within this document → a validator ref.
    std::string ref(const std::string& local) const { return file + local; }
    YAML::Node root() const { return yaml; }
};

// ── Fixture ──────────────────────────────────────────────────────────────────

static NwdafConfig confConfig() {
    NwdafConfig cfg;
    cfg.nf_instance_id = "conformance-uuid";
    cfg.plmn_mcc = "999"; cfg.plmn_mnc = "70";
    cfg.served_snssai_sst = 1; cfg.served_snssai_sd = "000001";
    cfg.served_dnn = "internet";
    cfg.sbi_bind_address = "127.0.0.1"; cfg.sbi_port = CONF_PORT;
    cfg.nf_service_names = {{"AMF","amfd"},{"SMF","smfd"},{"UPF","upfd"}};
    cfg.throughput_interfaces = {"ogstun"};
    cfg.throughput_history_size = 360;
    cfg.collection_interval_seconds = 10;
    cfg.amf_journal_lines = 50; cfg.smf_journal_lines = 50;
    cfg.supi_regex = "imsi-(\\d{15})";
    cfg.mongodb_uri = ""; cfg.mongodb_db = "open5gs";
    cfg.nrf_uri = "http://127.0.0.1:7777"; cfg.nrf_register_on_startup = false;
    cfg.model_dir = "/tmp/nwdaf_conformance_models";
    cfg.anomaly_contamination = 0.10; cfg.anomaly_min_samples = 10;
    cfg.baseline_stddev_min_kbps = 0.5; cfg.ewma_alpha = 0.3;
    cfg.rate_limit_per_ip_rps = 0; cfg.rate_limit_global_rps = 0;  // no throttling here
    cfg.openapi_spec_path = NWDAF_OPENAPI_SPEC;
    cfg.log_level = "warn"; cfg.log_file = "/tmp/nwdaf_conformance.log";
    return cfg;
}

struct ConformanceFixture {
    NwdafConfig            cfg;
    MockNwdafCollector     collector;
    NwdafAnalyticsEngine   engine;
    NwdafSubscriptionStore subs;
    NwdafServer            server;
    std::thread            server_thread;
    OperatorSpec           spec;

    ConformanceFixture()
        : cfg(confConfig()),
          collector(cfg),
          engine(collector, cfg),
          server(engine, subs, cfg),
          spec(NWDAF_OPENAPI_SPEC)
    {
        // Seed a realistic window so analytics return populated payloads
        // rather than only their INSUFFICIENT_DATA branch.
        collector.setNfMetrics({
            {"AMF", "active", 101, 12.0, 51200, 22.0, "LOW"},
            {"SMF", "active", 102, 10.0, 48000, 18.0, "LOW"},
            {"UPF", "active", 103, 30.0, 96000, 41.0, "MEDIUM"},
        });
        collector.setSmfLines({
            "[Established] PDU Session Establishment imsi-999700000000001",
            "[Established] PDU Session Establishment imsi-999700000000002",
            "PDU Session Establishment Reject imsi-999700000000003",
            "[Released] PDU Session Release imsi-999700000000002",
        });
        collector.setAmfLines({
            "Registration complete imsi-999700000000001",
            "Handover required imsi-999700000000002",
            "Authentication failure imsi-999700000000003",
        });
        collector.startBackgroundCollection();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        collector.stopBackgroundCollection();

        for (int i = 0; i < 40; ++i) {
            ThroughputSample s;
            char buf[32];
            snprintf(buf, sizeof(buf), "2026-08-23T11:%02d:00Z", i % 60);
            s.timestamp_iso = buf;
            // A varying series so the anomaly model's variance gate passes.
            s.total_dl_kbps = 400.0 + (i % 7) * 60.0;
            s.total_ul_kbps = 180.0 + (i % 5) * 25.0;
            s.total_dl_bps  = s.total_dl_kbps * 1000.0;
            s.total_ul_bps  = s.total_ul_kbps * 1000.0;
            collector.appendThroughputSample(s);
        }

        server_thread = std::thread([this]{ server.start(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    ~ConformanceFixture() {
        server.stop();
        if (server_thread.joinable()) server_thread.join();
    }
};

static std::unique_ptr<ConformanceFixture> g_conf;
static ConformanceFixture& fixture() {
    if (!g_conf) g_conf = std::make_unique<ConformanceFixture>();
    return *g_conf;
}

static void requireValid(const OperatorSpec& spec, const json& doc,
                         const std::string& local_ref, const std::string& what) {
    auto errs = spec.validator.validate(doc, spec.ref(local_ref));
    if (!errs.empty()) {
        std::ostringstream oss;
        oss << what << " violates the OpenAPI contract:";
        for (const auto& e : errs) oss << "\n  - " << (e.pointer.empty() ? "/" : e.pointer) << ": " << e.reason;
        oss << "\nPayload: " << doc.dump(2);
        FAIL(oss.str());
    }
    SUCCEED();
}

// ── Validator self-test ──────────────────────────────────────────────────────
//
// A contract test is only worth its rejections. These cases prove the
// validator actually fails on each violation class it claims to police, so a
// future refactor cannot quietly turn it into a rubber stamp.

// Writes a schema document to a scratch directory and returns a validator
// rooted there, so the self-tests exercise the real library code path.
static std::unique_ptr<NwdafSchemaValidator> inlineSpec(const std::string& yaml) {
    const auto dir = std::filesystem::temp_directory_path() / "nwdaf_schema_selftest";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "self.yaml") << yaml;
    return std::make_unique<NwdafSchemaValidator>(dir.string());
}

TEST_CASE("H1.6: the schema validator rejects each violation class") {
    auto v = inlineSpec(R"(
type: object
required: [id, level, score, items]
properties:
  id:    { type: string }
  level: { type: string, enum: [LOW, HIGH] }
  score: { type: number, minimum: 0, maximum: 100 }
  count: { type: integer }
  items:
    type: array
    items:
      type: object
      required: [name]
      properties:
        name: { type: string }
)");
    const std::string schema = "self.yaml#";

    const json good = {{"id", "a"}, {"level", "LOW"}, {"score", 50},
                       {"items", json::array({{{"name", "x"}}})}};
    REQUIRE(v->validate(good, schema).empty());

    SECTION("missing required property") {
        json bad = good; bad.erase("level");
        REQUIRE_FALSE(v->validate(bad, schema).empty());
    }
    SECTION("value outside the declared enum") {
        json bad = good; bad["level"] = "MEDIUM";
        REQUIRE_FALSE(v->validate(bad, schema).empty());
    }
    SECTION("number above the declared maximum") {
        json bad = good; bad["score"] = 101;
        REQUIRE_FALSE(v->validate(bad, schema).empty());
    }
    SECTION("number below the declared minimum") {
        json bad = good; bad["score"] = -1;
        REQUIRE_FALSE(v->validate(bad, schema).empty());
    }
    SECTION("wrong scalar type") {
        json bad = good; bad["id"] = 7;
        REQUIRE_FALSE(v->validate(bad, schema).empty());
    }
    SECTION("a non-integer where an integer is declared") {
        json bad = good; bad["count"] = 1.5;
        REQUIRE_FALSE(v->validate(bad, schema).empty());
    }
    SECTION("violation nested inside an array item") {
        json bad = good; bad["items"] = json::array({{{"nome", "typo"}}});
        REQUIRE_FALSE(v->validate(bad, schema).empty());
    }
    SECTION("an integer satisfies a number-typed field") {
        json ok = good; ok["score"] = 42;   // not 42.0
        REQUIRE(v->validate(ok, schema).empty());
    }
}

TEST_CASE("H1.6: the validator resolves $ref repeatably and reports a missing target") {
    auto v = inlineSpec(R"(
components:
  schemas:
    First:  { type: object, required: [a] }
    Second: { type: object, required: [b] }
)");
    for (int i = 0; i < 3; ++i) {
        REQUIRE(v->schema("self.yaml#/components/schemas/First")["required"][0]  == "a");
        REQUIRE(v->schema("self.yaml#/components/schemas/Second")["required"][0] == "b");
    }
    REQUIRE_THROWS(v->schema("self.yaml#/components/schemas/Missing"));
}

TEST_CASE("H1.6: oneOf accepts exactly one valid branch and rejects a document matching none") {
    auto v = inlineSpec(R"(
components:
  schemas:
    Full:
      type: object
      required: [value]
      properties: { value: { type: number } }
    Declined:
      type: object
      required: [reason]
      properties: { reason: { type: string } }
    Either:
      oneOf:
        - $ref: '#/components/schemas/Full'
        - $ref: '#/components/schemas/Declined'
)");
    const std::string either = "self.yaml#/components/schemas/Either";

    REQUIRE(v->validate(json{{"value", 1.5}}, either).empty());
    REQUIRE(v->validate(json{{"reason", "INSUFFICIENT_DATA"}}, either).empty());

    auto errs = v->validate(json{{"unrelated", true}}, either);
    REQUIRE(errs.size() == 1);
    REQUIRE(errs.front().reason.find("none of the oneOf") != std::string::npos);

    // Matching both branches is not "one of".
    REQUIRE_FALSE(v->validate(json{{"value", 1.0}, {"reason", "x"}}, either).empty());
}

// ── The spec itself ──────────────────────────────────────────────────────────

TEST_CASE("H1.6: the OpenAPI document is well-formed and complete") {
    auto& f = fixture();
    YAML::Node root = f.spec.root();
    REQUIRE(root["openapi"].as<std::string>().rfind("3.0", 0) == 0);
    REQUIRE(root["paths"].size() > 0);
    REQUIRE(root["components"]["schemas"].size() > 0);

    // Every registered analytics ID must appear in the published enum and in
    // the analData schema mapping — a new ID cannot ship undocumented.
    std::vector<std::string> enum_ids;
    for (const auto& e : root["components"]["schemas"]["AnalyticsId"]["enum"])
        enum_ids.push_back(e.as<std::string>());

    const YAML::Node mapping = root["x-analyticsDataSchemas"];
    for (const auto& id : NwdafAnalyticsCatalogue::OPERATOR_IDS) {
        INFO("analyticsId=" << id);
        REQUIRE(std::find(enum_ids.begin(), enum_ids.end(), id) != enum_ids.end());
        REQUIRE(mapping[id]);
        // ...and the schema it names must actually exist.
        REQUIRE(f.spec.validator.schema(f.spec.ref(mapping[id].as<std::string>())).is_object());
    }
    // No stale entries either: the enum must not advertise IDs the engine
    // does not serve.
    REQUIRE(enum_ids.size() == NwdafAnalyticsCatalogue::OPERATOR_IDS.size());
}

// ── Analytics responses ──────────────────────────────────────────────────────

TEST_CASE("H1.6: GET /analytics conforms for every analytics ID") {
    auto& f = fixture();
    httplib::Client cli("127.0.0.1", CONF_PORT);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(5);

    const std::string envelope = "#/components/schemas/AnalyticsResponse";
    YAML::Node mapping  = f.spec.root()["x-analyticsDataSchemas"];

    for (const auto& id : NwdafAnalyticsCatalogue::OPERATOR_IDS) {
        INFO("analyticsId=" << id);
        auto res = cli.Get("/nwdaf-analytics/v1/analytics?analyticsId=" + id);
        REQUIRE(res);
        REQUIRE(res->status == 200);

        json body = json::parse(res->body);
        requireValid(f.spec, body, envelope, "GET /analytics envelope for " + id);
        requireValid(f.spec, body["analData"],
                     mapping[id].as<std::string>(),
                     "analData for " + id);
        // The envelope's id must match what was asked for.
        REQUIRE(body["analyticsId"] == id);
    }
}

TEST_CASE("H1.6: POST /nnwdaf-analyticsinfo conforms for every analytics ID") {
    auto& f = fixture();
    httplib::Client cli("127.0.0.1", CONF_PORT);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(5);

    const std::string envelope = "#/components/schemas/AnalyticsResponse";
    YAML::Node mapping  = f.spec.root()["x-analyticsDataSchemas"];

    for (const auto& id : NwdafAnalyticsCatalogue::OPERATOR_IDS) {
        INFO("analyticsId=" << id);
        json req = {{"analyticsId", id},
                    {"dnn", "internet"},
                    {"snssai", {{"sst", 1}, {"sd", "000001"}}}};
        auto res = cli.Post("/nnwdaf-analyticsinfo/v1/analytics",
                            req.dump(), "application/json");
        REQUIRE(res);
        REQUIRE(res->status == 200);

        json body = json::parse(res->body);
        requireValid(f.spec, body, envelope, "POST analytics envelope for " + id);
        requireValid(f.spec, body["analData"],
                     mapping[id].as<std::string>(),
                     "analData for " + id);
    }
}

// ── Operational endpoints ────────────────────────────────────────────────────

TEST_CASE("H1.6: /health conforms and advertises the live catalogue") {
    auto& f = fixture();
    httplib::Client cli("127.0.0.1", CONF_PORT);
    cli.set_connection_timeout(5);
    auto res = cli.Get("/nwdaf-analytics/v1/health");
    REQUIRE(res);
    REQUIRE(res->status == 200);

    json body = json::parse(res->body);
    requireValid(f.spec, body,
                 "#/components/schemas/HealthResponse",
                 "GET /health");

    // The advertised list must be exactly what the engine serves.
    auto ids = body["nfProfile"]["nwdafInfo"]["analyticsIds"]
                   .get<std::vector<std::string>>();
    REQUIRE(ids.size() == NwdafAnalyticsCatalogue::OPERATOR_IDS.size());
    for (const auto& id : ids)
        REQUIRE(NwdafAnalyticsCatalogue::OPERATOR_IDS.count(id) == 1);
}

TEST_CASE("H1.6: /ready conforms in both states") {
    auto& f = fixture();
    httplib::Client cli("127.0.0.1", CONF_PORT);
    cli.set_connection_timeout(5);
    auto res = cli.Get("/nwdaf-analytics/v1/ready");
    REQUIRE(res);
    REQUIRE((res->status == 200 || res->status == 503));
    requireValid(f.spec, json::parse(res->body),
                 "#/components/schemas/ReadyResponse",
                 "GET /ready");
}

TEST_CASE("H1.6: POST /train conforms") {
    auto& f = fixture();
    httplib::Client cli("127.0.0.1", CONF_PORT);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);
    auto res = cli.Post("/nwdaf-analytics/v1/train", "", "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    requireValid(f.spec, json::parse(res->body),
                 "#/components/schemas/TrainResponse",
                 "POST /train");
}

TEST_CASE("H1.6: the subscription lifecycle conforms") {
    auto& f = fixture();
    httplib::Client cli("127.0.0.1", CONF_PORT);
    cli.set_connection_timeout(5);

    const std::string sub_schema = "#/components/schemas/Subscription";

    json req = {{"analyticsId", "SM_CONGESTION"},
                {"notifUri", "http://127.0.0.1:9/notify"}};
    auto created = cli.Post("/nwdaf-analytics/v1/subscriptions",
                            req.dump(), "application/json");
    REQUIRE(created);
    REQUIRE(created->status == 201);
    json sub = json::parse(created->body);
    requireValid(f.spec, sub, sub_schema, "POST /subscriptions");

    const std::string sub_id = sub["subId"].get<std::string>();

    auto fetched = cli.Get(("/nwdaf-analytics/v1/subscriptions/" + sub_id).c_str());
    REQUIRE(fetched);
    REQUIRE(fetched->status == 200);
    requireValid(f.spec, json::parse(fetched->body), sub_schema, "GET /subscriptions/{id}");

    auto listed = cli.Get("/nwdaf-analytics/v1/subscriptions");
    REQUIRE(listed);
    REQUIRE(listed->status == 200);
    json arr = json::parse(listed->body);
    REQUIRE(arr.is_array());
    for (const auto& item : arr)
        requireValid(f.spec, item, sub_schema, "GET /subscriptions item");

    auto removed = cli.Delete(("/nwdaf-analytics/v1/subscriptions/" + sub_id).c_str());
    REQUIRE(removed);
    REQUIRE(removed->status == 204);
}

TEST_CASE("H1.6: the SBI serves the published OpenAPI document") {
    auto& f = fixture();
    httplib::Client cli("127.0.0.1", CONF_PORT);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(5);

    auto res = cli.Get("/nwdaf-analytics/v1/openapi");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    REQUIRE(res->get_header_value("Content-Type") == "application/yaml");

    // What is served must be the same contract the tests above validate
    // against — not a stale copy that drifted from the repository.
    const YAML::Node served = YAML::Load(res->body);
    REQUIRE(served["openapi"].as<std::string>()
            == f.spec.root()["openapi"].as<std::string>());
    REQUIRE(served["info"]["version"].as<std::string>()
            == f.spec.root()["info"]["version"].as<std::string>());
    REQUIRE(served["components"]["schemas"].size()
            == f.spec.root()["components"]["schemas"].size());
    REQUIRE(served["x-analyticsDataSchemas"].size()
            == NwdafAnalyticsCatalogue::OPERATOR_IDS.size());
}

TEST_CASE("H1.6: a missing OpenAPI document degrades to 404, not a crash") {
    // Optional-dependency convention: an absent file must not take the SBI
    // down or return a 500.
    NwdafConfig cfg = confConfig();
    cfg.sbi_port = CONF_PORT + 1;
    cfg.openapi_spec_path = "/nonexistent/nwdaf-openapi-should-not-exist.yaml";

    MockNwdafCollector     collector(cfg);
    NwdafAnalyticsEngine   engine(collector, cfg);
    NwdafSubscriptionStore subs;
    NwdafServer            server(engine, subs, cfg);
    std::thread t([&]{ server.start(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    httplib::Client cli("127.0.0.1", cfg.sbi_port);
    cli.set_connection_timeout(5);
    auto res = cli.Get("/nwdaf-analytics/v1/openapi");
    REQUIRE(res);
    REQUIRE(res->status == 404);

    // The rest of the SBI keeps working.
    auto health = cli.Get("/nwdaf-analytics/v1/health");
    REQUIRE(health);
    REQUIRE(health->status == 200);

    server.stop();
    if (t.joinable()) t.join();
}

// ── Error responses ──────────────────────────────────────────────────────────

TEST_CASE("H1.6: error responses are RFC 7807 problem documents") {
    auto& f = fixture();
    httplib::Client cli("127.0.0.1", CONF_PORT);
    cli.set_connection_timeout(5);

    const std::string problem = "#/components/schemas/ProblemDetails";

    struct Case { const char* url; int status; const char* what; };
    const Case cases[] = {
        {"/nwdaf-analytics/v1/analytics",                          400, "missing analyticsId"},
        {"/nwdaf-analytics/v1/analytics?analyticsId=NOT_AN_ID",    422, "unknown analyticsId"},
        {"/nwdaf-analytics/v1/analytics?analyticsId=NF_LOAD&supi=imsi-1", 400, "supi on NF_LOAD"},
        {"/nwdaf-analytics/v1/subscriptions/does-not-exist",       404, "missing subscription"},
    };
    for (const auto& c : cases) {
        INFO(c.what);
        auto res = cli.Get(c.url);
        REQUIRE(res);
        REQUIRE(res->status == c.status);
        json body = json::parse(res->body);
        requireValid(f.spec, body, problem, std::string("error: ") + c.what);
        REQUIRE(body["status"].get<int>() == c.status);
    }
}

TEST_CASE("H1.6: legacy analyticsId spellings are accepted on the operator API") {
    auto& f = fixture();
    httplib::Client cli("127.0.0.1", CONF_PORT);
    cli.set_connection_timeout(5);
    const std::vector<std::pair<std::string, std::string>> aliases = {
        {"QoS_SUSTAINABILITY",     "QOS_SUSTAINABILITY"},
        {"REDUNDANT_TRANSMISSION", "RED_TRANS_EXP"},
    };
    for (const auto& [legacy, canonical] : aliases) {
        INFO("legacy=" << legacy);
        auto res = cli.Get("/nwdaf-analytics/v1/analytics?analyticsId=" + legacy);
        REQUIRE(res);
        REQUIRE(res->status == 200);
        json body = json::parse(res->body);
        // Normalised to the Rel-18 spelling the spec's enum declares.
        REQUIRE(body["analyticsId"] == canonical);
        requireValid(f.spec, body,
                     "#/components/schemas/AnalyticsResponse",
                     legacy + " alias");
    }
}
