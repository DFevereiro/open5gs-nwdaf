// H1.9 — NRF NFManagement client (TS 29.510 V18.11.0) against a mock NRF.
// The registered NFProfile is validated against the official schema.
#include <catch2/catch_test_macros.hpp>
#include "official_schema.hpp"
#include "nwdaf_nrf_client.hpp"
#include "nwdaf_live_config.hpp"
#include "nwdaf_nf_monitor.hpp"
#include "nwdaf_analytics_catalogue.hpp"
#include <map>
#include "nwdaf_schema_validator.hpp"
#include "nwdaf_sbi.hpp"
#include "mock_open5gs.hpp"
#include "nwdaf_analytics.hpp"
#include "nwdaf_subscription.hpp"
#include <httplib.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef NWDAF_USE_HTTP2
#include "nwdaf_h2_server.hpp"
#endif

using json = nlohmann::json;

static const int NRF_PORT = 17786;
static const char* NF_ID = "0f0e0d0c-0b0a-4908-8706-050403020100";

struct NrfRequest { std::string method, path, body; };

// Records requests and answers like an NRF. Speaks HTTP/2 when the build does
// (the NWDAF's NRF client is HTTP/2 then — the Open5GS NRF requires it).
struct MockNrf {
    std::mutex m;
    std::vector<NrfRequest> requests;
    int patch_status = 204;
    int list_status = 200;
    std::map<std::string, std::vector<std::string>> instances;   // NF type → registered IDs
    std::map<std::string, std::string> nf_status;                // ID → NFStatus (default REGISTERED)

    SbiResponse handle(const std::string& method, const std::string& path, const std::string& body,
                       const std::string& nf_type = "") {
        std::lock_guard<std::mutex> lk(m);
        requests.push_back({method, path + (nf_type.empty() ? "" : "?nf-type=" + nf_type), body});
        const std::string list = "/nnrf-nfm/v1/nf-instances";
        if (method == "GET" && path == list) {   // NFListRetrieval
            if (list_status != 200) return {list_status, "", "", {}};
            json items = json::array();
            for (const auto& id : instances[nf_type])
                items.push_back({{"href", "http://127.0.0.1:" + std::to_string(NRF_PORT) + list + "/" + id}});
            json uris = {{"_links", {{"item", items}, {"self", {{"href", list}}}}},
                         {"totalItemCount", items.size()}};
            return {200, "application/3gppHal+json", uris.dump(), {}};
        }
        if (method == "GET" && path.rfind(list + "/", 0) == 0) {   // NFProfileRetrieval
            const std::string id = path.substr(list.size() + 1);
            for (const auto& [type, ids] : instances)
                for (const auto& i : ids)
                    if (i == id) {
                        const auto st = nf_status.find(id);
                        return {200, "application/json",
                                json{{"nfInstanceId", id}, {"nfType", type},
                                     {"nfStatus", st == nf_status.end() ? "REGISTERED" : st->second}}.dump(),
                                {}};
                    }
            return {404, "", "", {}};
        }
        if (method == "PUT")    return {201, "application/json", R"({"heartBeatTimer":7})", {}};
        if (method == "PATCH")  return {patch_status, "", "", {}};
        if (method == "DELETE") return {204, "", "", {}};
        return {405, "", "", {}};
    }
    std::vector<NrfRequest> seen() {
        std::lock_guard<std::mutex> lk(m);
        return requests;
    }
#ifdef NWDAF_USE_HTTP2
    NwdafH2Server server{[] { NwdafH2Server::Options o; o.port = NRF_PORT; return o; }(),
        [this](const SbiRequest& r, const std::string&) {
            auto q = [&](const char* k) { auto it = r.query.find(k);
                                          return it == r.query.end() ? std::string() : it->second; };
            return handle(r.method, r.path, r.body, q("nf-type"));
        }};
    MockNrf() { server.start(); }
    ~MockNrf() { server.stop(); }
#else
    httplib::Server server;
    std::thread thread;
    MockNrf() {
        auto h = [this](const httplib::Request& req, httplib::Response& res) {
            SbiResponse r = handle(req.method, req.path, req.body, req.get_param_value("nf-type"));
            res.status = r.status;
            if (!r.body.empty()) res.set_content(r.body, r.content_type);
        };
        server.Get(".*", h);
        server.Put(".*", h);
        server.Patch(".*", h);
        server.Delete(".*", h);
        thread = std::thread([this] { server.listen("127.0.0.1", NRF_PORT); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ~MockNrf() { server.stop(); thread.join(); }
#endif
};

static NwdafConfig nrfConfig() {
    NwdafConfig cfg;
    cfg.nf_instance_id = NF_ID;
    cfg.plmn_mcc = "999"; cfg.plmn_mnc = "70";
    cfg.sbi_bind_address = "127.0.0.1"; cfg.sbi_port = 7779; cfg.sbi_h2_port = 7780;
    cfg.nrf_uri = "http://127.0.0.1:" + std::to_string(NRF_PORT);
    cfg.nrf_heartbeat_interval_seconds = 60;
    return cfg;
}

static void requireOfficialProfile(const json& profile) {
    requireOfficialSchema(profile, "TS29510_Nnrf_NFManagement.yaml#/components/schemas/NFProfile");
}

TEST_CASE("H1.9: the NF profile is a valid Rel-18 NFProfile with both Nnwdaf services") {
    const json p = NwdafNrfClient(nrfConfig()).profile();
    requireOfficialProfile(p);
    REQUIRE(p["nfType"] == "NWDAF");
    REQUIRE_FALSE(p.contains("nfServices"));   // deprecated in TS 29.510
    const json& info = p["nfServiceList"]["nnwdaf-analyticsinfo-1"];
    const json& subs = p["nfServiceList"]["nnwdaf-eventssubscription-1"];
    REQUIRE(info["versions"][0]["apiFullVersion"] == "1.3.5");
    REQUIRE(subs["versions"][0]["apiFullVersion"] == "1.3.3");
    REQUIRE(info["scheme"] == "http");
    // The endpoint of the 3GPP interfaces: the HTTP/2 listener when built.
    REQUIRE(info["ipEndPoints"][0]["port"] == (NwdafHttpClient::http2() ? 7780 : 7779));
}

TEST_CASE("H1.9: the profile claims nothing that is not supported") {
    // No analytics advertised → no nwdafInfo, no supported features.
    json p = NwdafNrfClient(nrfConfig()).profile();
    REQUIRE_FALSE(p.contains("nwdafInfo"));
    REQUIRE_FALSE(p["nfServiceList"]["nnwdaf-analyticsinfo-1"].contains("supportedFeatures"));
    REQUIRE_FALSE(p["nfServiceList"]["nnwdaf-analyticsinfo-1"].contains("oauth2Required"));

    // NF_LOAD advertised → exactly that, and NfLoad in each API's numbering.
    NwdafConfig cfg = nrfConfig();
    cfg.nf_instance_ids = {{"AMF", "11111111-1111-4111-8111-111111111111"}};
    p = NwdafNrfClient(cfg).profile();
    requireOfficialProfile(p);
    REQUIRE(p["nwdafInfo"] == json{{"nwdafEvents", {"NF_LOAD"}}, {"eventIds", {"NF_LOAD"}}});
    REQUIRE(p["nfServiceList"]["nnwdaf-analyticsinfo-1"]["supportedFeatures"] == "80");
    REQUIRE(p["nfServiceList"]["nnwdaf-eventssubscription-1"]["supportedFeatures"] == "40");
}

TEST_CASE("H1.9: nwdafInfo uses each enum's spelling and lists the served TAIs") {
    // SLICE_LOAD_LEVEL is a NwdafEvent; its Nnwdaf_AnalyticsInfo EventId is
    // LOAD_LEVEL_INFORMATION. taiList is backed by served_tai_list.
    NwdafConfig cfg = nrfConfig();
    cfg.oam_metrics_endpoints = {{"AMF", "http://127.0.0.5:9090/metrics"}};
    cfg.slice_capacity = {NwdafSliceCapacity{1, "", 10, 0}};
    cfg.served_tai_list = {{"999", "70", "000001"}};
    const json p = NwdafNrfClient(cfg).profile();
    requireOfficialProfile(p);
    const json& info = p["nwdafInfo"];
    const auto has = [](const json& a, const char* v) { return std::find(a.begin(), a.end(), v) != a.end(); };
    REQUIRE(has(info["nwdafEvents"], "SLICE_LOAD_LEVEL"));
    REQUIRE(has(info["eventIds"], "LOAD_LEVEL_INFORMATION"));
    REQUIRE_FALSE(has(info["eventIds"], "SLICE_LOAD_LEVEL"));
    REQUIRE(info["taiList"] == json::array({{{"plmnId", {{"mcc", "999"}, {"mnc", "70"}}}, {"tac", "000001"}}}));
}

TEST_CASE("H1.9: scheme and oauth2Required follow the configuration") {
    NwdafConfig cfg = nrfConfig();
    cfg.tls_enabled = true;
    cfg.oauth_enabled = true;
    const json s = NwdafNrfClient(cfg).profile()["nfServiceList"]["nnwdaf-analyticsinfo-1"];
    REQUIRE(s["scheme"] == "https");
    REQUIRE(s["oauth2Required"] == true);
}

TEST_CASE("H1.9: NFRegister PUTs the profile and adopts the NRF's heartbeat timer") {
    MockNrf nrf;
    NwdafNrfClient client(nrfConfig());
    REQUIRE(client.registerNf());
    auto seen = nrf.seen();
    REQUIRE(seen.size() == 1);
    REQUIRE(seen[0].method == "PUT");
    REQUIRE(seen[0].path == std::string("/nnrf-nfm/v1/nf-instances/") + NF_ID);
    REQUIRE(json::parse(seen[0].body) == client.profile());
    REQUIRE(client.heartbeatSeconds() == 7);
}

TEST_CASE("QOL-05: a reload that changes the NF profile sends NFUpdate with the new advertisement") {
    MockNrf nrf;
    NwdafConfig cfg = nrfConfig();
    cfg.model_dir = "/tmp/nwdaf_reload_models";
    auto live = std::make_shared<NwdafLiveConfig>(cfg);
    NwdafNrfClient client(cfg, live);
    MockNwdafCollector collector(cfg);
    NwdafAnalyticsEngine engine(collector, cfg);
    NwdafNfMonitor monitor(cfg);
    REQUIRE_FALSE(client.profile().contains("nwdafInfo"));

    // nf_instance_ids reloads (NF_LOAD becomes advertised); the ports need a restart.
    NwdafConfig fresh = cfg;
    fresh.nf_instance_ids = {{"AMF", "11111111-1111-4111-8111-111111111111"}};
    fresh.sbi_port = 1;
    fresh.sbi_h2_port = 2;
    auto r = nwdafApplyReload(fresh, *live, collector, engine, monitor, &client);
    REQUIRE(r.changed == std::vector<std::string>{"nf_instance_ids"});
    REQUIRE(r.profile_changed);
    REQUIRE(r.nrf_updated);
    const auto seen = nrf.seen();
    REQUIRE(seen.size() == 1);
    REQUIRE(seen[0].method == "PUT");   // TS 29.510 §5.2.2.3.1A: complete replacement
    const json body = json::parse(seen[0].body);
    requireOfficialProfile(body);
    REQUIRE(body["nwdafInfo"]["nwdafEvents"] == json::array({"NF_LOAD"}));
    // The restart-only port keeps its running value.
    REQUIRE(body["nfServiceList"]["nnwdaf-analyticsinfo-1"]["ipEndPoints"][0]["port"] ==
            (NwdafHttpClient::http2() ? 7780 : 7779));
    REQUIRE(monitor.ids().at("AMF") == "11111111-1111-4111-8111-111111111111");

    // The same file again: nothing changed, nothing sent.
    r = nwdafApplyReload(fresh, *live, collector, engine, monitor, &client);
    REQUIRE(r.changed.empty());
    REQUIRE(nrf.seen().size() == 1);
}

TEST_CASE("H1.9: heartbeat is an NFUpdate PATCH; 404 re-registers at once") {
    MockNrf nrf;
    NwdafNrfClient client(nrfConfig());
    REQUIRE(client.heartbeat() == NwdafNrfClient::Heartbeat::Ok);
    auto seen = nrf.seen();
    REQUIRE(seen.back().method == "PATCH");
    REQUIRE(json::parse(seen.back().body) ==
            json::array({{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}}));

    nrf.patch_status = 404;
    REQUIRE(client.heartbeat() == NwdafNrfClient::Heartbeat::ReRegistered);
    seen = nrf.seen();
    REQUIRE(seen[seen.size() - 2].method == "PATCH");
    REQUIRE(seen.back().method == "PUT");
}

TEST_CASE("H1.9: a transient NRF failure does not change the profile") {
    MockNrf nrf;
    NwdafConfig cfg = nrfConfig();
    cfg.nf_instance_ids = {{"AMF", "11111111-1111-4111-8111-111111111111"}};
    NwdafNrfClient client(cfg);
    const json before = client.profile();
    nrf.patch_status = 500;
    REQUIRE(client.heartbeat() == NwdafNrfClient::Heartbeat::Failed);
    REQUIRE(client.profile() == before);
}

TEST_CASE("H1.9: NFDeregister DELETEs the instance") {
    MockNrf nrf;
    REQUIRE(NwdafNrfClient(nrfConfig()).deregister());
    auto seen = nrf.seen();
    REQUIRE(seen.back().method == "DELETE");
    REQUIRE(seen.back().path == std::string("/nnrf-nfm/v1/nf-instances/") + NF_ID);
}

// ── H1.9: NF instance IDs and NRF status from NRF polls ─────────────────────

static const char* AMF_ID  = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
static const char* SMF1_ID = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
static const char* SMF2_ID = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
static const char* UPF_ID  = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";

static NwdafConfig discoveryConfig() {
    NwdafConfig cfg = nrfConfig();
    cfg.nf_service_names = {{"AMF", "amfd"}, {"SMF", "smfd"}, {"UPF", "upfd"}};
    cfg.nrf_nf_discovery = true;
    return cfg;
}

// A monitor on a controllable clock, for the status window.
struct ClockedMonitor : NwdafNfMonitor {
    using NwdafNfMonitor::NwdafNfMonitor;
    std::chrono::system_clock::time_point t = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point now() const override { return t; }
};

TEST_CASE("H1.9: NF instance IDs come from NFListRetrieval, only when exactly one instance is listed") {
    MockNrf nrf;
    nrf.instances = {{"AMF", {AMF_ID}}, {"SMF", {SMF1_ID, SMF2_ID}}};
    NwdafNfMonitor monitor(discoveryConfig());
    monitor.refresh();
    const auto ids = monitor.ids();
    REQUIRE(ids.at("AMF") == AMF_ID);
    REQUIRE(ids.count("SMF") == 0);   // two SMFs: which one is measured here is unknown
    REQUIRE(ids.count("UPF") == 0);   // none registered

    // NFManagement list and profile retrieval — never NFDiscover, which the
    // NRF filters by allowedNfTypes (Open5GS NFs don't allow NWDAF).
    bool listed = false;
    for (const auto& r : nrf.seen()) {
        REQUIRE(r.path.rfind("/nnrf-disc/", 0) == std::string::npos);
        listed = listed || (r.method == "GET" && r.path == "/nnrf-nfm/v1/nf-instances?nf-type=AMF");
    }
    REQUIRE(listed);
}

TEST_CASE("H1.9: configured NF instance IDs win over the NRF list") {
    MockNrf nrf;
    nrf.instances = {{"AMF", {AMF_ID}}, {"UPF", {"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"}}};
    NwdafConfig cfg = discoveryConfig();
    cfg.nf_instance_ids = {{"UPF", UPF_ID}};
    NwdafNfMonitor monitor(cfg);
    monitor.refresh();
    REQUIRE(monitor.ids().at("UPF") == UPF_ID);
    REQUIRE(monitor.ids().at("AMF") == AMF_ID);
}

TEST_CASE("H1.9: a failed NRF poll keeps the last known ID; a deregistered NF is dropped") {
    MockNrf nrf;
    nrf.instances = {{"AMF", {AMF_ID}}};
    NwdafNfMonitor monitor(discoveryConfig());
    monitor.refresh();
    nrf.list_status = 503;
    monitor.refresh();
    REQUIRE(monitor.ids().at("AMF") == AMF_ID);

    nrf.list_status = 200;
    nrf.instances.clear();
    monitor.refresh();
    REQUIRE(monitor.ids().count("AMF") == 0);
}

TEST_CASE("H1.9: without nrf_nf_discovery the NRF is never queried") {
    MockNrf nrf;
    NwdafConfig cfg = discoveryConfig();
    cfg.nrf_nf_discovery = false;
    NwdafNfMonitor monitor(cfg);
    monitor.refresh();
    REQUIRE(nrf.seen().empty());
    REQUIRE(monitor.ids().empty());
    REQUIRE(monitor.statuses().empty());
}

TEST_CASE("H1.9: enabling NRF polling is a configured capability that advertises NF_LOAD") {
    NwdafConfig cfg = nrfConfig();
    REQUIRE(NwdafAnalyticsCatalogue::rel18Advertised(cfg).count("NF_LOAD") == 0);
    cfg.nrf_nf_discovery = true;
    REQUIRE(NwdafAnalyticsCatalogue::rel18Advertised(cfg).count("NF_LOAD") == 1);
}

static json statusOf(const std::vector<NwdafNfStatusObservation>& all, const std::string& id) {
    for (const auto& s : all) if (s.nf_instance_id == id) return s.nf_status;
    return nullptr;
}

TEST_CASE("H1.9: nfStatus is the share of NRF polls that found each state (I-8)") {
    MockNrf nrf;
    nrf.instances = {{"AMF", {AMF_ID}}};
    ClockedMonitor monitor(discoveryConfig());
    REQUIRE(monitor.statuses().empty());   // no poll yet

    monitor.refresh();                                     // REGISTERED
    monitor.t += std::chrono::seconds(60); monitor.refresh();   // REGISTERED
    nrf.nf_status[AMF_ID] = "UNDISCOVERABLE";
    monitor.t += std::chrono::seconds(60); monitor.refresh();
    nrf.instances.clear();                                 // gone from the NRF
    monitor.t += std::chrono::seconds(60); monitor.refresh();

    const json st = statusOf(monitor.statuses(), AMF_ID);
    REQUIRE(st == json{{"statusRegistered", 50}, {"statusUndiscoverable", 25}, {"statusUnregistered", 25}});
    requireOfficialSchema(st, "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/NfStatus");
}

TEST_CASE("H1.9: failed polls add no status sample; polls older than the window drop out") {
    MockNrf nrf;
    nrf.instances = {{"AMF", {AMF_ID}}};
    NwdafConfig cfg = discoveryConfig();
    cfg.nrf_nf_status_window_seconds = 300;
    ClockedMonitor monitor(cfg);
    monitor.refresh();                                     // REGISTERED
    nrf.list_status = 503;
    monitor.t += std::chrono::seconds(60); monitor.refresh();   // failed: not "unregistered"
    REQUIRE(statusOf(monitor.statuses(), AMF_ID) == json{{"statusRegistered", 100}});

    nrf.list_status = 200;
    nrf.instances.clear();
    monitor.t += std::chrono::seconds(60); monitor.refresh();   // absent
    REQUIRE(statusOf(monitor.statuses(), AMF_ID) ==
            json{{"statusRegistered", 50}, {"statusUnregistered", 50}});

    // Past the window, the instance is no longer reported at all.
    monitor.t += std::chrono::seconds(400); monitor.refresh();
    REQUIRE(statusOf(monitor.statuses(), AMF_ID).is_null());
}

TEST_CASE("H1.9: SUSPENDED counts towards the total only (I-8)") {
    MockNrf nrf;
    nrf.instances = {{"SMF", {SMF1_ID}}};
    nrf.nf_status[SMF1_ID] = "SUSPENDED";
    ClockedMonitor monitor(discoveryConfig());
    monitor.refresh();
    REQUIRE(monitor.statuses().empty());   // NfStatus has no attribute for SUSPENDED
    nrf.nf_status.clear();
    monitor.t += std::chrono::seconds(60); monitor.refresh();
    REQUIRE(statusOf(monitor.statuses(), SMF1_ID) == json{{"statusRegistered", 50}});
}

TEST_CASE("H1.9: NF_LOAD reports the NRF status of the instances the NRF lists") {
    MockNrf nrf;
    nrf.instances = {{"SMF", {SMF1_ID}}};
    NwdafConfig cfg = discoveryConfig();
    cfg.openapi_3gpp_dir = NWDAF_3GPP_OPENAPI_DIR;
    cfg.model_dir = "/tmp/nwdaf_nrf_models";
    auto monitor = std::make_shared<NwdafNfMonitor>(cfg);
    monitor->refresh();

    MockNwdafCollector collector(cfg);   // no local measurements: NRF data only
    NwdafAnalyticsEngine engine(collector, cfg);
    NwdafSubscriptionStore subs;
    NwdafSbiService sbi(engine, subs, cfg, monitor);
    SbiRequest req;
    req.method = "GET";
    req.path = std::string(NwdafSbiService::ANALYTICS_INFO_ROOT) + "/analytics";
    req.query = {{"event-id", "NF_LOAD"}, {"tgt-ue", R"({"anyUe":true})"}};
    const SbiResponse res = sbi.dispatch(req);
    INFO(res.body);
    REQUIRE(res.status == 200);
    const json body = json::parse(res.body);
    requireOfficialSchema(body, "TS29520_Nnwdaf_AnalyticsInfo.yaml#/components/schemas/AnalyticsData");
    REQUIRE(body["nfLoadLevelInfos"] == json::array({{{"nfType", "SMF"}, {"nfInstanceId", SMF1_ID},
                                                      {"nfStatus", {{"statusRegistered", 100}}}}}));
}
