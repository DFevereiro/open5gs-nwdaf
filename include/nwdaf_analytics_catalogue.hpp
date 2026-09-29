#pragma once
#include "nwdaf_config.hpp"
#include <set>
#include <string>

// H1.7: explicit analytics-ID catalogue.
//
// A single VALID_ANALYTICS_IDS set used to drive input validation, /health,
// the OpenAPI enum and — implicitly — what a 3GPP consumer could expect. Those
// roles are separated here so the NRF only ever advertises what the 3GPP
// interface can truthfully provide (docs/3gpp-rel18-compliance.md §4).
//
// Invariant, enforced by tests/test_analytics_catalogue.cpp:
//     rel18Advertised(cfg) ⊆ REL18_IMPLEMENTED ⊆ KNOWN_REL18
//     OPERATOR_IDS ⊆ KNOWN_REL18
class NwdafAnalyticsCatalogue {
public:
    // The Rel-18 NwdafEvent enum at the frozen baseline
    // (TS29520_Nnwdaf_EventsSubscription.yaml at the commit pinned in
    // docs/frozen-standards.md). Recognised on the 3GPP interfaces.
    static const std::set<std::string> KNOWN_REL18;

    // IDs served by the Open5GS operator API (/nwdaf-analytics/v1), in their
    // canonical Rel-18 spelling.
    static const std::set<std::string> OPERATOR_IDS;

    // IDs with a Rel-18 output mapping (full or partial) on the 3GPP
    // interfaces. Grows as each mapping lands with its conformance test.
    static const std::set<std::string> REL18_IMPLEMENTED;

    // Operator API input only: maps the legacy spellings QoS_SUSTAINABILITY
    // and REDUNDANT_TRANSMISSION to their Rel-18 IDs and returns any other
    // input unchanged. Never applied on the 3GPP interfaces, where only the
    // official enum spelling is valid.
    static std::string canonicalOperatorId(const std::string& id);

    // The NwdafEvent that a Nnwdaf_AnalyticsInfo EventId value denotes:
    // LOAD_LEVEL_INFORMATION is SLICE_LOAD_LEVEL; other values are the same.
    static std::string fromAnalyticsInfoEventId(const std::string& event_id);

    // IDs to advertise in the NRF profile (nwdafInfo). A function of code and
    // configuration only — never of transient data availability — so the
    // advertisement does not flap when a data source or the NRF is briefly
    // unavailable; such failures surface as analytics failures instead.
    static std::set<std::string> rel18Advertised(const NwdafConfig& cfg);
};
