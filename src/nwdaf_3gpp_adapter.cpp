#include "nwdaf_3gpp_adapter.hpp"
#include <algorithm>
#include <cctype>
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

// ── H1.2: slice load level (I-9) ────────────────────────────────────────────

std::string Nwdaf3gppAdapter::snssaiKey(const json& snssai) {
    NwdafSliceCapacity s;
    s.sst = snssai.value("sst", 0);
    s.sd  = snssai.value("sd", std::string());
    for (auto& c : s.sd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s.key();
}

json Nwdaf3gppAdapter::snssai(const NwdafSliceCapacity& slice) {
    json j = {{"sst", slice.sst}};
    if (!slice.sd.empty()) j["sd"] = slice.sd;
    return j;
}

std::vector<NwdafSliceLoad> Nwdaf3gppAdapter::sliceLoads(const NwdafConfig& cfg, const SliceQuery& query,
                                                         const std::vector<NwdafOamScrape>& amf,
                                                         const std::vector<NwdafOamScrape>& smf,
                                                         std::chrono::system_clock::time_point now) {
    std::vector<NwdafSliceLoad> out;
    // I-9: without an analytics target period, the last slice_load_window_seconds.
    const auto from = query.from.value_or(now - std::chrono::seconds(cfg.slice_load_window_seconds));
    const auto to   = query.to.value_or(now);
    for (const auto& slice : cfg.slice_capacity) {
        if (!query.any && !query.keys.count(slice.key())) continue;
        if (auto load = NwdafSliceLoadCalculator::compute(slice, NwdafSliceLoadCalculator::plmnLabel(cfg),
                                                          amf, smf, from, to))
            out.push_back(*load);
    }
    return out;
}

json Nwdaf3gppAdapter::sliceLoadLevelInfos(const std::vector<NwdafSliceLoad>& loads) {
    json out = json::array();
    for (const auto& l : loads)
        out.push_back({{"loadLevelInformation", l.load_level},
                       {"snssais", json::array({snssai(l.slice)})}});
    return out;
}

std::vector<json> Nwdaf3gppAdapter::sliceLoadLevelGroups(const std::vector<NwdafSliceLoad>& loads) {
    std::map<int, json> by_level;
    for (const auto& l : loads) by_level[l.load_level].push_back(snssai(l.slice));
    std::vector<json> out;
    for (const auto& [level, snssais] : by_level)
        out.push_back({{"loadLevelInformation", level}, {"snssais", snssais}});
    return out;
}

json Nwdaf3gppAdapter::nsiLoadLevelInfos(const std::vector<NwdafSliceLoad>& loads) {
    json out = json::array();
    for (const auto& l : loads)
        out.push_back({{"loadLevelInformation", l.load_level}, {"snssai", snssai(l.slice)}});
    return out;
}
