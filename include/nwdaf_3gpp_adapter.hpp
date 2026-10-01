#pragma once
#include "nwdaf_collector.hpp"
#include "nwdaf_config.hpp"
#include "nwdaf_slice_load.hpp"
#include "nwdaf_network_performance.hpp"
#include "nwdaf_ue_location.hpp"
#include "ml/holt_forecaster.hpp"
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
        // H1.7: a future analytics target period asks for a prediction (I-13).
        bool prediction = false;
        std::optional<std::chrono::system_clock::time_point> from, to;
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
        bool prediction = false;            // H1.7: [from, to] is in the future (I-13)
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

    // H1.7: predictions (I-13). The forecaster settings from the configuration.
    static HoltForecaster::Params forecastParams(const NwdafConfig& cfg);
    // NfLoadLevelInformation predictions: nfCpuUsage and confidence per NF
    // instance with a CPU history, for [query.from, query.to].
    static nlohmann::json nfLoadPredictions(const NwdafConfig& cfg,
                                            const std::vector<NwdafNfLoadScrape>& history,
                                            const std::map<std::string, std::string>& nf_instance_ids,
                                            const NfLoadQuery& query,
                                            std::chrono::system_clock::time_point now);
    // NsiLoadLevelInfo predictions: loadLevelInformation and confidence per
    // configured slice the query selects, from its load level per scrape.
    static nlohmann::json nsiLoadPredictions(const NwdafConfig& cfg, const SliceQuery& query,
                                             const std::vector<NwdafOamScrape>& amf,
                                             const std::vector<NwdafOamScrape>& smf,
                                             std::chrono::system_clock::time_point now);

    // H1.4: which network performance types and period a request covers.
    struct NwPerfQuery {
        std::vector<std::string> types;   // NetworkPerfType values, in request order
        nlohmann::json area;              // the requested networkArea
        std::optional<std::chrono::system_clock::time_point> from, to;   // default: the last network_performance_window_seconds
    };

    // The types this NWDAF computes, given the configured sources (I-11):
    // NUM_OF_UE from the AMF's UE list, or from the AMF metrics endpoint for
    // the served area; SESS_SUCC_RATIO from the SMF's, for the served area.
    static bool nwPerfTypeAvailable(const std::string& type, const NwdafConfig& cfg);
    // True when the type is computed for any area (tais / ncgis) rather than
    // the whole served area only: NUM_OF_UE from the AMF's UE list.
    static bool nwPerfPerArea(const std::string& type, const NwdafConfig& cfg);

    // TS 29.571 Tai as "<mcc>-<mnc>-<6 hex digit tac>".
    static std::string taiKey(const nlohmann::json& tai);
    // NetworkAreaInfo of the configured served area (tais).
    static nlohmann::json servedArea(const NwdafConfig& cfg);
    // True when `network_area` (NetworkAreaInfo) lists every served TAI: the
    // measurements are per AMF, so they describe the whole served area only.
    static bool coversServedArea(const nlohmann::json& network_area, const NwdafConfig& cfg);

    // NetworkPerfInfo per requested type that has data for the period (I-11).
    // A per-area type reports the requested area; the others the served area.
    // `ues` is the AMF UE list (null = none).
    static nlohmann::json nwPerfInfos(const NwdafConfig& cfg, const NwPerfQuery& query,
                                      const std::vector<NwdafOamScrape>& amf,
                                      const std::vector<NwdafOamScrape>& smf,
                                      const NwdafUeLocationTracker* ues,
                                      std::chrono::system_clock::time_point now);
    // The average number of UEs in the area over [from, to] from the UE
    // list: UE-seconds in the area ÷ the period, which starts no earlier
    // than the first poll (I-11). nullopt when the period isn't held.
    static std::optional<double> ueCountInArea(const NwdafUeLocationTracker& ues, const nlohmann::json& area,
                                               std::chrono::system_clock::time_point from,
                                               std::chrono::system_clock::time_point to,
                                               std::chrono::system_clock::time_point now);

    // H1.1: which UEs, area and period a UE_MOBILITY request covers (I-12).
    struct UeMobilityQuery {
        std::vector<std::string> supis;
        std::optional<nlohmann::json> area;       // networkArea: tais and/or ncgis
        std::optional<size_t> max_objects;        // time slots, or locations of a group
        std::optional<std::chrono::system_clock::time_point> from, to;   // default: the last ue_mobility_window_seconds
    };

    // TS 29.571 UserLocation with nrLocation (tai, ncgi) of an AMF location.
    static nlohmann::json userLocation(const NwdafUeLocation& loc);
    // True when the location is in the NetworkAreaInfo (its tais or ncgis).
    static bool inArea(const NwdafUeLocation& loc, const nlohmann::json& network_area);

    // AnalyticsData.ueMobs / EventNotification.ueMobs (I-12). One SUPI: one
    // UeMobility per stay, in time order (TS 23.288 Table 6.7.2.3-1: the time
    // slot is the stay, its single location the TA and cell). Several SUPIs
    // are one group: a single time slot for the period whose locations carry
    // the time-averaged percentage of the group's UEs there (ratio),
    // highest first. Empty when the UEs have no location in the period.
    static nlohmann::json ueMobilities(const NwdafConfig& cfg, const UeMobilityQuery& query,
                                       const NwdafUeLocationTracker& tracker,
                                       std::chrono::system_clock::time_point now);
};
