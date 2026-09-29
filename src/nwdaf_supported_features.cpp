#include "nwdaf_supported_features.hpp"
#include "nwdaf_analytics_catalogue.hpp"
#include <map>

std::optional<NwdafFeatureSet> NwdafFeatureSet::parse(const std::string& hex) {
    NwdafFeatureSet fs;
    const size_t n = hex.size();
    for (size_t i = 0; i < n; ++i) {
        const char c = hex[n - 1 - i];   // last character = features 1..4
        int v;
        if (c >= '0' && c <= '9')      v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return std::nullopt;
        for (unsigned b = 0; b < 4; ++b)
            if (v & (1 << b)) fs.bits_.insert(static_cast<unsigned>(i * 4 + b + 1));
    }
    return fs;
}

std::string NwdafFeatureSet::toHex() const {
    if (bits_.empty()) return "0";
    const unsigned highest = *bits_.rbegin();
    const size_t chars = (highest + 3) / 4;
    std::string out(chars, '0');
    for (unsigned f : bits_) {
        const size_t idx = chars - 1 - (f - 1) / 4;
        int v = (out[idx] >= 'A') ? out[idx] - 'A' + 10 : out[idx] - '0';
        v |= 1 << ((f - 1) % 4);
        out[idx] = static_cast<char>(v < 10 ? '0' + v : 'A' + v - 10);
    }
    return out;
}

NwdafFeatureSet NwdafFeatureSet::intersect(const NwdafFeatureSet& other) const {
    NwdafFeatureSet out;
    for (unsigned f : bits_) if (other.has(f)) out.bits_.insert(f);
    return out;
}

std::optional<unsigned> NwdafSupportedFeatures::number(NnwdafApi api, Feature feature) {
    const bool info = api == NnwdafApi::AnalyticsInfo;
    switch (feature) {
    case Feature::EneNA:             return info ? 10u : 11u;
    case Feature::PredictionError:   return info ? 48u : 55u;
    case Feature::RoamingAnalytics:  return info ? 47u : 54u;
    case Feature::StatisticsFailure: return info ? std::nullopt : std::optional<unsigned>(53u);
    }
    return std::nullopt;
}

std::optional<unsigned> NwdafSupportedFeatures::eventFeature(NnwdafApi api,
                                                             const std::string& event) {
    // { event, { AnalyticsInfo (Table 5.2.8-1), EventsSubscription (Table 5.1.8-1) } };
    // 0 = no feature defined for that API.
    static const std::map<std::string, std::pair<unsigned, unsigned>> table = {
        {"UE_MOBILITY",         {1, 2}},
        {"UE_COMMUNICATION",    {2, 3}},
        {"NETWORK_PERFORMANCE", {3, 8}},
        {"SERVICE_EXPERIENCE",  {4, 1}},
        {"QOS_SUSTAINABILITY",  {5, 4}},
        {"ABNORMAL_BEHAVIOUR",  {6, 5}},
        {"NF_LOAD",             {8, 7}},
        {"NSI_LOAD_LEVEL",      {9, 9}},    // NsiLoad
        {"SM_CONGESTION",       {15, 0}},   // S-1: no SMCCE bit in Table 5.1.8-1
        {"DISPERSION",          {17, 18}},
        {"RED_TRANS_EXP",       {18, 19}},
    };
    auto it = table.find(event);
    if (it == table.end()) return std::nullopt;
    const unsigned n = api == NnwdafApi::AnalyticsInfo ? it->second.first : it->second.second;
    if (n == 0) return std::nullopt;
    return n;
}

NwdafFeatureSet NwdafSupportedFeatures::local(NnwdafApi api, const NwdafConfig& cfg) {
    NwdafFeatureSet fs;
    for (const auto& id : NwdafAnalyticsCatalogue::rel18Advertised(cfg))
        if (auto n = eventFeature(api, id)) fs.set(*n);
    return fs;
}
