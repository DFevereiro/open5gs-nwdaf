#include "nwdaf_3gpp_adapter.hpp"
#include <algorithm>
#include <cmath>

using json = nlohmann::json;

json Nwdaf3gppAdapter::nfLoadLevelInfos(const std::vector<NfMetric>& metrics,
                                        const NwdafConfig& cfg,
                                        const NfLoadQuery& query) {
    json out = json::array();
    for (const auto& m : metrics) {
        const auto id = cfg.nf_instance_ids.find(m.nf_type);
        if (id == cfg.nf_instance_ids.end()) continue;
        // The collector measures load only for a running NF with a PID.
        if (m.status != "active" || m.pid <= 0) continue;
        if (!query.nf_types.empty() && !query.nf_types.count(m.nf_type)) continue;
        if (!query.nf_instance_ids.empty() && !query.nf_instance_ids.count(id->second)) continue;
        if (query.max_objects && out.size() >= *query.max_objects) break;
        out.push_back({
            {"nfType",       m.nf_type},
            {"nfInstanceId", id->second},
            {"nfCpuUsage",   static_cast<int>(std::lround(std::clamp(m.load_pct, 0.0, 100.0)))},
        });
    }
    return out;
}
