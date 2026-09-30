#include "nwdaf_3gpp_adapter.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

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

// ── H1.4: NETWORK_PERFORMANCE (I-11) ────────────────────────────────────────

bool Nwdaf3gppAdapter::nwPerfTypeAvailable(const std::string& type, const NwdafConfig& cfg) {
    const bool served = !cfg.served_tai_list.empty();
    if (type == "NUM_OF_UE")
        return nwPerfPerArea(type, cfg) || (served && cfg.oam_metrics_endpoints.count("AMF") > 0);
    if (type == "SESS_SUCC_RATIO") return served && cfg.oam_metrics_endpoints.count("SMF") > 0;
    return false;
}

bool Nwdaf3gppAdapter::nwPerfPerArea(const std::string& type, const NwdafConfig& cfg) {
    return type == "NUM_OF_UE" && !cfg.amf_ue_info_endpoint.empty();
}

std::optional<double> Nwdaf3gppAdapter::ueCountInArea(const NwdafUeLocationTracker& ues, const json& area,
                                                      std::chrono::system_clock::time_point from,
                                                      std::chrono::system_clock::time_point to,
                                                      std::chrono::system_clock::time_point now) {
    // Only the observed time counts: from the first poll to the last.
    const auto held = ues.heldSince(now);
    const auto last = ues.lastPoll();
    if (!held || !last) return std::nullopt;
    const auto start = std::max(from, *held);
    const auto end = std::min(to, *last);
    if (end <= start) return std::nullopt;
    double ue_seconds = 0.0;
    for (const auto& s : ues.allStays(start, end))
        if (inArea(s.loc, area)) ue_seconds += std::chrono::duration<double>(s.to - s.from).count();
    return ue_seconds / std::chrono::duration<double>(end - start).count();
}

std::string Nwdaf3gppAdapter::taiKey(const json& tai) {
    const json plmn = tai.value("plmnId", json::object());
    long tac = 0;
    try { tac = std::stol(tai.value("tac", std::string("0")), nullptr, 16); } catch (...) {}
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06lx", tac);
    return plmn.value("mcc", std::string()) + "-" + plmn.value("mnc", std::string()) + "-" + buf;
}

json Nwdaf3gppAdapter::servedArea(const NwdafConfig& cfg) {
    json tais = json::array();
    for (const auto& t : cfg.served_tai_list)
        tais.push_back({{"plmnId", {{"mcc", t.mcc}, {"mnc", t.mnc}}}, {"tac", t.tac}});
    return {{"tais", tais}};
}

bool Nwdaf3gppAdapter::coversServedArea(const json& network_area, const NwdafConfig& cfg) {
    if (cfg.served_tai_list.empty() || !network_area.contains("tais")) return false;
    std::set<std::string> requested;
    for (const auto& t : network_area["tais"]) requested.insert(taiKey(t));
    const json served = servedArea(cfg);   // must outlive the loop
    for (const auto& t : served["tais"])
        if (!requested.count(taiKey(t))) return false;
    return true;
}

json Nwdaf3gppAdapter::nwPerfInfos(const NwdafConfig& cfg, const NwPerfQuery& query,
                                   const std::vector<NwdafOamScrape>& amf,
                                   const std::vector<NwdafOamScrape>& smf,
                                   const NwdafUeLocationTracker* ues,
                                   std::chrono::system_clock::time_point now) {
    const auto from = query.from.value_or(now - std::chrono::seconds(cfg.network_performance_window_seconds));
    const auto to   = query.to.value_or(now);
    json out = json::array();
    for (const auto& type : query.types) {
        json info = {{"networkArea", servedArea(cfg)}, {"nwPerfType", type}};
        if (nwPerfPerArea(type, cfg)) {
            // I-11: counted from the AMF's UE list, for the requested area.
            const auto v = ues ? ueCountInArea(*ues, query.area, from, to, now) : std::nullopt;
            if (!v) continue;
            info["networkArea"] = query.area;
            info["absoluteNum"] = std::lround(*v);
        } else if (type == "NUM_OF_UE") {
            const auto v = NwdafNetworkPerformanceCalculator::numOfUe(
                amf, NwdafSliceLoadCalculator::plmnLabel(cfg), from, to);
            if (!v) continue;
            info["absoluteNum"] = std::lround(*v);
        } else if (type == "SESS_SUCC_RATIO") {
            const auto v = NwdafNetworkPerformanceCalculator::sessSuccRatio(smf, from, to);
            // SamplingRatio is 1–100: a ratio that rounds to 0 % can't be sent (I-11).
            if (!v || std::lround(*v) < 1) continue;
            info["relativeRatio"] = std::lround(*v);
        } else {
            continue;
        }
        out.push_back(info);
    }
    return out;
}

// ── H1.1: UE_MOBILITY (I-12) ────────────────────────────────────────────────

namespace {

// TS 29.571 DateTime, UTC, whole seconds.
std::string dateTime(std::chrono::system_clock::time_point tp) {
    const auto t = std::chrono::system_clock::to_time_t(tp);
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

std::string locationKey(const NwdafUeLocation& loc) {
    char nci[16];
    std::snprintf(nci, sizeof(nci), "%09llx", static_cast<unsigned long long>(loc.nci));
    return loc.mcc + "-" + loc.mnc + "-" + loc.tac + "-" + nci;
}

long seconds(std::chrono::system_clock::duration d) {
    return std::lround(std::chrono::duration<double>(d).count());
}

}  // namespace

json Nwdaf3gppAdapter::userLocation(const NwdafUeLocation& loc) {
    char nci[16];
    std::snprintf(nci, sizeof(nci), "%09llx", static_cast<unsigned long long>(loc.nci));
    const json plmn = {{"mcc", loc.mcc}, {"mnc", loc.mnc}};
    return {{"nrLocation", {{"tai", {{"plmnId", plmn}, {"tac", loc.tac}}},
                            {"ncgi", {{"plmnId", plmn}, {"nrCellId", nci}}}}}};
}

bool Nwdaf3gppAdapter::inArea(const NwdafUeLocation& loc, const json& network_area) {
    const std::string tai = loc.mcc + "-" + loc.mnc + "-" + loc.tac;
    for (const auto& t : network_area.value("tais", json::array()))
        if (taiKey(t) == tai) return true;
    for (const auto& c : network_area.value("ncgis", json::array())) {
        const json plmn = c.value("plmnId", json::object());
        if (plmn.value("mcc", std::string()) != loc.mcc || plmn.value("mnc", std::string()) != loc.mnc) continue;
        try {
            if (std::stoull(c.value("nrCellId", std::string("x")), nullptr, 16) == loc.nci) return true;
        } catch (...) {}
    }
    return false;
}

json Nwdaf3gppAdapter::ueMobilities(const NwdafConfig& cfg, const UeMobilityQuery& query,
                                    const NwdafUeLocationTracker& tracker,
                                    std::chrono::system_clock::time_point now) {
    const auto from = query.from.value_or(now - std::chrono::seconds(cfg.ue_mobility_window_seconds));
    const auto to   = query.to.value_or(now);
    json out = json::array();
    if (to <= from || query.supis.empty()) return out;

    const auto staysOf = [&](const std::string& supi) {
        std::vector<NwdafUeStay> kept;
        for (auto& s : tracker.stays(supi, from, to)) {
            if (query.area && !inArea(s.loc, *query.area)) continue;
            // A stay that continues where the previous one at the same place ended is one stay.
            if (!kept.empty() && kept.back().loc == s.loc && kept.back().to == s.from) kept.back().to = s.to;
            else kept.push_back(s);
        }
        return kept;
    };

    if (query.supis.size() == 1) {
        // One UE: each stay is a time slot with its one location, in time
        // order (Table 6.7.2.3-1 NOTE 1); maxObjectNbr keeps the latest.
        for (const auto& s : staysOf(query.supis.front())) {
            const long duration = seconds(s.to - s.from);
            if (duration < 1) continue;
            out.push_back({{"ts", dateTime(s.from)}, {"duration", duration},
                           {"locInfos", json::array({{{"loc", userLocation(s.loc)}}})}});
        }
        if (query.max_objects && out.size() > *query.max_objects)
            out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(*query.max_objects));
        return out;
    }

    // A group: the proportion of its UEs at each location, averaged over the
    // period; rounded down so the ratios never sum above 100 %. The period
    // is the observed part: from the first poll (before it, UEs that have
    // since left are unknown) to the last.
    auto start = from;
    auto end = to;
    if (const auto held = tracker.heldSince(now)) start = std::max(start, *held);
    if (const auto last = tracker.lastPoll()) end = std::min(end, *last);
    if (end <= start) return out;
    std::map<std::string, std::pair<NwdafUeLocation, double>> time_at;   // key → location, UE-seconds
    for (const auto& supi : query.supis)
        for (auto s : staysOf(supi)) {
            if (s.to <= start || s.from >= end) continue;
            s.from = std::max(s.from, start);
            s.to = std::min(s.to, end);
            auto& e = time_at[locationKey(s.loc)];
            e.first = s.loc;
            e.second += std::chrono::duration<double>(s.to - s.from).count();
        }
    const double period = std::chrono::duration<double>(end - start).count();
    std::vector<std::pair<long, NwdafUeLocation>> ratios;
    for (const auto& [key, e] : time_at) {
        const long ratio = static_cast<long>(std::floor(100.0 * e.second / (period * query.supis.size())));
        if (ratio >= 1) ratios.push_back({ratio, e.first});   // SamplingRatio is 1–100
    }
    if (ratios.empty()) return out;
    std::stable_sort(ratios.begin(), ratios.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (query.max_objects && ratios.size() > *query.max_objects) ratios.resize(*query.max_objects);
    json locs = json::array();
    for (const auto& [ratio, loc] : ratios) locs.push_back({{"loc", userLocation(loc)}, {"ratio", ratio}});
    out.push_back({{"ts", dateTime(start)}, {"duration", seconds(end - start)}, {"locInfos", locs}});
    return out;
}
