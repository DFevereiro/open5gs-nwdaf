#include "nwdaf_nf_monitor.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

NwdafNfMonitor::NwdafNfMonitor(const NwdafConfig& config)
    : config_(config), http_(config) {}

std::chrono::system_clock::time_point NwdafNfMonitor::now() const {
    return std::chrono::system_clock::now();
}

std::map<std::string, std::string> NwdafNfMonitor::ids() const {
    std::map<std::string, std::string> out;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        out = resolved_;
    }
    for (const auto& [type, id] : config_.nf_instance_ids) out[type] = id;   // configured wins
    return out;
}

void NwdafNfMonitor::refresh() {
    if (!config_.nrf_nf_discovery) return;
    const std::string base = config_.nrf_uri + "/nnrf-nfm/v1/nf-instances";
    for (const auto& [type, unit] : config_.nf_service_names) {
        (void)unit;
        // NFListRetrieval (TS 29.510 §5.2.2.6): the instances of one NF type.
        const auto res = http_.request("GET", base + "?nf-type=" + type);
        if (!res || res.status != 200) {
            spdlog::debug("NRF NF list for {} failed ({})", type,
                          res ? "HTTP " + std::to_string(res.status) : res.error);
            continue;   // keep what was known; no status sample
        }
        std::vector<std::string> listed;
        try {
            const json list = json::parse(res.body);   // must outlive the loop
            for (const auto& item : list.at("_links").value("item", json::array())) {
                const std::string href = item.at("href").get<std::string>();
                const std::string id = href.substr(href.rfind('/') + 1);
                if (!id.empty()) listed.push_back(id);
            }
        } catch (const json::exception& e) {
            spdlog::warn("NRF NF list for {}: unreadable UriList ({})", type, e.what());
            continue;
        }

        // NFProfileRetrieval (§5.2.2.7) for each instance's NFStatus. A listed
        // instance is registered; its profile says whether it is also
        // SUSPENDED or UNDISCOVERABLE.
        Poll poll{now(), {}};
        for (const auto& id : listed) {
            const auto p = http_.request("GET", base + "/" + id);
            if (p && p.status == 404) continue;   // deregistered since the list
            std::string status = "REGISTERED";
            if (p && p.status == 200) {
                try {
                    status = json::parse(p.body).value("nfStatus", status);
                } catch (const json::exception&) {}
            }
            poll.status[id] = status;
        }

        std::lock_guard<std::mutex> lk(mutex_);
        auto& polls = polls_[type];
        polls.push_back(poll);
        const auto start = poll.at - std::chrono::seconds(config_.nrf_nf_status_window_seconds);
        while (!polls.empty() && polls.front().at < start) polls.pop_front();

        if (config_.nf_instance_ids.count(type)) continue;
        if (listed.size() == 1) {
            if (resolved_[type] != listed.front())
                spdlog::info("NRF: {} is {}", type, listed.front());
            resolved_[type] = listed.front();
        } else {
            if (listed.size() > 1)
                spdlog::warn("NRF: {} instances of {} registered; NF_LOAD cannot tell which "
                             "is measured here — set nf_instance_ids.{}", listed.size(), type, type);
            resolved_.erase(type);
        }
    }
}

std::vector<NwdafNfStatusObservation> NwdafNfMonitor::statuses() const {
    const auto start = now() - std::chrono::seconds(config_.nrf_nf_status_window_seconds);
    std::vector<NwdafNfStatusObservation> out;
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& [type, polls] : polls_) {
        size_t total = 0;
        std::map<std::string, std::map<std::string, size_t>> counts;   // ID → NFStatus → polls
        for (const auto& p : polls) {
            if (p.at < start) continue;
            ++total;
            for (const auto& entry : p.status) counts[entry.first];   // every instance seen
        }
        for (const auto& p : polls) {
            if (p.at < start) continue;
            for (auto& [id, per_status] : counts) {
                const auto it = p.status.find(id);
                ++per_status[it == p.status.end() ? "" : it->second];      // "" = not listed
            }
        }
        for (const auto& [id, per_status] : counts)
            if (auto s = Nwdaf3gppAdapter::nfStatus(per_status, total))
                out.push_back({type, id, *s});
    }
    return out;
}
