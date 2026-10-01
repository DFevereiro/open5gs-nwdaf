#include "nwdaf_slice_load.hpp"
#include <algorithm>
#include <cmath>

namespace {

const char* UES_METRIC = "fivegs_amffunction_rm_registeredsubnbr";
const char* PDU_METRIC = "fivegs_smffunction_sm_sessionnbr";

std::optional<NwdafNumberStat> stat(const std::vector<NwdafOamScrape>& scrapes, const char* metric,
                                    const std::string& plmn_id, const std::string& snssai,
                                    std::chrono::system_clock::time_point from,
                                    std::chrono::system_clock::time_point to) {
    std::vector<double> values;
    for (const auto& s : scrapes) {
        if (s.at < from || s.at > to) continue;
        double v = 0.0;   // series not created yet = no UE / session so far
        // O5GS-05: per subscribed S-NSSAI, as the AMF counts it (A.6).
        for (const auto& p : s.samples) {
            if (p.name != metric) continue;
            const auto pl = p.labels.find("plmnid");
            const auto sn = p.labels.find("snssai");
            if (pl == p.labels.end() || sn == p.labels.end()) continue;
            if (pl->second == plmn_id && sn->second == snssai) v += p.value;
        }
        values.push_back(v);
    }
    if (values.empty()) return std::nullopt;
    NwdafNumberStat st;
    st.samples = values.size();
    for (double v : values) st.average += v;
    st.average /= static_cast<double>(values.size());
    for (double v : values) st.variance += (v - st.average) * (v - st.average);
    st.variance /= static_cast<double>(values.size());
    return st;
}

int percentage(double average, long maximum) {
    const double pct = 100.0 * average / static_cast<double>(maximum);
    return static_cast<int>(std::lround(std::clamp(pct, 0.0, 100.0)));
}

}  // namespace

std::optional<NwdafSliceLoad> NwdafSliceLoadCalculator::compute(const NwdafSliceCapacity& slice,
                                                                const std::string& plmn_id,
                                                                const std::vector<NwdafOamScrape>& amf,
                                                                const std::vector<NwdafOamScrape>& smf,
                                                                std::chrono::system_clock::time_point from,
                                                                std::chrono::system_clock::time_point to) {
    NwdafSliceLoad load;
    load.slice = slice;
    if (slice.max_ues > 0) {
        load.ues = stat(amf, UES_METRIC, plmn_id, slice.key(), from, to);
        if (!load.ues) return std::nullopt;
        load.load_level = std::max(load.load_level, percentage(load.ues->average, slice.max_ues));
    }
    if (slice.max_pdu_sessions > 0) {
        load.pdu_sessions = stat(smf, PDU_METRIC, plmn_id, slice.key(), from, to);
        if (!load.pdu_sessions) return std::nullopt;
        load.load_level = std::max(load.load_level,
                                   percentage(load.pdu_sessions->average, slice.max_pdu_sessions));
    }
    return load;
}
