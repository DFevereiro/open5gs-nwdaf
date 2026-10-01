#include "nwdaf_data_sources.hpp"
#include <algorithm>
#include <cstdint>

using Input = NwdafInput;

const std::vector<NwdafInput>& NwdafDataSources::all() {
    static const std::vector<NwdafInput> inputs = {
        Input::NfInstanceIds, Input::NfStatus, Input::NfCpu, Input::UeCountPerSlice, Input::PduSessionsPerSlice,
        Input::SessionSetupCounters, Input::UeLocations, Input::ServedArea, Input::SliceCapacity};
    return inputs;
}

const char* NwdafDataSources::name(NwdafInput input) {
    switch (input) {
    case Input::NfInstanceIds:        return "NF_INSTANCE_IDS";
    case Input::NfStatus:             return "NF_STATUS";
    case Input::NfCpu:                return "NF_CPU";
    case Input::UeCountPerSlice:      return "UE_COUNT_PER_SLICE";
    case Input::PduSessionsPerSlice:  return "PDU_SESSIONS_PER_SLICE";
    case Input::SessionSetupCounters: return "SESSION_SETUP_COUNTERS";
    case Input::UeLocations:          return "UE_LOCATIONS";
    case Input::ServedArea:           return "SERVED_AREA";
    case Input::SliceCapacity:        return "SLICE_CAPACITY";
    }
    return "?";
}

const char* NwdafDataSources::describe(NwdafInput input) {
    switch (input) {
    case Input::NfInstanceIds:        return "NF instance IDs (nf_instance_ids or nrf_nf_discovery)";
    case Input::NfStatus:             return "NF status (nrf_nf_discovery)";
    case Input::NfCpu:                return "NF CPU load (nf_service_names)";
    case Input::UeCountPerSlice:      return "UEs per slice (oam_metrics_endpoints.AMF)";
    case Input::PduSessionsPerSlice:  return "PDU sessions per slice (oam_metrics_endpoints.SMF)";
    case Input::SessionSetupCounters: return "PDU session setup counters (oam_metrics_endpoints.SMF)";
    case Input::UeLocations:          return "UE locations (amf_ue_info_endpoint)";
    case Input::ServedArea:           return "the served area (served_tai_list)";
    case Input::SliceCapacity:        return "slice capacities (slice_capacity)";
    }
    return "?";
}

std::optional<std::string> NwdafDataSources::sourceOf(NwdafInput input, const NwdafConfig& cfg) {
    const bool amf = cfg.oam_metrics_endpoints.count("AMF") > 0;
    const bool smf = cfg.oam_metrics_endpoints.count("SMF") > 0;
    switch (input) {
    case Input::NfInstanceIds:
        if (!cfg.nf_instance_ids.empty()) return std::string("config");
        if (cfg.nrf_nf_discovery) return std::string("nrf");
        return std::nullopt;
    case Input::NfStatus:
        return cfg.nrf_nf_discovery ? std::optional<std::string>("nrf") : std::nullopt;
    case Input::NfCpu:
        return cfg.nf_service_names.empty() ? std::nullopt : std::optional<std::string>("open5gs:systemd-proc");
    case Input::UeCountPerSlice:
        return amf ? std::optional<std::string>("open5gs:amf-metrics") : std::nullopt;
    case Input::PduSessionsPerSlice:
    case Input::SessionSetupCounters:
        return smf ? std::optional<std::string>("open5gs:smf-metrics") : std::nullopt;
    case Input::UeLocations:
        return cfg.amf_ue_info_endpoint.empty() ? std::nullopt : std::optional<std::string>("open5gs:amf-ue-info");
    case Input::ServedArea:
        return cfg.served_tai_list.empty() ? std::nullopt : std::optional<std::string>("config");
    case Input::SliceCapacity:
        return cfg.slice_capacity.empty() ? std::nullopt : std::optional<std::string>("config");
    }
    return std::nullopt;
}

const std::map<std::string, NwdafDataSources::Requirement>& NwdafDataSources::analyticsRequirements() {
    // NfLoadLevelInformation requires nfInstanceId (A.1); slice load level is
    // occupancy of a configured capacity, from UE or session counts (I-9);
    // NETWORK_PERFORMANCE needs one of its types (I-11); UE_MOBILITY the UE
    // locations (I-12).
    static const std::map<std::string, Requirement> table = {
        {"NF_LOAD",             {{Input::NfInstanceIds}}},
        {"SLICE_LOAD_LEVEL",    {{Input::SliceCapacity, Input::UeCountPerSlice},
                                 {Input::SliceCapacity, Input::PduSessionsPerSlice}}},
        {"NSI_LOAD_LEVEL",      {{Input::SliceCapacity, Input::UeCountPerSlice},
                                 {Input::SliceCapacity, Input::PduSessionsPerSlice}}},
        {"NETWORK_PERFORMANCE", {{Input::UeLocations},
                                 {Input::ServedArea, Input::UeCountPerSlice},
                                 {Input::ServedArea, Input::SessionSetupCounters}}},
        {"UE_MOBILITY",         {{Input::UeLocations}}},
    };
    return table;
}

const std::map<std::string, NwdafDataSources::Requirement>& NwdafDataSources::networkPerformanceTypes() {
    // I-11: NUM_OF_UE from the UE list for any area, or from the metrics for
    // the whole served area; SESS_SUCC_RATIO from the metrics only.
    static const std::map<std::string, Requirement> table = {
        {"NUM_OF_UE",       {{Input::UeLocations}, {Input::ServedArea, Input::UeCountPerSlice}}},
        {"SESS_SUCC_RATIO", {{Input::ServedArea, Input::SessionSetupCounters}}},
    };
    return table;
}

bool NwdafDataSources::satisfied(const Requirement& r, const NwdafConfig& cfg) {
    return missingReason(r, cfg).empty();
}

std::string NwdafDataSources::missingReason(const Requirement& r, const NwdafConfig& cfg) {
    std::vector<std::vector<NwdafInput>> lacking;
    size_t fewest = SIZE_MAX;
    for (const auto& alternative : r) {
        std::vector<NwdafInput> l;
        for (auto input : alternative)
            if (!sourceOf(input, cfg)) l.push_back(input);
        if (l.empty()) return "";
        fewest = std::min(fewest, l.size());
        lacking.push_back(std::move(l));
    }
    std::string out;
    for (const auto& l : lacking) {
        if (l.size() != fewest) continue;
        std::string alt;
        for (auto input : l) alt += (alt.empty() ? "" : " and ") + std::string(describe(input));
        out += (out.empty() ? "needs " : ", or ") + alt;
    }
    return out;
}
