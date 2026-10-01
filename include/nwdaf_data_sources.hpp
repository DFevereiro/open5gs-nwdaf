#pragma once
#include "nwdaf_config.hpp"
#include <map>
#include <optional>
#include <string>
#include <vector>

// H1.1: the inputs the Rel-18 analytics are computed from, and the source
// that provides each one in this deployment (COMPAT-01).
//
// The 3GPP interfaces stay the same whatever the core; what a core can feed
// decides what is advertised. An analytics is advertised only when every
// input of one of its alternatives has a configured source, so a core with
// more data enables more analytics with no code change. Today's sources are
// the configuration, the NRF, and the Open5GS backend: the collector's
// metrics, JSON and systemd scraping, an implementation-specific OAM input
// that TS 23.288 V18.13.0 §6.2 allows, used because Open5GS has no NF event
// exposure (O5GS-08). A standard backend (Nnf_EventExposure, TS 28.532 OAM)
// would add sources for the same inputs.
enum class NwdafInput {
    NfInstanceIds,          // NF type → NF instance ID
    NfStatus,               // NF status over time (NRF)
    NfCpu,                  // per-NF CPU load
    UeCountPerSlice,        // registered UEs per S-NSSAI (TS 28.552 RM.RegisteredSubNbrMean)
    PduSessionsPerSlice,    // PDU sessions per S-NSSAI
    SessionSetupCounters,   // PDU session creation requests and failures (TS 28.552 SM.PduSessionCreation*)
    UeLocations,            // per-UE TAI and cell over time
    ServedArea,             // the TAIs the core serves
    SliceCapacity,          // per-slice admission maxima (I-9)
};

class NwdafDataSources {
public:
    // Every input, in declaration order.
    static const std::vector<NwdafInput>& all();
    // Its name in /health ("UE_LOCATIONS").
    static const char* name(NwdafInput input);
    // What it is and the settings that provide it, for the reason an
    // analytics isn't advertised.
    static const char* describe(NwdafInput input);

    // The source configured for the input ("open5gs:amf-ue-info", "nrf",
    // "config"), or nullopt.
    static std::optional<std::string> sourceOf(NwdafInput input, const NwdafConfig& cfg);

    // The inputs of an analytics or a NetworkPerfType: alternatives, each a
    // set of inputs that must all be present.
    using Requirement = std::vector<std::vector<NwdafInput>>;
    static const std::map<std::string, Requirement>& analyticsRequirements();
    static const std::map<std::string, Requirement>& networkPerformanceTypes();

    // True when one alternative has all its inputs.
    static bool satisfied(const Requirement& r, const NwdafConfig& cfg);
    // Empty when satisfied; otherwise what the closest alternatives lack,
    // e.g. "needs UE locations (amf_ue_info_endpoint), or …".
    static std::string missingReason(const Requirement& r, const NwdafConfig& cfg);
};
