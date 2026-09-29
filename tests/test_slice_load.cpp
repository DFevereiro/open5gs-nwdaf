// H1.2 — SLICE_LOAD_LEVEL / NSI_LOAD_LEVEL from the per-slice OAM counts of
// Open5GS (TS 23.288 V18.13.0 §6.3), load level per I-9, outputs checked
// against the official TS 29.520 V18.14.0 schemas.
#include <catch2/catch_test_macros.hpp>
#include "mock_open5gs.hpp"
#include "nwdaf_3gpp_adapter.hpp"
#include "nwdaf_analytics.hpp"
#include "nwdaf_analytics_catalogue.hpp"
#include "nwdaf_sbi.hpp"
#include "nwdaf_schema_validator.hpp"
#include "nwdaf_slice_load.hpp"
#include "nwdaf_subscription.hpp"
#include "nwdaf_supported_features.hpp"
#include <fstream>
#include <thread>

using json = nlohmann::json;
using Clock = std::chrono::system_clock;

static const char* AMF_URL = "http://127.0.0.5:9090/metrics";
static const char* SMF_URL = "http://127.0.0.4:9090/metrics";

static NwdafSliceCapacity slice(int sst, const std::string& sd, long max_ues, long max_pdu) {
    NwdafSliceCapacity c;
    c.sst = sst; c.sd = sd; c.max_ues = max_ues; c.max_pdu_sessions = max_pdu;
    return c;
}

static NwdafPromSample ues(const std::string& snssai, double v, const std::string& plmn = "99970") {
    return {"fivegs_amffunction_rm_registeredsubnbr", {{"plmnid", plmn}, {"snssai", snssai}}, v};
}
static NwdafPromSample pdus(const std::string& snssai, double v, const std::string& plmn = "99970") {
    return {"fivegs_smffunction_sm_sessionnbr", {{"plmnid", plmn}, {"snssai", snssai}}, v};
}

// ── I-9 arithmetic ──────────────────────────────────────────────────────────

TEST_CASE("H1.2: slice load level is the higher of UE and PDU-session occupancy (I-9)") {
    const auto t0 = Clock::now() - std::chrono::minutes(5);
    const std::vector<NwdafOamScrape> amf = {{t0, {ues("1-000001", 4)}},
                                             {t0 + std::chrono::seconds(10), {ues("1-000001", 6)}}};
    const std::vector<NwdafOamScrape> smf = {{t0, {pdus("1-000001", 2)}},
                                             {t0 + std::chrono::seconds(10), {pdus("1-000001", 2)}}};
    const auto l = NwdafSliceLoadCalculator::compute(slice(1, "000001", 10, 20), "99970", amf, smf,
                                                     Clock::time_point{}, Clock::now());
    REQUIRE(l);
    REQUIRE(l->ues->average == 5);
    REQUIRE(l->ues->variance == 1);
    REQUIRE(l->pdu_sessions->average == 2);
    REQUIRE(l->load_level == 50);   // max(5/10, 2/20) = 50 %
}

TEST_CASE("H1.2: slice counts — absent series, other slices and PLMNs, clamping") {
    const auto t0 = Clock::now() - std::chrono::minutes(1);
    // A scrape without the series counts as 0; other slices and PLMNs are not counted.
    const std::vector<NwdafOamScrape> smf = {
        {t0, {pdus("2", 30), pdus("1-000001", 99), pdus("2", 99, "00101")}},
        {t0 + std::chrono::seconds(10), {}}};
    auto l = NwdafSliceLoadCalculator::compute(slice(2, "", 0, 20), "99970", {}, smf,
                                               Clock::time_point{}, Clock::now());
    REQUIRE(l);
    REQUIRE(l->pdu_sessions->average == 15);
    REQUIRE_FALSE(l->ues);   // max_ues not configured: not a dimension
    REQUIRE(l->load_level == 75);

    // Open5GS does not enforce the capacity: counts above it clamp to 100.
    l = NwdafSliceLoadCalculator::compute(slice(2, "", 0, 10), "99970", {}, smf,
                                          Clock::time_point{}, Clock::now());
    REQUIRE(l->load_level == 100);
}

TEST_CASE("H1.2: a configured dimension without samples in the period gives no load level") {
    const auto t0 = Clock::now() - std::chrono::minutes(10);
    const std::vector<NwdafOamScrape> amf = {{t0, {ues("1", 1)}}};
    // Only UEs are sampled, but both dimensions are configured.
    REQUIRE_FALSE(NwdafSliceLoadCalculator::compute(slice(1, "", 10, 10), "99970", amf, {},
                                                    Clock::time_point{}, Clock::now()));
    // The period excludes the only UE sample.
    REQUIRE_FALSE(NwdafSliceLoadCalculator::compute(slice(1, "", 10, 0), "99970", amf, {},
                                                    t0 + std::chrono::minutes(1), Clock::now()));
}

// ── Configuration and advertisement ─────────────────────────────────────────

static std::string writeConfig(const std::string& body) {
    const std::string path = "/tmp/nwdaf_slice_cfg_test.yaml";
    std::ofstream(path) << "nwdaf:\n"
                           "  nf_instance_id: \"7c1f4e2a-9b3d-4a6e-8f10-2d3c4b5a6e7f\"\n" << body;
    return path;
}

TEST_CASE("H1.2: slice_capacity is validated at load") {
    const std::string endpoints =
        "  oam_metrics_endpoints: {AMF: \"http://127.0.0.5:9090/metrics\", SMF: \"http://127.0.0.4:9090/metrics\"}\n";
    auto cfg = NwdafConfig::load(writeConfig(endpoints +
        "  slice_capacity:\n"
        "    - {snssai: {sst: 1, sd: \"00000A\"}, max_ues: 100, max_pdu_sessions: 200}\n"
        "    - {snssai: {sst: 2}, max_pdu_sessions: 5}\n"));
    REQUIRE(cfg.slice_capacity.size() == 2);
    REQUIRE(cfg.slice_capacity[0].key() == "1-00000a");   // Open5GS labels in lower case
    REQUIRE(cfg.slice_capacity[1].key() == "2");

    REQUIRE_THROWS(NwdafConfig::load(writeConfig(endpoints +
        "  slice_capacity: [{snssai: {sst: 1}}]\n")));                        // no maximum
    REQUIRE_THROWS(NwdafConfig::load(writeConfig(endpoints +
        "  slice_capacity: [{snssai: {sst: 1, sd: \"xyz\"}, max_ues: 1}]\n")));
    REQUIRE_THROWS(NwdafConfig::load(writeConfig(endpoints +
        "  slice_capacity: [{snssai: {sst: 1}, max_ues: 1}, {snssai: {sst: 1}, max_ues: 2}]\n")));
    REQUIRE_THROWS(NwdafConfig::load(writeConfig(                              // no AMF endpoint
        "  slice_capacity: [{snssai: {sst: 1}, max_ues: 1}]\n")));
}

static NwdafConfig sliceConfig() {
    NwdafConfig cfg;
    cfg.nf_instance_id = "7c1f4e2a-9b3d-4a6e-8f10-2d3c4b5a6e7f";
    cfg.plmn_mcc = "999"; cfg.plmn_mnc = "70";
    cfg.nf_service_names = {{"AMF", "amfd"}, {"SMF", "smfd"}};
    cfg.collection_interval_seconds = 1;
    cfg.throughput_history_size = 10;
    cfg.oam_metrics_endpoints = {{"AMF", AMF_URL}, {"SMF", SMF_URL}};
    cfg.slice_capacity = {slice(1, "000001", 10, 20), slice(2, "", 0, 5)};
    cfg.openapi_3gpp_dir = NWDAF_3GPP_OPENAPI_DIR;
    cfg.model_dir = "/tmp/nwdaf_slice_models";
    cfg.log_level = "warn"; cfg.log_file = "/tmp/nwdaf_slice_test.log";
    return cfg;
}

TEST_CASE("H1.2: slice load IDs are advertised only with a configured slice capacity") {
    NwdafConfig cfg = sliceConfig();
    auto adv = NwdafAnalyticsCatalogue::rel18Advertised(cfg);
    REQUIRE(adv.count("SLICE_LOAD_LEVEL") == 1);
    REQUIRE(adv.count("NSI_LOAD_LEVEL") == 1);
    // NsiLoad is feature 9 in both APIs; slice load level itself is base functionality.
    REQUIRE(NwdafSupportedFeatures::local(NnwdafApi::AnalyticsInfo, cfg).toHex() == "100");
    REQUIRE(NwdafSupportedFeatures::local(NnwdafApi::EventsSubscription, cfg).toHex() == "100");
    REQUIRE(NwdafAnalyticsCatalogue::fromAnalyticsInfoEventId("LOAD_LEVEL_INFORMATION") == "SLICE_LOAD_LEVEL");

    cfg.slice_capacity.clear();
    adv = NwdafAnalyticsCatalogue::rel18Advertised(cfg);
    REQUIRE(adv.count("SLICE_LOAD_LEVEL") == 0);
    REQUIRE(adv.count("NSI_LOAD_LEVEL") == 0);
}

// ── The 3GPP interfaces ─────────────────────────────────────────────────────

static NwdafSchemaValidator& official() {
    static NwdafSchemaValidator v(NWDAF_3GPP_OPENAPI_DIR);
    return v;
}

static void requireSchema(const json& body, const std::string& ref) {
    const auto v = official().validate(body, ref);
    INFO(body.dump(2));
    INFO((v.empty() ? std::string() : v.front().pointer + " " + v.front().reason));
    REQUIRE(v.empty());
}

// A service over a mock collector that has scraped the NFs twice:
// slice 1-000001: UEs 4 then 6 (of 10), PDU sessions 2 then 2 (of 20) → 50;
// slice 2: PDU sessions 1 then 3 (of 5) → 40.
struct SliceFixture {
    NwdafConfig            cfg;
    MockNwdafCollector     collector;
    NwdafAnalyticsEngine   engine;
    NwdafSubscriptionStore subs;
    NwdafSbiService        sbi;

    explicit SliceFixture(NwdafConfig c = sliceConfig())
        : cfg(std::move(c)), collector(cfg), engine(collector, cfg), sbi(engine, subs, cfg) {
        scrape(4, 2, 1);
        scrape(6, 2, 3);
    }
    void scrape(int ues1, int pdu1, int pdu2) {
        collector.setOamMetrics(AMF_URL, "fivegs_amffunction_rm_registeredsubnbr{plmnid=\"99970\",snssai=\"1-000001\"} " +
                                         std::to_string(ues1) + "\n");
        collector.setOamMetrics(SMF_URL, "fivegs_smffunction_sm_sessionnbr{plmnid=\"99970\",snssai=\"1-000001\"} " +
                                         std::to_string(pdu1) + "\n"
                                         "fivegs_smffunction_sm_sessionnbr{plmnid=\"99970\",snssai=\"2\"} " +
                                         std::to_string(pdu2) + "\n");
        collector.startBackgroundCollection();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        collector.stopBackgroundCollection();
    }
    SbiResponse get(std::multimap<std::string, std::string> query) {
        SbiRequest r;
        r.method = "GET";
        r.path = std::string(NwdafSbiService::ANALYTICS_INFO_ROOT) + "/analytics";
        r.query = std::move(query);
        r.api_root = "http://127.0.0.1:7780";
        return sbi.dispatch(r);
    }
    SbiResponse subscribe(const json& body) {
        SbiRequest r;
        r.method = "POST";
        r.path = std::string(NwdafSbiService::EVENTS_SUBSCRIPTION_ROOT) + "/subscriptions";
        r.headers = {{"content-type", "application/json"}};
        r.body = body.dump();
        r.api_root = "http://127.0.0.1:7780";
        return sbi.dispatch(r);
    }
};

static json requireAnalyticsData(const SbiResponse& res) {
    INFO(res.body);
    REQUIRE(res.status == 200);
    const json body = json::parse(res.body);
    requireSchema(body, "TS29520_Nnwdaf_AnalyticsInfo.yaml#/components/schemas/AnalyticsData");
    return body;
}

static int problemStatus(const SbiResponse& res, const std::string& cause) {
    INFO(res.body);
    if (!res.body.empty()) REQUIRE(json::parse(res.body)["cause"] == cause);
    return res.status;
}

TEST_CASE("H1.2: LOAD_LEVEL_INFORMATION answers sliceLoadLevelInfos per configured slice") {
    SliceFixture f;
    const json body = requireAnalyticsData(
        f.get({{"event-id", "LOAD_LEVEL_INFORMATION"}, {"event-filter", R"({"anySlice":true})"}}));
    REQUIRE(body["sliceLoadLevelInfos"] == json::array({
        {{"loadLevelInformation", 50}, {"snssais", {{{"sst", 1}, {"sd", "000001"}}}}},
        {{"loadLevelInformation", 40}, {"snssais", {{{"sst", 2}}}}}}));

    // One slice by S-NSSAI; the SD matches regardless of case.
    const json one = requireAnalyticsData(
        f.get({{"event-id", "LOAD_LEVEL_INFORMATION"},
               {"event-filter", R"({"snssais":[{"sst":1,"sd":"000001"}]})"}}));
    REQUIRE(one["sliceLoadLevelInfos"].size() == 1);
}

TEST_CASE("H1.2: NSI_LOAD_LEVEL answers nsiLoadLevelInfos at S-NSSAI level") {
    SliceFixture f;
    const json body = requireAnalyticsData(
        f.get({{"event-id", "NSI_LOAD_LEVEL"},
               {"event-filter", R"({"nsiIdInfos":[{"snssai":{"sst":2}}]})"},
               {"supported-features", "100"}}));
    REQUIRE(body["nsiLoadLevelInfos"] == json::array({{{"loadLevelInformation", 40}, {"snssai", {{"sst", 2}}}}}));
    REQUIRE(body["suppFeat"] == "100");   // NsiLoad negotiated
}

TEST_CASE("H1.2: slice load request rules and failure semantics") {
    SliceFixture f;
    // §4.3.2.2: the slices are mandatory.
    REQUIRE(problemStatus(f.get({{"event-id", "LOAD_LEVEL_INFORMATION"}}), "MANDATORY_QUERY_PARAM_MISSING") == 400);
    REQUIRE(problemStatus(f.get({{"event-id", "LOAD_LEVEL_INFORMATION"},
                                 {"event-filter", R"({"anySlice":false})"}}), "MANDATORY_QUERY_PARAM_INCORRECT") == 400);
    // No network slice instances in Open5GS.
    REQUIRE(problemStatus(f.get({{"event-id", "NSI_LOAD_LEVEL"},
                                 {"event-filter", R"({"nsiIdInfos":[{"snssai":{"sst":1,"sd":"000001"},"nsiIds":["nsi-1"]}]})"}}),
                          "OPTIONAL_QUERY_PARAM_INCORRECT") == 400);
    // A slice without a configured capacity has no load level: no data.
    REQUIRE(f.get({{"event-id", "LOAD_LEVEL_INFORMATION"},
                   {"event-filter", R"({"snssais":[{"sst":3}]})"}}).status == 204);
    // Predictions are not supported; statistics before the held history are unavailable.
    REQUIRE(problemStatus(f.get({{"event-id", "LOAD_LEVEL_INFORMATION"}, {"event-filter", R"({"anySlice":true})"},
                                 {"ana-req", R"({"startTs":"2099-01-01T00:00:00Z"})"}}),
                          "OPTIONAL_QUERY_PARAM_INCORRECT") == 400);
    REQUIRE(problemStatus(f.get({{"event-id", "LOAD_LEVEL_INFORMATION"}, {"event-filter", R"({"anySlice":true})"},
                                 {"ana-req", R"({"startTs":"2020-01-01T00:00:00Z"})"}}),
                          "UNAVAILABLE_DATA") == 500);
}

TEST_CASE("H1.2: without a configured slice capacity the slice load IDs are not served") {
    NwdafConfig cfg = sliceConfig();
    cfg.slice_capacity.clear();
    SliceFixture f(cfg);
    REQUIRE(problemStatus(f.get({{"event-id", "LOAD_LEVEL_INFORMATION"}, {"event-filter", R"({"anySlice":true})"}}),
                          "MANDATORY_QUERY_PARAM_INCORRECT") == 400);
}

TEST_CASE("H1.2: SLICE_LOAD_LEVEL and NSI_LOAD_LEVEL subscriptions report immediately") {
    SliceFixture f;
    const json sub = {
        {"notificationURI", "http://127.0.0.1:9/cb"},
        {"evtReq", {{"immRep", true}}},
        {"eventSubscriptions", {
            {{"event", "SLICE_LOAD_LEVEL"}, {"anySlice", true},
             {"notificationMethod", "PERIODIC"}, {"repetitionPeriod", 60}},
            {{"event", "NSI_LOAD_LEVEL"}, {"nsiIdInfos", {{{"snssai", {{"sst", 1}, {"sd", "000001"}}}}}},
             {"notificationMethod", "PERIODIC"}, {"repetitionPeriod", 60}}}},
        {"supportedFeatures", "100"}};
    const SbiResponse res = f.subscribe(sub);
    INFO(res.body);
    REQUIRE(res.status == 201);
    const json body = json::parse(res.body);
    requireSchema(body, "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/NnwdafEventsSubscription");
    REQUIRE_FALSE(body.contains("failEventReports"));
    // SLICE_LOAD_LEVEL: one notification per distinct level (50, 40); NSI_LOAD_LEVEL: one.
    const json& n = body["eventNotifications"];
    REQUIRE(n.size() == 3);
    for (const auto& e : n)
        requireSchema(e, "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/EventNotification");
    REQUIRE(n[0]["sliceLoadLevelInfo"]["loadLevelInformation"] == 40);
    REQUIRE(n[1]["sliceLoadLevelInfo"]["loadLevelInformation"] == 50);
    REQUIRE(n[2]["nsiLoadLevelInfos"][0]["loadLevelInformation"] == 50);
}

TEST_CASE("H1.2: THRESHOLD slice load subscriptions are not supported yet") {
    SliceFixture f;
    // The default method is THRESHOLD, which needs loadLevelThreshold.
    const json sub = {
        {"notificationURI", "http://127.0.0.1:9/cb"},
        {"eventSubscriptions", {
            {{"event", "SLICE_LOAD_LEVEL"}, {"anySlice", true}, {"loadLevelThreshold", 80}},
            {{"event", "NSI_LOAD_LEVEL"}, {"anySlice", true},
             {"notificationMethod", "PERIODIC"}, {"repetitionPeriod", 60}}}}};
    const SbiResponse res = f.subscribe(sub);
    INFO(res.body);
    REQUIRE(res.status == 201);
    const json body = json::parse(res.body);
    REQUIRE(body["failEventReports"] == json::array({{{"event", "SLICE_LOAD_LEVEL"}, {"failureCode", "OTHER"}}}));
    REQUIRE(body["eventSubscriptions"].size() == 1);
}

TEST_CASE("H1.2: without a target period, statistics cover the last slice_load_window_seconds") {
    const auto now = Clock::now();
    NwdafConfig cfg = sliceConfig();
    cfg.slice_load_window_seconds = 60;
    // An old scrape (0 PDU sessions) and a recent one (4 of 5): only the recent counts.
    const std::vector<NwdafOamScrape> smf = {{now - std::chrono::minutes(10), {}},
                                             {now - std::chrono::seconds(10), {pdus("2", 4)}}};
    Nwdaf3gppAdapter::SliceQuery q;
    q.keys = {"2"};
    const auto loads = Nwdaf3gppAdapter::sliceLoads(cfg, q, {}, smf, now);
    REQUIRE(loads.size() == 1);
    REQUIRE(loads[0].load_level == 80);
    // An explicit start includes the old scrape: (0 + 4) / 2 of 5 = 40.
    q.from = now - std::chrono::minutes(15);
    REQUIRE(Nwdaf3gppAdapter::sliceLoads(cfg, q, {}, smf, now)[0].load_level == 40);
}
