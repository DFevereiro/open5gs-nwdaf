#include "nwdaf_analytics_catalogue.hpp"

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
};

std::string NwdafAnalyticsCatalogue::canonicalOperatorId(const std::string& id) {
    // COMP-05 / H1.4: pre-Rel-18 spellings kept working on the operator API.
    if (id == "QoS_SUSTAINABILITY")     return "QOS_SUSTAINABILITY";
    if (id == "REDUNDANT_TRANSMISSION") return "RED_TRANS_EXP";
    return id;
}

std::set<std::string> NwdafAnalyticsCatalogue::rel18Advertised(const NwdafConfig& cfg) {
    std::set<std::string> out;
    for (const auto& id : REL18_IMPLEMENTED) {
        // NfLoadLevelInformation requires nfInstanceId: without a configured
        // instance-ID source (the map, or NRF discovery) the NWDAF cannot
        // produce NF_LOAD truthfully.
        if (id == "NF_LOAD" && cfg.nf_instance_ids.empty() && !cfg.nrf_nf_discovery) continue;
        out.insert(id);
    }
    return out;
}
