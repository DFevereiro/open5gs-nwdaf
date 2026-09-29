#pragma once
#include "nwdaf_config.hpp"
#include <optional>
#include <set>
#include <string>

// H1.7: supported-features negotiation (TS 29.500 V18.10.0 §6.6.2).

// A TS 29.571 V18.12.0 "SupportedFeatures" bitmask: hexadecimal, most
// significant character first, the last character carrying features 1–4.
// Feature numbers start at 1 and are defined per API.
class NwdafFeatureSet {
public:
    NwdafFeatureSet() = default;
    NwdafFeatureSet(std::initializer_list<unsigned> features) : bits_(features) {}

    // nullopt when the string contains a non-hexadecimal character. An empty
    // string is valid and means "no features".
    static std::optional<NwdafFeatureSet> parse(const std::string& hex);

    // Shortest encoding; "0" when no feature is set.
    std::string toHex() const;

    NwdafFeatureSet intersect(const NwdafFeatureSet& other) const;
    void set(unsigned feature) { bits_.insert(feature); }
    bool has(unsigned feature) const { return bits_.count(feature) > 0; }
    bool empty() const { return bits_.empty(); }
    bool operator==(const NwdafFeatureSet& o) const { return bits_ == o.bits_; }

private:
    std::set<unsigned> bits_;
};

enum class NnwdafApi { AnalyticsInfo, EventsSubscription };

// Feature numbering of TS 29.520 V18.14.0 Table 5.2.8-1 (Nnwdaf_AnalyticsInfo)
// and Table 5.1.8-1 (Nnwdaf_EventsSubscription). The same feature has a
// different number in each API; see docs/3gpp-rel18-compliance.md Appendix A.3.
class NwdafSupportedFeatures {
public:
    // Optional features other than per-analytics support that the failure
    // semantics depend on.
    enum class Feature { EneNA, StatisticsFailure, PredictionError, RoamingAnalytics };

    // Number of `feature` in `api`'s table; nullopt when that API defines none
    // (StatisticsFailure exists only for Nnwdaf_EventsSubscription).
    static std::optional<unsigned> number(NnwdafApi api, Feature feature);

    // Number of the feature that gates analytics `event` in `api`'s table.
    // nullopt when the table defines no such feature: slice load level
    // (SLICE_LOAD_LEVEL / LOAD_LEVEL_INFORMATION), which is base
    // functionality of both APIs; SM_CONGESTION on Nnwdaf_EventsSubscription
    // (open item S-1); or an ID this NWDAF does not map.
    static std::optional<unsigned> eventFeature(NnwdafApi api, const std::string& event);

    // This NWDAF's own features for `api`: the gating feature of every
    // advertised analytics ID. Derived from NwdafAnalyticsCatalogue::
    // rel18Advertised(cfg), so negotiation, request acceptance and the NRF
    // profile all agree, and it depends only on code and configuration.
    static NwdafFeatureSet local(NnwdafApi api, const NwdafConfig& cfg);
};
