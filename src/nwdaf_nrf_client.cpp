#include "nwdaf_nrf_client.hpp"
#include "nwdaf_3gpp_adapter.hpp"
#include "nwdaf_analytics_catalogue.hpp"
#include "nwdaf_supported_features.hpp"
#include <spdlog/spdlog.h>

using json = nlohmann::json;

NwdafNrfClient::NwdafNrfClient(const NwdafConfig& config)
    : config_(config), http_(config), heartbeat_s_(config.nrf_heartbeat_interval_seconds) {}

std::string NwdafNrfClient::instanceUrl() const {
    return config_.nrf_uri + "/nnrf-nfm/v1/nf-instances/" + config_.nf_instance_id;
}

json NwdafNrfClient::profile() const {
    // The 3GPP interfaces are on the HTTP/2 listener when there is one (H1.8).
    const int port = NwdafHttpClient::http2() && config_.sbi_h2_port > 0
                   ? config_.sbi_h2_port : config_.sbi_port;

    auto service = [&](const char* name, const char* full_version, NnwdafApi api) {
        json s = {
            {"serviceInstanceId", std::string(name) + "-1"},
            {"serviceName",       name},
            // API versions of the pinned artifacts (docs/frozen-standards.md).
            {"versions",          {{{"apiVersionInUri", "v1"}, {"apiFullVersion", full_version}}}},
            {"scheme",            config_.tls_enabled ? "https" : "http"},
            {"nfServiceStatus",   "REGISTERED"},
            {"ipEndPoints",       {{{"ipv4Address", config_.sbi_bind_address}, {"port", port}}}},
        };
        // TS 29.500 §6.6.2: register the features the NF supports.
        const NwdafFeatureSet features = NwdafSupportedFeatures::local(api, config_);
        if (!features.empty()) s["supportedFeatures"] = features.toHex();
        if (config_.oauth_enabled) s["oauth2Required"] = true;
        return s;
    };

    json p = {
        {"nfInstanceId",   config_.nf_instance_id},
        {"nfType",         "NWDAF"},
        {"nfStatus",       "REGISTERED"},
        {"heartBeatTimer", config_.nrf_heartbeat_interval_seconds},
        {"ipv4Addresses",  {config_.sbi_bind_address}},
        {"plmnList",       {{{"mcc", config_.plmn_mcc}, {"mnc", config_.plmn_mnc}}}},
        {"nfServiceList",  {
            {"nnwdaf-analyticsinfo-1",
             service("nnwdaf-analyticsinfo", "1.3.5", NnwdafApi::AnalyticsInfo)},
            {"nnwdaf-eventssubscription-1",
             service("nnwdaf-eventssubscription", "1.3.3", NnwdafApi::EventsSubscription)},
        }},
    };

    // nwdafInfo only with what is actually supported: the advertised
    // analytics (nwdafEvents; eventIds in the Nnwdaf_AnalyticsInfo EventId
    // spelling), and taiList when the served area is configured.
    // nwdafCapability, analyticsDelay, serving-NF lists and mlAnalyticsList
    // are omitted — nothing backs them.
    const auto advertised = NwdafAnalyticsCatalogue::rel18Advertised(config_);
    if (!advertised.empty()) {
        json event_ids = json::array();
        for (const auto& id : advertised) event_ids.push_back(NwdafAnalyticsCatalogue::toAnalyticsInfoEventId(id));
        p["nwdafInfo"] = {{"nwdafEvents", advertised}, {"eventIds", event_ids}};
        if (!config_.served_tai_list.empty())
            p["nwdafInfo"]["taiList"] = Nwdaf3gppAdapter::servedArea(config_)["tais"];
    }
    return p;
}

bool NwdafNrfClient::registerNf() {
    const auto res = http_.request("PUT", instanceUrl(), profile().dump(), "application/json");
    if (!res || (res.status != 200 && res.status != 201)) {
        spdlog::warn("NRF NFRegister failed ({})",
                     res ? "HTTP " + std::to_string(res.status) : res.error);
        return false;
    }
    spdlog::info("NRF NFRegister: {} (HTTP/{})", res.status == 201 ? "201 Created" : "200 OK",
                 res.http_version);
    try {
        const json body = json::parse(res.body);
        if (body.contains("heartBeatTimer")) {
            heartbeat_s_ = body["heartBeatTimer"].get<int>();
            spdlog::info("NRF heartbeat interval: {}s", heartbeat_s_);
        }
    } catch (const json::exception&) {}
    return true;
}

NwdafNrfClient::Heartbeat NwdafNrfClient::heartbeat() {
    const json patch = json::array({{{"op", "replace"}, {"path", "/nfStatus"}, {"value", "REGISTERED"}}});
    const auto res = http_.request("PATCH", instanceUrl(), patch.dump(), "application/json-patch+json");
    if (res && (res.status == 200 || res.status == 204)) return Heartbeat::Ok;
    if (res && res.status == 404) {
        // TS 29.510: the NRF no longer holds the profile.
        spdlog::warn("NRF heartbeat: 404 — the NRF lost the profile, registering again");
        return registerNf() ? Heartbeat::ReRegistered : Heartbeat::Failed;
    }
    spdlog::warn("NRF heartbeat failed ({})",
                 res ? "HTTP " + std::to_string(res.status) : res.error);
    return Heartbeat::Failed;
}

bool NwdafNrfClient::deregister() {
    const auto res = http_.request("DELETE", instanceUrl());
    const bool ok = res && (res.status == 204 || res.status == 200);
    if (ok) spdlog::info("NRF NFDeregister: done");
    else spdlog::warn("NRF NFDeregister failed ({})",
                      res ? "HTTP " + std::to_string(res.status) : res.error);
    return ok;
}
