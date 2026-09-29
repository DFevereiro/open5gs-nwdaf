#include "nwdaf_3gpp_adapter.hpp"
#include <algorithm>
#include <cmath>

using json = nlohmann::json;

json Nwdaf3gppAdapter::nfLoadLevelInfos(const std::vector<NfMetric>& metrics,
                                        const std::map<std::string, std::string>& nf_instance_ids,
                                        const std::vector<NwdafNfStatusObservation>& statuses,
                                        const NfLoadQuery& query) {
    // Instances in output order: measured NFs first, then NRF-only ones.
    std::vector<std::string> order;
    std::map<std::string, json> by_id;
    auto entry = [&](const std::string& type, const std::string& id) -> json& {
        auto it = by_id.find(id);
        if (it != by_id.end()) return it->second;
        order.push_back(id);
        return by_id[id] = {{"nfType", type}, {"nfInstanceId", id}};
    };

    for (const auto& m : metrics) {
        const auto id = nf_instance_ids.find(m.nf_type);
        if (id == nf_instance_ids.end()) continue;
        // The collector measures load only for a running NF with a PID.
        if (m.status != "active" || m.pid <= 0) continue;
        entry(m.nf_type, id->second)["nfCpuUsage"] =
            static_cast<int>(std::lround(std::clamp(m.load_pct, 0.0, 100.0)));
    }
    for (const auto& s : statuses)
        entry(s.nf_type, s.nf_instance_id)["nfStatus"] = s.nf_status;

    json out = json::array();
    for (const auto& id : order) {
        const json& e = by_id[id];
        if (!query.nf_types.empty() && !query.nf_types.count(e["nfType"].get<std::string>())) continue;
        if (!query.nf_instance_ids.empty() && !query.nf_instance_ids.count(id)) continue;
        if (query.max_objects && out.size() >= *query.max_objects) break;
        out.push_back(e);
    }
    return out;
}

std::optional<json> Nwdaf3gppAdapter::nfStatus(const std::map<std::string, size_t>& polls_per_status,
                                               size_t total_polls) {
    if (total_polls == 0) return std::nullopt;
    static const std::pair<const char*, const char*> MAP[] = {
        {"REGISTERED", "statusRegistered"},
        {"UNDISCOVERABLE", "statusUndiscoverable"},
        {"", "statusUnregistered"},   // not listed by the NRF
    };
    json out = json::object();
    for (const auto& [state, attr] : MAP) {
        const auto it = polls_per_status.find(state);
        if (it == polls_per_status.end()) continue;
        const long pct = std::lround(100.0 * static_cast<double>(it->second) / static_cast<double>(total_polls));
        if (pct >= 1) out[attr] = pct;
    }
    if (out.empty()) return std::nullopt;
    return out;
}
