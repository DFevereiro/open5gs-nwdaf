// H1.4 — NETWORK_PERFORMANCE (TS 23.288 V18.13.0 §6.6) from the Open5GS
// TS 28.552 measurements, per I-11: NUM_OF_UE and SESS_SUCC_RATIO for the whole
// served area, outputs checked against the official TS 29.520 schemas.
#include <catch2/catch_test_macros.hpp>
#include "official_schema.hpp"
#include "mock_open5gs.hpp"
#include "nwdaf_3gpp_adapter.hpp"
#include "nwdaf_analytics.hpp"
#include "nwdaf_analytics_catalogue.hpp"
#include "nwdaf_network_performance.hpp"
#include "nwdaf_sbi.hpp"
#include "nwdaf_subscription.hpp"
#include <fstream>
#include <thread>

using json = nlohmann::json;
using Clock = std::chrono::system_clock;
using Calc = NwdafNetworkPerformanceCalculator;

static const char* AMF_URL = "http://127.0.0.5:9090/metrics";
static const char* SMF_URL = "http://127.0.0.4:9090/metrics";
static const char* UE_INFO_URL = "http://127.0.0.5:9090/ue-info";

static NwdafPromSample ues(const std::string& snssai, double v, const std::string& plmn = "99970") {
    return {"fivegs_amffunction_rm_registeredsubnbr", {{"plmnid", plmn}, {"snssai", snssai}}, v};
}
static NwdafPromSample req(double v) {   // the unlabelled series: every request once
    return {"fivegs_smffunction_sm_pdusessioncreationreq", {{"plmnid", ""}, {"snssai", ""}}, v};
}
static NwdafPromSample reqSlice(double v) {
    return {"fivegs_smffunction_sm_pdusessioncreationreq", {{"plmnid", "99970"}, {"snssai", "1"}}, v};
}
static NwdafPromSample fail(const std::string& cause, double v) {
    return {"fivegs_smffunction_sm_pdusessioncreationfail", {{"cause", cause}}, v};
}

// ── I-11 arithmetic ─────────────────────────────────────────────────────────

TEST_CASE("H1.4: NUM_OF_UE is the mean of the per-slice registered-UE counts of this PLMN (I-11)") {
    const auto t0 = Clock::now() - std::chrono::minutes(2);
    const std::vector<NwdafOamScrape> amf = {
        {t0, {ues("1", 2), ues("2", 1), ues("1", 50, "00101")}},   // another PLMN is not counted
        {t0 + std::chrono::seconds(10), {ues("1", 3), ues("2", 2)}},
        {t0 + std::chrono::seconds(20), {}}};                     // no series yet → 0
    REQUIRE(*Calc::numOfUe(amf, "99970", Clock::time_point{}, Clock::now()) == (3.0 + 5.0 + 0.0) / 3);
    REQUIRE_FALSE(Calc::numOfUe(amf, "99970", Clock::now() - std::chrono::seconds(1), Clock::now()));
}

TEST_CASE("H1.4: counter increases handle resets; SESS_SUCC_RATIO is (Req - Fail) / Req (I-11)") {
    const auto t0 = Clock::now() - std::chrono::minutes(2);
    const auto s = [&](int sec, std::vector<NwdafPromSample> v) { return NwdafOamScrape{t0 + std::chrono::seconds(sec), v}; };
    // Requests 10 → 14, then a restart (0 → 6): +4 +6 = 10. Failures 1 → 3: +2.
    // The per-slice request series (counted again after parsing) is ignored.
    const std::vector<NwdafOamScrape> smf = {
        s(0,  {req(10), reqSlice(10), fail("403", 1)}),
        s(10, {req(14), reqSlice(14), fail("403", 3)}),
        s(20, {req(6),  reqSlice(6),  fail("403", 3)})};
    const auto any = [](const NwdafPromSample& p) { return p.name == "fivegs_smffunction_sm_pdusessioncreationreq" &&
                                                           p.labels.at("plmnid").empty(); };
    REQUIRE(*Calc::increase(smf, Clock::time_point{}, Clock::now(), any) == 10);
    REQUIRE(*Calc::sessSuccRatio(smf, Clock::time_point{}, Clock::now()) == 80.0);
    // One scrape, or no request in the period: no ratio.
    REQUIRE_FALSE(Calc::sessSuccRatio({smf[0]}, Clock::time_point{}, Clock::now()));
    REQUIRE_FALSE(Calc::sessSuccRatio({s(0, {req(5)}), s(10, {req(5)})}, Clock::time_point{}, Clock::now()));
}

// ── Configuration and advertisement ─────────────────────────────────────────

static std::string writeConfig(const std::string& body) {
    const std::string path = "/tmp/nwdaf_nwperf_cfg_test.yaml";
    std::ofstream(path) << "nwdaf:\n  nf_instance_id: \"7c1f4e2a-9b3d-4a6e-8f10-2d3c4b5a6e7f\"\n"
                           "  plmn_mcc: \"999\"\n  plmn_mnc: \"70\"\n" << body;
    return path;
}

TEST_CASE("H1.4: served_tai_list is parsed and validated") {
    auto cfg = NwdafConfig::load(writeConfig(
        "  served_tai_list:\n"
        "    - {tac: \"000001\"}\n"
        "    - {mcc: \"001\", mnc: \"01\", tac: \"00AB\"}\n"
        "    - {tac: 7}\n"));
    REQUIRE(cfg.served_tai_list.size() == 3);
    REQUIRE(cfg.served_tai_list[0] == NwdafTai{"999", "70", "000001"});   // PLMN defaults to plmn_mcc/mnc
    REQUIRE(cfg.served_tai_list[1] == NwdafTai{"001", "01", "0000ab"});
    REQUIRE(cfg.served_tai_list[2].tac == "000007");
    // An unquoted number is decimal, as in Open5GS amf.yaml; a quoted one is hex.
    cfg = NwdafConfig::load(writeConfig("  served_tai_list:\n    - {tac: 1000}\n    - {tac: \"1000\"}\n"));
    REQUIRE(cfg.served_tai_list[0].tac == "0003e8");
    REQUIRE(cfg.served_tai_list[1].tac == "001000");
    REQUIRE_THROWS(NwdafConfig::load(writeConfig("  served_tai_list: [{tac: \"xyz\"}]\n")));
    REQUIRE_THROWS(NwdafConfig::load(writeConfig("  served_tai_list: [{mcc: \"999\"}]\n")));
}

static NwdafConfig npConfig() {
    NwdafConfig cfg;
    cfg.nf_instance_id = "7c1f4e2a-9b3d-4a6e-8f10-2d3c4b5a6e7f";
    cfg.plmn_mcc = "999"; cfg.plmn_mnc = "70";
    cfg.nf_service_names = {{"AMF", "amfd"}, {"SMF", "smfd"}};
    cfg.collection_interval_seconds = 1;
    cfg.throughput_history_size = 10;
    cfg.oam_metrics_endpoints = {{"AMF", AMF_URL}, {"SMF", SMF_URL}};
    cfg.served_tai_list = {{"999", "70", "000001"}};
    cfg.openapi_3gpp_dir = NWDAF_3GPP_OPENAPI_DIR;
    cfg.model_dir = "/tmp/nwdaf_nwperf_models";
    cfg.log_level = "warn"; cfg.log_file = "/tmp/nwdaf_nwperf_test.log";
    return cfg;
}

TEST_CASE("H1.4: NETWORK_PERFORMANCE is advertised only with a served area and a metrics source") {
    NwdafConfig cfg = npConfig();
    REQUIRE(NwdafAnalyticsCatalogue::rel18Advertised(cfg).count("NETWORK_PERFORMANCE") == 1);
    // NetworkPerformance is AnalyticsInfo feature 3, EventsSubscription feature 8.
    REQUIRE(NwdafSupportedFeatures::local(NnwdafApi::AnalyticsInfo, cfg).has(3));
    REQUIRE(NwdafSupportedFeatures::local(NnwdafApi::EventsSubscription, cfg).has(8));
    cfg.served_tai_list.clear();
    REQUIRE(NwdafAnalyticsCatalogue::rel18Advertised(cfg).count("NETWORK_PERFORMANCE") == 0);
    cfg = npConfig();
    cfg.oam_metrics_endpoints.clear();
    REQUIRE(NwdafAnalyticsCatalogue::rel18Advertised(cfg).count("NETWORK_PERFORMANCE") == 0);
}

// ── The 3GPP interfaces ─────────────────────────────────────────────────────

// A service over a mock collector that scraped the AMF and SMF twice:
// 2 then 4 registered UEs (mean 3); 10 → 20 requests with 0 → 2 failures (80 %).
struct NpFixture {
    NwdafConfig            cfg;
    MockNwdafCollector     collector;
    NwdafAnalyticsEngine   engine;
    NwdafSubscriptionStore subs;
    NwdafSbiService        sbi;

    // `ue_info`: the AMF's UE list (/ue-info page), when configured.
    explicit NpFixture(NwdafConfig c = npConfig(), const std::string& ue_info = "")
        : cfg(std::move(c)), collector(cfg), engine(collector, cfg), sbi(engine, subs, cfg) {
        if (!ue_info.empty()) collector.setOamMetrics(std::string(UE_INFO_URL) + "?page=0&page_size=100", ue_info);
        scrape(2, 10, 0);
        scrape(4, 20, 2);
    }
    void scrape(int ue_count, int requests, int failures) {
        collector.setOamMetrics(AMF_URL, "fivegs_amffunction_rm_registeredsubnbr{plmnid=\"99970\",snssai=\"1\"} " +
                                         std::to_string(ue_count) + "\n");
        collector.setOamMetrics(SMF_URL,
            "fivegs_smffunction_sm_pdusessioncreationreq{plmnid=\"\",snssai=\"\"} " + std::to_string(requests) + "\n"
            "fivegs_smffunction_sm_pdusessioncreationfail{cause=\"403\"} " + std::to_string(failures) + "\n");
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

static const json AREA = {{"tais", {{{"plmnId", {{"mcc", "999"}, {"mnc", "70"}}}, {"tac", "000001"}}}}};
static const std::string ANY_UE = R"({"anyUe":true})";

static std::string filter(json types, json area = AREA) {
    json f = {{"nwPerfTypes", types}};
    if (!area.is_null()) f["networkArea"] = area;
    return f.dump();
}

static int causeOf(const SbiResponse& res, const std::string& cause) {
    INFO(res.body);
    if (!res.body.empty()) REQUIRE(json::parse(res.body)["cause"] == cause);
    return res.status;
}

TEST_CASE("H1.4: NETWORK_PERFORMANCE answers nwPerfs for the served area") {
    NpFixture f;
    const SbiResponse res = f.get({{"event-id", "NETWORK_PERFORMANCE"}, {"tgt-ue", ANY_UE},
                                   {"event-filter", filter({"NUM_OF_UE", "SESS_SUCC_RATIO"})},
                                   {"supported-features", "4"}});
    INFO(res.body);
    REQUIRE(res.status == 200);
    const json body = json::parse(res.body);
    requireOfficialSchema(body, "TS29520_Nnwdaf_AnalyticsInfo.yaml#/components/schemas/AnalyticsData");
    REQUIRE(body["nwPerfs"] == json::array({
        {{"networkArea", AREA}, {"nwPerfType", "NUM_OF_UE"}, {"absoluteNum", 3}},
        {{"networkArea", AREA}, {"nwPerfType", "SESS_SUCC_RATIO"}, {"relativeRatio", 80}}}));
    REQUIRE(body["suppFeat"] == "4");   // NetworkPerformance negotiated
}

TEST_CASE("H1.4: NETWORK_PERFORMANCE request rules and failure semantics") {
    NpFixture f;
    const auto get = [&](std::multimap<std::string, std::string> q) {
        q.insert({"event-id", "NETWORK_PERFORMANCE"});
        return f.get(q);
    };
    // §4.3.2.2: the target UE and nwPerfTypes are mandatory; networkArea with anyUe.
    REQUIRE(causeOf(get({{"event-filter", filter({"NUM_OF_UE"})}}), "MANDATORY_QUERY_PARAM_MISSING") == 400);
    REQUIRE(causeOf(get({{"tgt-ue", ANY_UE}}), "MANDATORY_QUERY_PARAM_MISSING") == 400);
    REQUIRE(causeOf(get({{"tgt-ue", ANY_UE}, {"event-filter", filter({"NUM_OF_UE"}, json())}}),
                    "MANDATORY_QUERY_PARAM_MISSING") == 400);
    // Per-UE statistics aren't observed.
    REQUIRE(causeOf(get({{"tgt-ue", R"({"supis":["imsi-999700000000001"]})"}, {"event-filter", filter({"NUM_OF_UE"})}}),
                    "MANDATORY_QUERY_PARAM_INCORRECT") == 400);
    // Only the whole served area; only the available types; no predictions.
    const json other_tac = {{"tais", {{{"plmnId", {{"mcc", "999"}, {"mnc", "70"}}}, {"tac", "000002"}}}}};
    REQUIRE(causeOf(get({{"tgt-ue", ANY_UE}, {"event-filter", filter({"NUM_OF_UE"}, other_tac)}}),
                    "OPTIONAL_QUERY_PARAM_INCORRECT") == 400);
    REQUIRE(causeOf(get({{"tgt-ue", ANY_UE}, {"event-filter", filter({"GNB_ACTIVE_RATIO"})}}),
                    "OPTIONAL_QUERY_PARAM_INCORRECT") == 400);
    REQUIRE(causeOf(get({{"tgt-ue", ANY_UE}, {"event-filter", filter({"NUM_OF_UE"})},
                         {"ana-req", R"({"startTs":"2099-01-01T00:00:00Z"})"}}),
                    "OPTIONAL_QUERY_PARAM_INCORRECT") == 400);
    REQUIRE(causeOf(get({{"tgt-ue", ANY_UE}, {"event-filter", filter({"NUM_OF_UE"})},
                         {"ana-req", R"({"startTs":"2020-01-01T00:00:00Z"})"}}),
                    "UNAVAILABLE_DATA") == 500);
}

TEST_CASE("H1.4: a larger requested area that contains the served area is answered") {
    NpFixture f;
    json area = AREA;
    area["tais"].push_back({{"plmnId", {{"mcc", "999"}, {"mnc", "70"}}}, {"tac", "0002"}});
    const SbiResponse res = f.get({{"event-id", "NETWORK_PERFORMANCE"}, {"tgt-ue", ANY_UE},
                                   {"event-filter", filter({"NUM_OF_UE"}, area)}});
    REQUIRE(res.status == 200);
    // The statistics describe the served area, which is what is reported.
    REQUIRE(json::parse(res.body)["nwPerfs"][0]["networkArea"] == AREA);
}

TEST_CASE("H1.4: NETWORK_PERFORMANCE subscriptions: periodic reports and threshold inputs") {
    NpFixture f;
    json sub = {
        {"notificationURI", "http://127.0.0.1:9/cb"},
        {"evtReq", {{"immRep", true}}},
        {"eventSubscriptions", {{{"event", "NETWORK_PERFORMANCE"}, {"tgtUe", {{"anyUe", true}}},
                                 {"networkArea", AREA},
                                 {"nwPerfRequs", {{{"nwPerfType", "NUM_OF_UE"}}, {{"nwPerfType", "SESS_SUCC_RATIO"}}}},
                                 {"notificationMethod", "PERIODIC"}, {"repetitionPeriod", 60}}}}};
    SbiResponse res = f.subscribe(sub);
    INFO(res.body);
    REQUIRE(res.status == 201);
    json body = json::parse(res.body);
    requireOfficialSchema(body, "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/NnwdafEventsSubscription");
    REQUIRE(body["eventNotifications"][0]["nwPerfs"].size() == 2);

    // THRESHOLD (the default): each requirement needs its threshold (Table 5.1.6.2.22-1 NOTE 1).
    json& es = sub["eventSubscriptions"][0];
    es.erase("notificationMethod"); es.erase("repetitionPeriod");
    REQUIRE(causeOf(f.subscribe(sub), "MANDATORY_IE_MISSING") == 400);
    es["nwPerfRequs"] = {{{"nwPerfType", "NUM_OF_UE"}, {"absoluteNum", 5}},
                         {{"nwPerfType", "SESS_SUCC_RATIO"}, {"relativeRatio", 90}}};
    es["matchingDir"] = "DESCENDING";
    REQUIRE(f.subscribe(sub).status == 201);
    // A count threshold on a ratio type is not meaningful: the event fails (I-3).
    es["nwPerfRequs"] = {{{"nwPerfType", "SESS_SUCC_RATIO"}, {"absoluteNum", 3}}};
    REQUIRE(causeOf(f.subscribe(sub), "MANDATORY_IE_INCORRECT") == 400);
}

TEST_CASE("H1.4: NETWORK_PERFORMANCE thresholds report the crossed types (I-10, I-11)") {
    const NwdafConfig cfg = npConfig();
    const json es = {{"event", "NETWORK_PERFORMANCE"}, {"tgtUe", {{"anyUe", true}}}, {"networkArea", AREA},
                     {"nwPerfRequs", {{{"nwPerfType", "NUM_OF_UE"}, {"absoluteNum", 4}}}},
                     {"matchingDir", "ASCENDING"}};
    const auto inputs = [](int ue_count) {
        NwdafReportInputs in;
        in.amf_oam = {{Clock::now() - std::chrono::seconds(1), {ues("1", ue_count)}}};
        return in;
    };
    NwdafSbiService::ThresholdState state;
    REQUIRE(NwdafSbiService::thresholdReports(es, inputs(2), cfg, state).empty());   // baseline
    const auto rs = NwdafSbiService::thresholdReports(es, inputs(5), cfg, state);
    REQUIRE(rs.size() == 1);
    requireOfficialSchema(rs[0], "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/EventNotification");
    REQUIRE(rs[0]["nwPerfs"][0]["nwPerfType"] == "NUM_OF_UE");
    REQUIRE(rs[0]["nwPerfs"][0]["absoluteNum"] == 5);
    REQUIRE(NwdafSbiService::thresholdReports(es, inputs(3), cfg, state).empty());   // descending: not matched
}

// ── NUM_OF_UE per area from the AMF's UE list ───────────────────────────────

// An Open5GS /ue-info page: two UEs in TA 1 (cell 16), one in TA 2 (cell 32).
static std::string ueInfoPage() {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        (Clock::now() - std::chrono::seconds(60)).time_since_epoch()).count();
    json items = json::array();
    const std::vector<std::pair<int, int>> ues = {{1, 16}, {1, 16}, {2, 32}};
    for (size_t i = 0; i < ues.size(); ++i) {
        char tac[8];
        std::snprintf(tac, sizeof(tac), "%06x", ues[i].first);
        items.push_back({{"supi", "imsi-99970000000000" + std::to_string(i + 1)},
                         {"location", {{"timestamp", us},
                                       {"nr_tai", {{"plmn", "99970"}, {"tac_hex", tac}}},
                                       {"nr_cgi", {{"plmn", "99970"}, {"nci", ues[i].second}}}}}});
    }
    return json{{"items", items}, {"pager", {{"page", 0}, {"page_size", 100}, {"count", items.size()}}}}.dump();
}

static json taiArea(const char* tac) {
    return {{"tais", {{{"plmnId", {{"mcc", "999"}, {"mnc", "70"}}}, {"tac", tac}}}}};
}

TEST_CASE("H1.4: with the AMF UE list, NUM_OF_UE is answered for any area of TAs or cells (I-11)") {
    NwdafConfig cfg = npConfig();
    cfg.amf_ue_info_endpoint = UE_INFO_URL;
    NpFixture f(cfg, ueInfoPage());
    const auto get = [&](json types, json area) {
        return f.get({{"event-id", "NETWORK_PERFORMANCE"}, {"tgt-ue", ANY_UE}, {"event-filter", filter(types, area)}});
    };

    SbiResponse res = get({"NUM_OF_UE"}, taiArea("000002"));
    INFO(res.body);
    REQUIRE(res.status == 200);
    json body = json::parse(res.body);
    requireOfficialSchema(body, "TS29520_Nnwdaf_AnalyticsInfo.yaml#/components/schemas/AnalyticsData");
    // The requested area is reported.
    REQUIRE(body["nwPerfs"] == json::array({{{"networkArea", taiArea("000002")}, {"nwPerfType", "NUM_OF_UE"},
                                             {"absoluteNum", 1}}}));
    const json cell = {{"ncgis", {{{"plmnId", {{"mcc", "999"}, {"mnc", "70"}}}, {"nrCellId", "000000010"}}}}};
    REQUIRE(json::parse(get({"NUM_OF_UE"}, cell).body)["nwPerfs"][0]["absoluteNum"] == 2);

    // The served area: NUM_OF_UE counts UEs in the UE list (2 in TA 1, not the
    // metrics' mean of 3); SESS_SUCC_RATIO still comes from the SMF.
    body = json::parse(get({"NUM_OF_UE", "SESS_SUCC_RATIO"}, AREA).body);
    REQUIRE(body["nwPerfs"][0]["absoluteNum"] == 2);
    REQUIRE(body["nwPerfs"][1]["relativeRatio"] == 80);

    // SESS_SUCC_RATIO has no per-area source; other area forms aren't reported.
    REQUIRE(causeOf(get({"NUM_OF_UE", "SESS_SUCC_RATIO"}, taiArea("000002")), "OPTIONAL_QUERY_PARAM_INCORRECT") == 400);
    const json ecgi = {{"ecgis", {{{"plmnId", {{"mcc", "999"}, {"mnc", "70"}}}, {"eutraCellId", "0000001"}}}}};
    REQUIRE(causeOf(get({"NUM_OF_UE"}, ecgi), "OPTIONAL_QUERY_PARAM_INCORRECT") == 400);
}

TEST_CASE("H1.4: the AMF UE list alone advertises NETWORK_PERFORMANCE for NUM_OF_UE") {
    NwdafConfig cfg = npConfig();
    cfg.served_tai_list.clear();
    cfg.oam_metrics_endpoints.clear();
    cfg.amf_ue_info_endpoint = UE_INFO_URL;
    REQUIRE(NwdafAnalyticsCatalogue::rel18Advertised(cfg).count("NETWORK_PERFORMANCE") == 1);
    REQUIRE(Nwdaf3gppAdapter::nwPerfTypeAvailable("NUM_OF_UE", cfg));
    REQUIRE_FALSE(Nwdaf3gppAdapter::nwPerfTypeAvailable("SESS_SUCC_RATIO", cfg));
    // SESS_SUCC_RATIO needs the served area as well as the SMF endpoint.
    cfg.oam_metrics_endpoints = {{"SMF", SMF_URL}};
    REQUIRE_FALSE(Nwdaf3gppAdapter::nwPerfTypeAvailable("SESS_SUCC_RATIO", cfg));
}
