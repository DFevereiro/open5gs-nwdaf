#include "nwdaf_network_performance.hpp"
#include <algorithm>

std::optional<double> NwdafNetworkPerformanceCalculator::numOfUe(const std::vector<NwdafOamScrape>& amf,
                                                                 const std::string& plmn_id,
                                                                 std::chrono::system_clock::time_point from,
                                                                 std::chrono::system_clock::time_point to) {
    double sum = 0.0;
    size_t n = 0;
    for (const auto& s : amf) {
        if (s.at < from || s.at > to) continue;
        double ues = 0.0;   // a scrape without the series: no UE registered yet
        for (const auto& p : s.samples) {
            if (p.name != "fivegs_amffunction_rm_registeredsubnbr") continue;
            const auto pl = p.labels.find("plmnid");
            if (pl != p.labels.end() && pl->second == plmn_id) ues += p.value;
        }
        sum += ues;
        ++n;
    }
    if (n == 0) return std::nullopt;
    return sum / static_cast<double>(n);
}

std::optional<double> NwdafNetworkPerformanceCalculator::sessSuccRatio(const std::vector<NwdafOamScrape>& smf,
                                                                       std::chrono::system_clock::time_point from,
                                                                       std::chrono::system_clock::time_point to) {
    // Every request, once: the series without PLMN and S-NSSAI labels.
    const auto req = increase(smf, from, to, [](const NwdafPromSample& p) {
        if (p.name != "fivegs_smffunction_sm_pdusessioncreationreq") return false;
        const auto pl = p.labels.find("plmnid");
        const auto sn = p.labels.find("snssai");
        return pl != p.labels.end() && pl->second.empty() && sn != p.labels.end() && sn->second.empty();
    });
    const auto fail = increase(smf, from, to, [](const NwdafPromSample& p) {
        return p.name == "fivegs_smffunction_sm_pdusessioncreationfail";
    });
    if (!req || *req == 0) return std::nullopt;
    const double failed = fail ? static_cast<double>(std::min(*fail, *req)) : 0.0;
    return 100.0 * (static_cast<double>(*req) - failed) / static_cast<double>(*req);
}
