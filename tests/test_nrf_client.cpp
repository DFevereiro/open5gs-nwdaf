// H1.9 — NRF NFManagement client (TS 29.510 V18.11.0) against a mock NRF.
// The registered NFProfile is validated against the official schema.
#include <catch2/catch_test_macros.hpp>
#include "nwdaf_nrf_client.hpp"
#include "nwdaf_nf_id_resolver.hpp"
#include "nwdaf_analytics_catalogue.hpp"
#include <map>
#include "nwdaf_schema_validator.hpp"
#include "nwdaf_sbi.hpp"
#include <httplib.h>
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
    int discovery_status = 200;
    std::map<std::string, std::vector<std::string>> instances;   // NF type → registered IDs

    SbiResponse handle(const std::string& method, const std::string& path, const std::string& body,
                       const std::string& target_type = "", const std::string& requester = "") {
        std::lock_guard<std::mutex> lk(m);
        requests.push_back({method, path + (target_type.empty() ? "" : "?" + target_type + "," + requester), body});
        if (method == "GET" && path == "/nnrf-disc/v1/nf-instances") {
            if (discovery_status != 200) return {discovery_status, "", "", {}};
            json result = {{"nfInstances", json::array()}};
            for (const auto& id : instances[target_type])
                result["nfInstances"].push_back({{"nfInstanceId", id}, {"nfType", target_type},
                                                 {"nfStatus", "REGISTERED"}});
            return {200, "application/json", result.dump(), {}};
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
            return handle(r.method, r.path, r.body, q("target-nf-type"), q("requester-nf-type"));
        }};
    MockNrf() { server.start(); }
    ~MockNrf() { server.stop(); }
#else
    httplib::Server server;
    std::thread thread;
    MockNrf() {
        auto h = [this](const httplib::Request& req, httplib::Response& res) {
            SbiResponse r = handle(req.method, req.path, req.body,
                                   req.get_param_value("target-nf-type"),
                                   req.get_param_value("requester-nf-type"));
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
    static NwdafSchemaValidator official(NWDAF_3GPP_OPENAPI_DIR);
    auto v = official.validate(profile, "TS29510_Nnrf_NFManagement.yaml#/components/schemas/NFProfile");
    INFO(profile.dump(2));
    INFO((v.empty() ? std::string() : v.front().pointer + " " + v.front().reason));
    REQUIRE(v.empty());
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

// ── H1.9: NF instance IDs from NRF discovery ────────────────────────────────

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

TEST_CASE("H1.9: discovery resolves an NF type only when exactly one instance is registered") {
    MockNrf nrf;
    nrf.instances = {{"AMF", {AMF_ID}}, {"SMF", {SMF1_ID, SMF2_ID}}};
    NwdafNfIdResolver resolver(discoveryConfig());
    resolver.refresh();
    const auto ids = resolver.ids();
    REQUIRE(ids.at("AMF") == AMF_ID);
    REQUIRE(ids.count("SMF") == 0);   // two SMFs: which one is measured here is unknown
    REQUIRE(ids.count("UPF") == 0);   // none registered

    // The query names both mandatory parameters (TS 29.510 NFDiscovery).
    bool asked = false;
    for (const auto& r : nrf.seen())
        asked = asked || (r.method == "GET" && r.path == "/nnrf-disc/v1/nf-instances?AMF,NWDAF");
    REQUIRE(asked);
}

TEST_CASE("H1.9: configured NF instance IDs win and are not looked up") {
    MockNrf nrf;
    nrf.instances = {{"AMF", {AMF_ID}}, {"UPF", {"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"}}};
    NwdafConfig cfg = discoveryConfig();
    cfg.nf_instance_ids = {{"UPF", UPF_ID}};
    NwdafNfIdResolver resolver(cfg);
    resolver.refresh();
    REQUIRE(resolver.ids().at("UPF") == UPF_ID);
    for (const auto& r : nrf.seen()) REQUIRE(r.path.find("?UPF,") == std::string::npos);
}

TEST_CASE("H1.9: a failed discovery keeps the last known ID; a deregistered NF is dropped") {
    MockNrf nrf;
    nrf.instances = {{"AMF", {AMF_ID}}};
    NwdafNfIdResolver resolver(discoveryConfig());
    resolver.refresh();
    nrf.discovery_status = 503;
    resolver.refresh();
    REQUIRE(resolver.ids().at("AMF") == AMF_ID);

    nrf.discovery_status = 200;
    nrf.instances.clear();
    resolver.refresh();
    REQUIRE(resolver.ids().count("AMF") == 0);
}

TEST_CASE("H1.9: without nrf_nf_discovery the NRF is never queried") {
    MockNrf nrf;
    NwdafConfig cfg = discoveryConfig();
    cfg.nrf_nf_discovery = false;
    NwdafNfIdResolver resolver(cfg);
    resolver.refresh();
    REQUIRE(nrf.seen().empty());
    REQUIRE(resolver.ids().empty());
}

TEST_CASE("H1.9: enabling discovery is a configured capability that advertises NF_LOAD") {
    NwdafConfig cfg = nrfConfig();
    REQUIRE(NwdafAnalyticsCatalogue::rel18Advertised(cfg).count("NF_LOAD") == 0);
    cfg.nrf_nf_discovery = true;
    REQUIRE(NwdafAnalyticsCatalogue::rel18Advertised(cfg).count("NF_LOAD") == 1);
}
