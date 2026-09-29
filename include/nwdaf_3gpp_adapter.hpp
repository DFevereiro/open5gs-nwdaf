#pragma once
#include "nwdaf_collector.hpp"
#include "nwdaf_config.hpp"
#include "nwdaf_slice_load.hpp"
#include <chrono>
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

    // H1.2: which slices and period a slice load request covers.
    struct SliceQuery {
        bool any = false;                   // anySlice
        std::set<std::string> keys;         // requested S-NSSAIs, as NwdafSliceCapacity::key()
        std::optional<std::chrono::system_clock::time_point> from, to;   // default: the last slice_load_window_seconds
    };

    // TS 29.571 Snssai ↔ NwdafSliceCapacity::key() ("1-000001", "1").
    static std::string snssaiKey(const nlohmann::json& snssai);
    static nlohmann::json snssai(const NwdafSliceCapacity& slice);

    // The load of every configured slice the query selects and that has data
    // for the period (I-9). A requested slice without a configured capacity
    // has no load level, so it is absent.
    static std::vector<NwdafSliceLoad> sliceLoads(const NwdafConfig& cfg, const SliceQuery& query,
                                                  const std::vector<NwdafOamScrape>& amf,
                                                  const std::vector<NwdafOamScrape>& smf,
                                                  std::chrono::system_clock::time_point now);

    // AnalyticsData.sliceLoadLevelInfos: one SliceLoadLevelInformation per slice.
    static nlohmann::json sliceLoadLevelInfos(const std::vector<NwdafSliceLoad>& loads);
    // EventNotification.sliceLoadLevelInfo carries one SliceLoadLevelInformation,
    // whose single level applies to all its snssais: one per distinct level.
    static std::vector<nlohmann::json> sliceLoadLevelGroups(const std::vector<NwdafSliceLoad>& loads);
    // NsiLoadLevelInfo per slice, without nsiId (no network slice instances)
    // and without the NsiLoadExt attributes.
    static nlohmann::json nsiLoadLevelInfos(const std::vector<NwdafSliceLoad>& loads);
};
