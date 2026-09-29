#pragma once
#include "nwdaf_collector.hpp"
#include "nwdaf_config.hpp"
#include <nlohmann/json.hpp>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// H1.9: the NRF status of one NF instance over the status window, as the
// TS 29.520 NfStatus (percentages; I-8).
struct NwdafNfStatusObservation {
    std::string    nf_type;
    std::string    nf_instance_id;
    nlohmann::json nf_status;   // NfStatus
};

// H1.7 Step 3: Rel-18 output mappings (TS 29.520 V18.14.0) of what the
// Open5GS collectors measure. Pure functions: request validation and the
// operation-specific failure semantics live in NwdafSbiService.
class Nwdaf3gppAdapter {
public:
    // Filters the NWDAF honours for NF_LOAD (EventFilter / EventSubscription
    // nfInstanceIds and nfTypes; EventReportingRequirement maxObjectNbr).
    struct NfLoadQuery {
        std::set<std::string> nf_instance_ids;   // empty = no filter
        std::set<std::string> nf_types;          // empty = no filter
        std::optional<size_t> max_objects;
    };

    // NfLoadLevelInformation for every NF instance that has a load measure
    // and matches the query:
    // - nfCpuUsage, the measured CPU percentage of one core (0–100), for a
    //   running monitored NF with a known instance ID (I-5);
    // - nfStatus, from the NRF (`statuses`, H1.9), for every instance the NRF
    //   listed within the status window.
    // An instance with both gets both.
    static nlohmann::json nfLoadLevelInfos(const std::vector<NfMetric>& metrics,
                                           const std::map<std::string, std::string>& nf_instance_ids,
                                           const std::vector<NwdafNfStatusObservation>& statuses,
                                           const NfLoadQuery& query);

    // NfStatus from counts of polls per NFStatus value (I-8). REGISTERED maps
    // to statusRegistered, UNDISCOVERABLE to statusUndiscoverable, and absent
    // from the NRF to statusUnregistered. SUSPENDED and CANARY_RELEASE count
    // towards the total only: NfStatus has no attribute for them. Values are
    // SamplingRatio (1–100), so a state below 0.5 % is omitted. nullopt when
    // no attribute would remain.
    static std::optional<nlohmann::json> nfStatus(const std::map<std::string, size_t>& polls_per_status,
                                                  size_t total_polls);
};
