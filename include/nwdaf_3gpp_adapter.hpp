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

    // NfLoadLevelInformation for every monitored NF that has a configured
    // instance ID, is running (a load measurement exists) and matches the
    // query. nfCpuUsage is the measured CPU percentage of one core (0–100).
    // nfStatus is deliberately not reported: it describes NRF registration
    // status, which the collectors do not observe.
    static nlohmann::json nfLoadLevelInfos(const std::vector<NfMetric>& metrics,
                                           const std::map<std::string, std::string>& nf_instance_ids,
                                           const NfLoadQuery& query);
};
