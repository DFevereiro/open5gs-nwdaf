#include "nwdaf_nf_id_resolver.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

NwdafNfIdResolver::NwdafNfIdResolver(const NwdafConfig& config)
    : config_(config), http_(config) {}

std::map<std::string, std::string> NwdafNfIdResolver::ids() const {
    std::map<std::string, std::string> out;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        out = discovered_;
    }
    for (const auto& [type, id] : config_.nf_instance_ids) out[type] = id;   // configured wins
    return out;
}

void NwdafNfIdResolver::refresh() {
    if (!config_.nrf_nf_discovery) return;
    for (const auto& [type, unit] : config_.nf_service_names) {
        (void)unit;
        if (config_.nf_instance_ids.count(type)) continue;
        const std::string url = config_.nrf_uri + "/nnrf-disc/v1/nf-instances?target-nf-type=" +
                                type + "&requester-nf-type=NWDAF";
        const auto res = http_.request("GET", url);
        if (!res || res.status != 200) {
            spdlog::debug("NF discovery for {} failed ({})", type,
                          res ? "HTTP " + std::to_string(res.status) : res.error);
            continue;   // keep what was known
        }
        std::string id;
        size_t count = 0;
        try {
            const json result = json::parse(res.body);   // must outlive the loop
            for (const auto& p : result.at("nfInstances")) {
                if (p.value("nfType", "") != type) continue;
                ++count;
                id = p.at("nfInstanceId").get<std::string>();
            }
        } catch (const json::exception& e) {
            spdlog::warn("NF discovery for {}: unreadable SearchResult ({})", type, e.what());
            continue;
        }
        std::lock_guard<std::mutex> lk(mutex_);
        if (count == 1) {
            if (discovered_[type] != id)
                spdlog::info("NF discovery: {} is {}", type, id);
            discovered_[type] = id;
        } else {
            if (count > 1)
                spdlog::warn("NF discovery: {} instances of {} registered; NF_LOAD cannot tell which "
                             "is measured here — set nf_instance_ids.{}", count, type, type);
            discovered_.erase(type);
        }
    }
}
