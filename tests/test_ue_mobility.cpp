// H1.1 — UE_MOBILITY (TS 23.288 V18.13.0 §6.7.2) from the Open5GS AMF /ue-info
// list, per I-12: stays per UE, time slots on the 3GPP interfaces, outputs
// checked against the official TS 29.520 schemas. SUPIs are test values.
#include <catch2/catch_test_macros.hpp>
#include "official_schema.hpp"
#include "mock_open5gs.hpp"
#include "nwdaf_3gpp_adapter.hpp"
#include "nwdaf_analytics.hpp"
#include "nwdaf_analytics_catalogue.hpp"
#include "nwdaf_sbi.hpp"
#include "nwdaf_subscription.hpp"
#include "nwdaf_ue_location.hpp"
#include <thread>

using json = nlohmann::json;
using Clock = std::chrono::system_clock;
using std::chrono::seconds;

static const char* UE_INFO_URL = "http://127.0.0.5:9090/ue-info";
static const char* UE_A = "imsi-999700000000001";
static const char* UE_B = "imsi-999700000000002";

static int64_t micros(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
}

// One /ue-info item as Open5GS v2.8.0 prints it (interoperability record of 2026-09-30).
static json ueItem(const std::string& supi, int tac, uint64_t nci, Clock::time_point detected) {
    char tac_hex[8];
    std::snprintf(tac_hex, sizeof(tac_hex), "%06x", tac);
    return {{"supi", supi}, {"cm_state", "connected"},
            {"location", {{"timestamp", micros(detected)},
                          {"nr_tai", {{"plmn", "99970"}, {"tac_hex", tac_hex}, {"tac", tac}}},
                          {"nr_cgi", {{"plmn", "99970"}, {"nci", nci}, {"gnb_id", nci >> 14}, {"cell_id", nci & 0x3fff}}},
                          {"last_visited_plmn_id", "000000"}}},
            {"pdu_sessions_count", 1}};
}

static std::string page(json items, bool next = false) {
    json pager = {{"page", 0}, {"page_size", 100}, {"count", items.size()}};
    if (next) pager["next"] = "/ue-info?page=1";
    return json{{"items", items}, {"pager", pager}}.dump();
}

static NwdafUeInfoItem item(const std::string& supi, const std::string& tac, uint64_t nci, Clock::time_point t) {
    return {supi, {"999", "70", tac, nci}, t};
}

// ── Source parsing and trajectories ─────────────────────────────────────────

TEST_CASE("H1.1: an AMF /ue-info page gives each UE's TAI, cell and detection time") {
    const auto t = Clock::now() - seconds(30);
    json no_location = ueItem(UE_B, 1, 16, t);
    no_location["location"]["timestamp"] = 0;   // the AMF has no location yet
    bool next = false;
    const auto items = NwdafUeLocationTracker::parsePage(page({ueItem(UE_A, 0xab, 16, t), no_location}, true), next);
    REQUIRE(items);
    REQUIRE(next);
    REQUIRE(items->size() == 1);
    const auto& a = items->front();
    REQUIRE(a.supi == UE_A);
    REQUIRE(a.loc == NwdafUeLocation{"999", "70", "0000ab", 16});
    REQUIRE(micros(a.detected) == micros(t));
    REQUIRE_FALSE(NwdafUeLocationTracker::parsePage("fivegs_x 1\n", next));   // not a UE list
}

TEST_CASE("H1.1: polls become stays: a move closes the stay at the AMF's detection time (I-12)") {
    const auto t0 = Clock::now() - seconds(100);
    NwdafUeLocationTracker tracker(seconds(3600));
    REQUIRE_FALSE(tracker.heldSince(Clock::now()));
    tracker.observe(t0,               {item(UE_A, "000001", 16, t0 - seconds(5)), item(UE_B, "000001", 16, t0)});
    tracker.observe(t0 + seconds(10), {item(UE_A, "000001", 16, t0 + seconds(8)), item(UE_B, "000001", 16, t0)});
    // UE A was detected in another cell of TA 2 at t0+15; UE B deregistered.
    tracker.observe(t0 + seconds(20), {item(UE_A, "000002", 32, t0 + seconds(15))});

    const auto a = tracker.stays(UE_A, t0 - seconds(60), Clock::now());
    REQUIRE(a.size() == 2);
    REQUIRE(a[0].loc.tac == "000001");
    REQUIRE(a[0].from == t0 - seconds(5));   // detected there before the first poll
    REQUIRE(a[0].to == t0 + seconds(15));
    REQUIRE(a[1].loc == NwdafUeLocation{"999", "70", "000002", 32});
    REQUIRE(a[1].to == t0 + seconds(20));
    const auto b = tracker.stays(UE_B, t0 - seconds(60), Clock::now());
    REQUIRE(b.size() == 1);
    REQUIRE(b[0].to == t0 + seconds(10));    // closed at the last poll that listed it
    // Clipped to the requested period.
    REQUIRE(tracker.stays(UE_A, t0 + seconds(12), t0 + seconds(18)).size() == 2);
    REQUIRE(tracker.stays(UE_A, t0 + seconds(12), t0 + seconds(18))[0].from == t0 + seconds(12));
    REQUIRE(*tracker.heldSince(Clock::now()) == t0);
    REQUIRE(tracker.ueCount() == 1);
}

// ── Output mapping ──────────────────────────────────────────────────────────

static NwdafConfig umConfig() {
    NwdafConfig cfg;
    cfg.nf_instance_id = "7c1f4e2a-9b3d-4a6e-8f10-2d3c4b5a6e7f";
    cfg.plmn_mcc = "999"; cfg.plmn_mnc = "70";
    cfg.nf_service_names = {{"AMF", "amfd"}};
    cfg.collection_interval_seconds = 1;
    cfg.amf_ue_info_endpoint = UE_INFO_URL;
    cfg.openapi_3gpp_dir = NWDAF_3GPP_OPENAPI_DIR;
    cfg.model_dir = "/tmp/nwdaf_uemob_models";
    cfg.log_level = "warn"; cfg.log_file = "/tmp/nwdaf_uemob_test.log";
    return cfg;
}

TEST_CASE("H1.1: one UE's stays are time slots with one location each (Table 6.7.2.3-1, I-12)") {
    const auto t0 = Clock::now() - seconds(100);
    NwdafUeLocationTracker tracker(seconds(3600));
    tracker.observe(t0,               {item(UE_A, "000001", 16, t0)});
    tracker.observe(t0 + seconds(30), {item(UE_A, "000002", 32, t0 + seconds(20))});
    tracker.observe(t0 + seconds(60), {item(UE_A, "000001", 16, t0 + seconds(50))});

    Nwdaf3gppAdapter::UeMobilityQuery q;
    q.supis = {UE_A};
    const json mobs = Nwdaf3gppAdapter::ueMobilities(umConfig(), q, tracker, Clock::now());
    REQUIRE(mobs.size() == 3);
    for (const auto& m : mobs)
        requireOfficialSchema(m, "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/UeMobility");
    REQUIRE(mobs[0]["duration"] == 20);
    REQUIRE(mobs[1]["duration"] == 30);
    REQUIRE(mobs[1]["locInfos"][0]["loc"]["nrLocation"]["ncgi"]["nrCellId"] == "000000020");
    REQUIRE(mobs[1]["locInfos"][0]["loc"]["nrLocation"]["tai"]["tac"] == "000002");
    REQUIRE_FALSE(mobs[1]["locInfos"][0].contains("ratio"));   // a ratio is of a group's UEs

    q.max_objects = 1;   // maxObjectNbr keeps the latest
    REQUIRE(Nwdaf3gppAdapter::ueMobilities(umConfig(), q, tracker, Clock::now())[0]["ts"] == mobs[2]["ts"]);
    q.max_objects.reset();
    q.area = json{{"ncgis", {{{"plmnId", {{"mcc", "999"}, {"mnc", "70"}}}, {"nrCellId", "000000010"}}}}};
    REQUIRE(Nwdaf3gppAdapter::ueMobilities(umConfig(), q, tracker, Clock::now()).size() == 2);
}

TEST_CASE("H1.1: several SUPIs are a group: ratios of its UEs per location, highest first (I-12)") {
    const auto t0 = Clock::now() - seconds(100);
    NwdafUeLocationTracker tracker(seconds(3600));
    tracker.observe(t0,               {item(UE_A, "000001", 16, t0), item(UE_B, "000002", 32, t0)});
    tracker.observe(t0 + seconds(100), {item(UE_A, "000001", 16, t0), item(UE_B, "000002", 32, t0)});

    Nwdaf3gppAdapter::UeMobilityQuery q;
    q.supis = {UE_A, UE_B, "imsi-999700000000009"};   // one UE never seen
    q.from = t0;
    q.to = t0 + seconds(100);
    const json mobs = Nwdaf3gppAdapter::ueMobilities(umConfig(), q, tracker, Clock::now());
    REQUIRE(mobs.size() == 1);
    requireOfficialSchema(mobs[0], "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/UeMobility");
    REQUIRE(mobs[0]["duration"] == 100);
    REQUIRE(mobs[0]["locInfos"].size() == 2);
    REQUIRE(mobs[0]["locInfos"][0]["ratio"] == 33);   // 1 of 3 UEs, rounded down
    REQUIRE(mobs[0]["locInfos"][1]["ratio"] == 33);
}

// ── The 3GPP interfaces ─────────────────────────────────────────────────────

TEST_CASE("H1.1: UE_MOBILITY is advertised only with the AMF UE list configured") {
    NwdafConfig cfg = umConfig();
    REQUIRE(NwdafAnalyticsCatalogue::rel18Advertised(cfg).count("UE_MOBILITY") == 1);
    // UeMobility is AnalyticsInfo feature 1, EventsSubscription feature 2.
    REQUIRE(NwdafSupportedFeatures::local(NnwdafApi::AnalyticsInfo, cfg).has(1));
    REQUIRE(NwdafSupportedFeatures::local(NnwdafApi::EventsSubscription, cfg).has(2));
    cfg.amf_ue_info_endpoint.clear();
    REQUIRE(NwdafAnalyticsCatalogue::rel18NotAdvertised(cfg).at("UE_MOBILITY") == "needs UE locations (amf_ue_info_endpoint)");
}

// A service over a mock collector that polled the AMF twice: UE A moved from
// TA 1 (cell 16) to TA 2 (cell 32); UE B stayed in TA 1.
struct UmFixture {
    NwdafConfig            cfg;
    MockNwdafCollector     collector;
    NwdafAnalyticsEngine   engine;
    NwdafSubscriptionStore subs;
    NwdafSbiService        sbi;

    UmFixture() : cfg(umConfig()), collector(cfg), engine(collector, cfg), sbi(engine, subs, cfg) {
        const auto t = Clock::now() - seconds(40);
        poll({ueItem(UE_A, 1, 16, t), ueItem(UE_B, 1, 16, t)});
        poll({ueItem(UE_A, 2, 32, t + seconds(20)), ueItem(UE_B, 1, 16, t)});
    }
    void poll(json items) {
        collector.setOamMetrics(std::string(UE_INFO_URL) + "?page=0&page_size=100", page(items));
        collector.startBackgroundCollection();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        collector.stopBackgroundCollection();
    }
    SbiResponse get(std::multimap<std::string, std::string> query) {
        query.insert({"event-id", "UE_MOBILITY"});
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

static std::string supis(std::vector<std::string> s) { return json{{"supis", s}}.dump(); }

static int causeOf(const SbiResponse& res, const std::string& cause) {
    INFO(res.body);
    if (!res.body.empty()) REQUIRE(json::parse(res.body)["cause"] == cause);
    return res.status;
}

TEST_CASE("H1.1: UE_MOBILITY answers ueMobs for a SUPI") {
    UmFixture f;
    const SbiResponse res = f.get({{"tgt-ue", supis({UE_A})}, {"supported-features", "1"}});
    INFO(res.body);
    REQUIRE(res.status == 200);
    const json body = json::parse(res.body);
    requireOfficialSchema(body, "TS29520_Nnwdaf_AnalyticsInfo.yaml#/components/schemas/AnalyticsData");
    REQUIRE(body["ueMobs"].size() == 2);
    REQUIRE(body["ueMobs"][1]["locInfos"][0]["loc"]["nrLocation"]["tai"]["tac"] == "000002");
    REQUIRE(body["suppFeat"] == "1");   // UeMobility (feature 1) negotiated

    // The operator /health shows the source.
    bool listed = false;
    for (const auto& src : f.engine.getOamSources())
        if (src.endpoint == UE_INFO_URL) listed = src.up && src.count == 2;
    REQUIRE(listed);
}

TEST_CASE("H1.1: UE_MOBILITY request rules and failure semantics") {
    UmFixture f;
    // §4.3.2.2: supis or intGroupIds is mandatory.
    REQUIRE(causeOf(f.get({}), "MANDATORY_QUERY_PARAM_MISSING") == 400);
    REQUIRE(causeOf(f.get({{"tgt-ue", R"({"anyUe":true})"}}), "MANDATORY_QUERY_PARAM_INCORRECT") == 400);
    // Groups can't be resolved; area forms other than TAs and cells; predictions.
    REQUIRE(causeOf(f.get({{"tgt-ue", R"({"intGroupIds":["00000000-001-01-01"]})"}}),
                    "OPTIONAL_QUERY_PARAM_INCORRECT") == 400);
    REQUIRE(causeOf(f.get({{"tgt-ue", supis({UE_A})},
                           {"event-filter", R"({"networkArea":{"ecgis":[{"plmnId":{"mcc":"999","mnc":"70"},"eutraCellId":"0000001"}]}})"}}),
                    "OPTIONAL_QUERY_PARAM_INCORRECT") == 400);
    REQUIRE(causeOf(f.get({{"tgt-ue", supis({UE_A})}, {"ana-req", R"({"startTs":"2099-01-01T00:00:00Z"})"}}),
                    "OPTIONAL_QUERY_PARAM_INCORRECT") == 400);
    // Past statistics from before the NWDAF held locations.
    REQUIRE(causeOf(f.get({{"tgt-ue", supis({UE_A})}, {"ana-req", R"({"startTs":"2020-01-01T00:00:00Z"})"}}),
                    "UNAVAILABLE_DATA") == 500);
    // No location for the UE, or none in the area of interest: 204.
    REQUIRE(f.get({{"tgt-ue", supis({"imsi-999700000000009"})}}).status == 204);
    REQUIRE(f.get({{"tgt-ue", supis({UE_B})},
                   {"event-filter", R"({"networkArea":{"tais":[{"plmnId":{"mcc":"999","mnc":"70"},"tac":"000002"}]}})"}})
                .status == 204);
}

TEST_CASE("H1.1: UE_MOBILITY subscriptions report ueMobs; THRESHOLD is not defined for it") {
    UmFixture f;
    json sub = {
        {"notificationURI", "http://127.0.0.1:9/cb"},
        {"evtReq", {{"immRep", true}}},
        {"eventSubscriptions", {{{"event", "UE_MOBILITY"}, {"tgtUe", {{"supis", {UE_A, UE_B}}}},
                                 {"notificationMethod", "PERIODIC"}, {"repetitionPeriod", 60}}}}};
    SbiResponse res = f.subscribe(sub);
    INFO(res.body);
    REQUIRE(res.status == 201);
    const json body = json::parse(res.body);
    requireOfficialSchema(body, "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/NnwdafEventsSubscription");
    const json& mobs = body["eventNotifications"][0]["ueMobs"];
    REQUIRE(mobs.size() == 1);                      // the two UEs as a group
    REQUIRE(mobs[0]["locInfos"][0].contains("ratio"));

    // THRESHOLD (the default method) has no UE mobility threshold: the only
    // event fails, so nothing is subscribed (I-3).
    sub["eventSubscriptions"][0].erase("notificationMethod");
    sub["eventSubscriptions"][0].erase("repetitionPeriod");
    REQUIRE(causeOf(f.subscribe(sub), "MANDATORY_IE_INCORRECT") == 400);
}

TEST_CASE("H1.1: the AMF UE list is read page by page; a failed page changes nothing") {
    const NwdafConfig cfg = umConfig();
    MockNwdafCollector collector(cfg);
    const auto t = Clock::now() - seconds(10);
    const std::string base = std::string(UE_INFO_URL) + "?page=";
    collector.setOamMetrics(base + "0&page_size=100", page({ueItem(UE_A, 1, 16, t)}, true));
    collector.setOamMetrics(base + "1&page_size=100", page({ueItem(UE_B, 1, 16, t)}));
    collector.startBackgroundCollection();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    collector.stopBackgroundCollection();
    REQUIRE(collector.ueLocations()->ueCount() == 2);

    collector.setOamMetrics(base + "1&page_size=100", std::nullopt);   // the second page fails
    collector.startBackgroundCollection();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    collector.stopBackgroundCollection();
    REQUIRE(collector.ueLocations()->ueCount() == 2);   // not treated as UE B leaving
}
