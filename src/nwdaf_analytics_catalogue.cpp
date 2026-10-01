#include "nwdaf_analytics_catalogue.hpp"
#include "nwdaf_data_sources.hpp"

const std::set<std::string> NwdafAnalyticsCatalogue::KNOWN_REL18 = {
    "SLICE_LOAD_LEVEL", "NETWORK_PERFORMANCE", "NF_LOAD", "SERVICE_EXPERIENCE",
    "UE_MOBILITY", "UE_COMMUNICATION", "QOS_SUSTAINABILITY", "ABNORMAL_BEHAVIOUR",
    "USER_DATA_CONGESTION", "NSI_LOAD_LEVEL", "DN_PERFORMANCE", "DISPERSION",
    "RED_TRANS_EXP", "WLAN_PERFORMANCE", "SM_CONGESTION", "PFD_DETERMINATION",
    "PDU_SESSION_TRAFFIC", "E2E_DATA_VOL_TRANS_TIME", "MOVEMENT_BEHAVIOUR",
    "LOC_ACCURACY", "RELATIVE_PROXIMITY"
};

const std::set<std::string> NwdafAnalyticsCatalogue::OPERATOR_IDS = {
    "NF_LOAD", "UE_MOBILITY", "UE_COMMUNICATION",
    "ABNORMAL_BEHAVIOUR", "QOS_SUSTAINABILITY",
    "SERVICE_EXPERIENCE", "NETWORK_PERFORMANCE",
    // H1.4 — Rel-17/18 catalogue completion
    "SM_CONGESTION", "RED_TRANS_EXP", "DISPERSION"
};

// An ID is added here together with its official-schema conformance test
// (H1.7, Step 3 of the Rel-18 plan).
const std::set<std::string> NwdafAnalyticsCatalogue::REL18_IMPLEMENTED = {
    "NF_LOAD",
    "SLICE_LOAD_LEVEL",   // H1.2, I-9
    "NSI_LOAD_LEVEL",     // H1.2, I-9 (S-NSSAI level, no NSI IDs)
    "NETWORK_PERFORMANCE",   // H1.4, I-11 (NUM_OF_UE, SESS_SUCC_RATIO; whole served area)
    "UE_MOBILITY",           // H1.1, I-12 (SUPIs; TA and cell from the AMF /ue-info)
};

std::string NwdafAnalyticsCatalogue::fromAnalyticsInfoEventId(const std::string& event_id) {
    // Nnwdaf_AnalyticsInfo names slice load level LOAD_LEVEL_INFORMATION
    // (EventId); Nnwdaf_EventsSubscription and the NRF call it
    // SLICE_LOAD_LEVEL (NwdafEvent). The other shared values are identical.
    return event_id == "LOAD_LEVEL_INFORMATION" ? "SLICE_LOAD_LEVEL" : event_id;
}

std::string NwdafAnalyticsCatalogue::toAnalyticsInfoEventId(const std::string& nwdaf_event) {
    return nwdaf_event == "SLICE_LOAD_LEVEL" ? "LOAD_LEVEL_INFORMATION" : nwdaf_event;
}

std::string NwdafAnalyticsCatalogue::canonicalOperatorId(const std::string& id) {
    // COMP-05 / H1.4: pre-Rel-18 spellings kept working on the operator API.
    if (id == "QoS_SUSTAINABILITY")     return "QOS_SUSTAINABILITY";
    if (id == "REDUNDANT_TRANSMISSION") return "RED_TRANS_EXP";
    return id;
}

std::map<std::string, std::string> NwdafAnalyticsCatalogue::rel18NotAdvertised(const NwdafConfig& cfg) {
    // COMPAT-01: an implemented ID is advertised when the inputs of one of
    // its alternatives all have a configured source (NwdafDataSources).
    std::map<std::string, std::string> out;
    for (const auto& id : REL18_IMPLEMENTED) {
        const auto& table = NwdafDataSources::analyticsRequirements();
        const auto it = table.find(id);
        // An implemented ID without an inputs entry is a defect: not advertised.
        const std::string reason = it == table.end() ? "no inputs are defined for it"
                                                     : NwdafDataSources::missingReason(it->second, cfg);
        if (!reason.empty()) out[id] = reason;
    }
    return out;
}

std::set<std::string> NwdafAnalyticsCatalogue::rel18Advertised(const NwdafConfig& cfg) {
    const auto withheld = rel18NotAdvertised(cfg);
    std::set<std::string> out;
    for (const auto& id : REL18_IMPLEMENTED)
        if (!withheld.count(id)) out.insert(id);
    return out;
}
