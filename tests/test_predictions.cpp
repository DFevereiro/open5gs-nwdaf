// H1.7 — predictions on the 3GPP interfaces (interpretation I-13): Holt's
// linear-trend forecast with a probability confidence, for NF_LOAD and
// NSI_LOAD_LEVEL, outputs checked against the official TS 29.520 schemas.
#include <catch2/catch_test_macros.hpp>
#include "official_schema.hpp"
#include "mock_open5gs.hpp"
#include "ml/holt_forecaster.hpp"
#include "nwdaf_3gpp_adapter.hpp"
#include "nwdaf_analytics.hpp"
#include "nwdaf_sbi.hpp"
#include "nwdaf_subscription.hpp"
#include <cmath>
#include <thread>

using json = nlohmann::json;
using Clock = std::chrono::system_clock;
using std::chrono::seconds;

static const char* AMF_URL = "http://127.0.0.5:9090/metrics";
static const char* AMF_ID = "11111111-1111-4111-8111-111111111111";

// ── The forecaster ──────────────────────────────────────────────────────────

static std::vector<std::pair<double, double>> series(std::vector<double> v, double step = 10) {
    std::vector<std::pair<double, double>> s;
    for (size_t i = 0; i < v.size(); ++i) s.push_back({1000 + step * i, v[i]});
    return s;
}

TEST_CASE("H1.7: the forecast follows level and trend; confidence is a probability (I-13)") {
    HoltForecaster::Params p;   // alpha 0.3, beta 0.1, 10 s steps, tolerance 10, 10 samples
    // A flat series: its level, with certainty.
    auto f = HoltForecaster::forecast(series(std::vector<double>(12, 40)), 1200, 1300, p);
    REQUIRE(f);
    REQUIRE(std::abs(f->value - 40) < 1e-9);
    REQUIRE(f->confidence == 100);
    // A steady rise of 1 per step: extrapolated to the period's midpoint.
    std::vector<double> rise;
    for (int i = 0; i < 12; ++i) rise.push_back(20 + i);
    f = HoltForecaster::forecast(series(rise), 1110 + 50, 1110 + 150, p);   // 10 steps after the last (31)
    REQUIRE(std::abs(f->value - 41) < 1e-6);
    // Clamped to 0–100.
    f = HoltForecaster::forecast(series(rise), 1110 + 10000, 1110 + 10000, p);
    REQUIRE(f->value == 100);
    // Too few samples: zero confidence (TS 29.520 Table 5.1.6.2.11-1 NOTE 1).
    f = HoltForecaster::forecast(series({50, 52, 51}), 1100, 1100, p);
    REQUIRE(f->confidence == 0);
    REQUIRE_FALSE(HoltForecaster::forecast({}, 0, 0, p));
}

TEST_CASE("H1.7: a noisy series gives lower confidence, and lower further ahead (I-13)") {
    HoltForecaster::Params p;
    std::vector<double> noisy;
    for (int i = 0; i < 30; ++i) noisy.push_back(50 + (i % 2 ? 8 : -8));
    const double last = 1000 + 10 * 29;
    const auto near = HoltForecaster::forecast(series(noisy), last + 10, last + 10, p);
    const auto far  = HoltForecaster::forecast(series(noisy), last + 600, last + 600, p);
    REQUIRE(near->confidence > 0);
    REQUIRE(near->confidence < 100);
    REQUIRE(far->confidence < near->confidence);
}

// ── Output mappings ─────────────────────────────────────────────────────────

static NwdafConfig predConfig() {
    NwdafConfig cfg;
    cfg.nf_instance_id = "7c1f4e2a-9b3d-4a6e-8f10-2d3c4b5a6e7f";
    cfg.plmn_mcc = "999"; cfg.plmn_mnc = "70";
    cfg.nf_service_names = {{"AMF", "amfd"}, {"SMF", "smfd"}};
    cfg.nf_instance_ids = {{"AMF", AMF_ID}};
    cfg.collection_interval_seconds = 1;
    cfg.throughput_history_size = 50;
    cfg.oam_metrics_endpoints = {{"AMF", AMF_URL}};
    cfg.slice_capacity = {NwdafSliceCapacity{1, "", 10, 0}};
    cfg.openapi_3gpp_dir = NWDAF_3GPP_OPENAPI_DIR;
    cfg.model_dir = "/tmp/nwdaf_pred_models";
    cfg.log_level = "warn"; cfg.log_file = "/tmp/nwdaf_pred_test.log";
    return cfg;
}

TEST_CASE("H1.7: NF_LOAD predictions are NfLoadLevelInformation with nfCpuUsage and confidence") {
    const NwdafConfig cfg = predConfig();
    const auto t0 = Clock::now() - seconds(20);
    std::vector<NwdafNfLoadScrape> history;
    for (int i = 0; i < 20; ++i)   // a flat 30 % CPU, one sample a second
        history.push_back({t0 + seconds(i), {{"AMF", "active", 101, 1.0, 1000, 30.0, "LOW"},
                                             {"SMF", "active", 102, 1.0, 1000, 70.0, "HIGH"}}});
    Nwdaf3gppAdapter::NfLoadQuery q;
    q.prediction = true;
    q.from = Clock::now() + seconds(60);
    q.to = Clock::now() + seconds(120);
    const json infos = Nwdaf3gppAdapter::nfLoadPredictions(cfg, history, cfg.nf_instance_ids, q, Clock::now());
    REQUIRE(infos.size() == 1);   // the SMF has no instance ID
    requireOfficialSchema(infos[0], "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/NfLoadLevelInformation");
    REQUIRE(infos[0]["nfInstanceId"] == AMF_ID);
    REQUIRE(infos[0]["nfCpuUsage"] == 30);
    REQUIRE(infos[0]["confidence"] == 100);
}

TEST_CASE("H1.7: NSI_LOAD_LEVEL predictions follow the slice's load level per scrape") {
    const NwdafConfig cfg = predConfig();
    const auto t0 = Clock::now() - seconds(20);
    std::vector<NwdafOamScrape> amf;
    for (int i = 0; i < 12; ++i)   // 1, 2, … 12 UEs of 10 would clamp: rise by half a UE a second
        amf.push_back({t0 + seconds(i), {{"fivegs_amffunction_rm_registeredsubnbr",
                                          {{"plmnid", "99970"}, {"snssai", "1"}}, 2.0 + 0.5 * i}}});
    Nwdaf3gppAdapter::SliceQuery q;
    q.any = true;
    q.prediction = true;
    q.from = q.to = amf.back().at + seconds(4);
    const json infos = Nwdaf3gppAdapter::nsiLoadPredictions(cfg, q, amf, {}, Clock::now());
    REQUIRE(infos.size() == 1);
    requireOfficialSchema(infos[0], "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/NsiLoadLevelInfo");
    // Last level 75 % (7.5 of 10 UEs), rising 5 points a second: about 95 four seconds on.
    REQUIRE(infos[0]["loadLevelInformation"].get<int>() >= 90);
    REQUIRE(infos[0]["snssai"] == json{{"sst", 1}});
    REQUIRE(infos[0].contains("confidence"));
}

// ── The 3GPP interfaces ─────────────────────────────────────────────────────

// One collection tick: the AMF at 40 % CPU, 5 UEs on slice 1 (level 50).
struct PredFixture {
    NwdafConfig            cfg;
    MockNwdafCollector     collector;
    NwdafAnalyticsEngine   engine;
    NwdafSubscriptionStore subs;
    NwdafSbiService        sbi;

    explicit PredFixture(NwdafConfig c = predConfig())
        : cfg(std::move(c)), collector(cfg), engine(collector, cfg), sbi(engine, subs, cfg) {
        collector.setNfMetrics({{"AMF", "active", 101, 1.0, 1000, 40.0, "LOW"}});
        collector.setOamMetrics(AMF_URL, "fivegs_amffunction_rm_registeredsubnbr{plmnid=\"99970\",snssai=\"1\"} 5\n");
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

static json ahead(int from_s, int to_s) {
    return {{"startTs", NwdafSbiService::formatDateTime(Clock::now() + seconds(from_s))},
            {"endTs", NwdafSbiService::formatDateTime(Clock::now() + seconds(to_s))}};
}

static int causeOf(const SbiResponse& res, const std::string& cause) {
    INFO(res.body);
    if (!res.body.empty()) REQUIRE(json::parse(res.body)["cause"] == cause);
    return res.status;
}

TEST_CASE("H1.7: an NF_LOAD request for a future period is answered with a prediction") {
    PredFixture f;
    const json period = ahead(60, 120);
    const SbiResponse res = f.get({{"event-id", "NF_LOAD"}, {"tgt-ue", R"({"anyUe":true})"},
                                   {"ana-req", period.dump()}});
    INFO(res.body);
    REQUIRE(res.status == 200);
    const json body = json::parse(res.body);
    requireOfficialSchema(body, "TS29520_Nnwdaf_AnalyticsInfo.yaml#/components/schemas/AnalyticsData");
    REQUIRE(body["nfLoadLevelInfos"][0]["nfCpuUsage"] == 40);
    // One sample is below prediction_min_samples: zero confidence.
    REQUIRE(body["nfLoadLevelInfos"][0]["confidence"] == 0);
    // The predicted period is the validity period.
    REQUIRE(body["start"] == period["startTs"]);
    REQUIRE(body["expiry"] == period["endTs"]);
}

TEST_CASE("H1.7: prediction rules: NSI_LOAD_LEVEL yes, SLICE_LOAD_LEVEL no, within the horizon (I-13)") {
    PredFixture f;
    const std::string any = R"({"anySlice":true})";
    SbiResponse res = f.get({{"event-id", "NSI_LOAD_LEVEL"}, {"event-filter", any}, {"ana-req", ahead(60, 120).dump()}});
    INFO(res.body);
    REQUIRE(res.status == 200);
    const json body = json::parse(res.body);
    requireOfficialSchema(body, "TS29520_Nnwdaf_AnalyticsInfo.yaml#/components/schemas/AnalyticsData");
    REQUIRE(body["nsiLoadLevelInfos"][0]["loadLevelInformation"] == 50);
    REQUIRE(body["nsiLoadLevelInfos"][0].contains("confidence"));

    // SliceLoadLevelInformation can't carry the confidence a prediction needs.
    REQUIRE(causeOf(f.get({{"event-id", "LOAD_LEVEL_INFORMATION"}, {"event-filter", any},
                           {"ana-req", ahead(60, 120).dump()}}), "OPTIONAL_QUERY_PARAM_INCORRECT") == 400);
    // Beyond prediction_horizon_seconds (900 by default).
    REQUIRE(causeOf(f.get({{"event-id", "NSI_LOAD_LEVEL"}, {"event-filter", any}, {"ana-req", ahead(60, 5000).dump()}}),
                    "OPTIONAL_QUERY_PARAM_INCORRECT") == 400);
    // A horizon of 0 turns predictions off.
    NwdafConfig off = predConfig();
    off.prediction_horizon_seconds = 0;
    PredFixture g(off);
    REQUIRE(causeOf(g.get({{"event-id", "NF_LOAD"}, {"tgt-ue", R"({"anyUe":true})"}, {"ana-req", ahead(60, 120).dump()}}),
                    "OPTIONAL_QUERY_PARAM_INCORRECT") == 400);
}

TEST_CASE("H1.7: subscriptions to a future period report predictions; not on THRESHOLD (I-13)") {
    PredFixture f;
    json sub = {
        {"notificationURI", "http://127.0.0.1:9/cb"},
        {"evtReq", {{"immRep", true}}},
        {"eventSubscriptions", {{{"event", "NSI_LOAD_LEVEL"}, {"anySlice", true},
                                 {"extraReportReq", ahead(60, 120)},
                                 {"notificationMethod", "PERIODIC"}, {"repetitionPeriod", 30}}}}};
    SbiResponse res = f.subscribe(sub);
    INFO(res.body);
    REQUIRE(res.status == 201);
    const json body = json::parse(res.body);
    requireOfficialSchema(body, "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/NnwdafEventsSubscription");
    const json& n = body["eventNotifications"][0];
    REQUIRE(n["nsiLoadLevelInfos"][0].contains("confidence"));
    REQUIRE(n["start"] == sub["eventSubscriptions"][0]["extraReportReq"]["startTs"]);

    // THRESHOLD on a prediction isn't supported: the only event fails (I-3).
    json& es = sub["eventSubscriptions"][0];
    es.erase("notificationMethod"); es.erase("repetitionPeriod");
    es["nsiLevelThrds"] = {60};
    REQUIRE(causeOf(f.subscribe(sub), "MANDATORY_IE_INCORRECT") == 400);
}
