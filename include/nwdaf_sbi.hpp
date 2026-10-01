#pragma once
#include "nwdaf_3gpp_adapter.hpp"
#include "nwdaf_analytics.hpp"
#include "nwdaf_config.hpp"
#include "nwdaf_nf_monitor.hpp"
#include "nwdaf_schema_validator.hpp"
#include "nwdaf_subscription.hpp"
#include "nwdaf_supported_features.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// H1.7: the 3GPP Nnwdaf service-based interface — Nnwdaf_AnalyticsInfo and
// Nnwdaf_EventsSubscription, TS 29.520 V18.14.0 — independent of the HTTP
// transport, so the HTTP/1.1 and HTTP/2 (H1.8) servers share one
// implementation. Requests are validated against the official Rel-18
// artifacts pinned in docs/frozen-standards.md before any semantics apply.
//
// Rules taken from the specification prose are pinned, with their clauses, in
// docs/3gpp-rel18-compliance.md Appendix A; interpretations are recorded
// there as I-1..I-13.

struct SbiRequest {
    std::string method;                              // "GET", "POST", ...
    std::string path;                                // without the query string
    std::multimap<std::string, std::string> query;   // percent-decoded
    std::map<std::string, std::string> headers;      // lower-case names
    std::string body;
    std::string api_root;   // "{scheme}://{authority}" the consumer addressed
    // H1.10: NF Instance ID in the consumer's TLS client certificate ("" = none).
    std::string client_nf_instance_id;
    // H1.10 (I-6): the access token's analyticsIdList, when it carried one.
    std::optional<std::set<std::string>> authorized_analytics;
};

// The measurements event reports are computed from, gathered once per round.
struct NwdafReportInputs {
    std::vector<NfMetric>                  metrics;
    std::map<std::string, std::string>     nf_instance_ids;
    std::vector<NwdafNfStatusObservation>  statuses;
    std::vector<NwdafOamScrape>            amf_oam, smf_oam;   // H1.2
    std::shared_ptr<const NwdafUeLocationTracker> ue_locations;   // H1.1; null = none
    std::vector<NwdafNfLoadScrape>         nf_history;         // H1.7: for NF_LOAD predictions
};

struct SbiResponse {
    int status = 200;
    std::string content_type;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

class NwdafSbiService {
public:
    static constexpr const char* ANALYTICS_INFO_ROOT     = "/nnwdaf-analyticsinfo/v1";
    static constexpr const char* EVENTS_SUBSCRIPTION_ROOT = "/nnwdaf-eventssubscription/v1";

    // `nf_monitor` supplies NF instance IDs and NRF status (H1.9); null = the
    // configured IDs only.
    NwdafSbiService(NwdafAnalyticsEngine& engine,
                    NwdafSubscriptionStore& subs,
                    const NwdafConfig& config,
                    std::shared_ptr<NwdafNfMonitor> nf_monitor = nullptr);

    // True when `path` names one of this service's resources.
    static bool handles(const std::string& path);

    // Serve one request. `path` must satisfy handles().
    SbiResponse dispatch(const SbiRequest& req);

    // False when the official OpenAPI artifacts could not be loaded from
    // openapi_3gpp_dir; the 3GPP interfaces then answer 500 SYSTEM_FAILURE
    // rather than serve unvalidated requests.
    bool available() const { return available_; }

    // RFC 3339 date-time (TS 29.571 DateTime), including fractional seconds
    // and UTC offsets. nullopt when malformed.
    static std::optional<std::chrono::system_clock::time_point>
    parseDateTime(const std::string& s);
    // TS 29.571 DateTime (RFC 3339, UTC, whole seconds).
    static std::string formatDateTime(std::chrono::system_clock::time_point tp);

private:
    SbiResponse getAnalytics(const SbiRequest& req);
    SbiResponse createSubscription(const SbiRequest& req);
    SbiResponse modifySubscription(const SbiRequest& req, const std::string& id);
    SbiResponse deleteSubscription(const std::string& id);

    // Shared POST/PUT processing: schema, prose and capability checks on an
    // NnwdafEventsSubscription body. Returns an error response, or nullopt
    // with `accepted` / `failed` filled in.
    struct SubscriptionOutcome {
        nlohmann::json request;
        nlohmann::json failed = nlohmann::json::array();   // FailureEventInfo[]
        std::vector<size_t> accepted;                      // indices into eventSubscriptions
        NwdafFeatureSet negotiated;
    };
    std::optional<SbiResponse> evaluateSubscription(const SbiRequest& req,
                                                    SubscriptionOutcome& out);

public:
    // Why an NF_LOAD or slice load request cannot be served as asked; each
    // operation maps a kind to its own spec-mandated response (Appendix A.2 / A.4).
    struct Rejection {
        enum Kind {
            MandatoryMissing,     // a conditionally mandatory input is absent (tgt-ue, the slices, thresholds)
            MandatoryIncorrect,   // it is present but unusable (neither anyUe nor supis, …)
            Unsupported,       // a relevant attribute this NWDAF does not implement
            UnavailableData,   // past statistics requested; data not held (UNAVAILABLE_DATA)
        } kind;
        std::string where;     // query parameter name or JSON Pointer
        std::string reason;
    };

    // Interpret the NF_LOAD inputs (TS 29.520 V18.14.0 §4.2.2.2.2 / §4.3.2.2).
    // `target`, `filter` and `req` may be null. `*_at` label where each came
    // from, for the error. Fills `query` when nullopt is returned. A future
    // analytics target period asks for a prediction (I-13).
    static std::optional<Rejection> interpretNfLoad(
        const nlohmann::json* target, const std::string& target_at,
        const nlohmann::json& filter, const std::string& filter_at,
        const nlohmann::json* req, const std::string& req_at,
        const NwdafConfig& config, Nwdaf3gppAdapter::NfLoadQuery& query);

    // H1.2: interpret the slice load inputs of SLICE_LOAD_LEVEL / NSI_LOAD_LEVEL
    // (TS 29.520 V18.14.0 §4.2.2.2.2, §4.3.2.2). `filter` is the AnalyticsInfo
    // event-filter or the EventSubscription itself (null when absent); `req`
    // the reporting requirements (may be null). NSI_LOAD_LEVEL is predicted
    // for a future period; SLICE_LOAD_LEVEL is not, since its Stage 3 type
    // has no confidence (I-13).
    static std::optional<Rejection> interpretSliceLoad(
        const std::string& event,
        const nlohmann::json* filter, const std::string& filter_at,
        const nlohmann::json* req, const std::string& req_at,
        const NwdafConfig& config, Nwdaf3gppAdapter::SliceQuery& query);

    // H1.4: interpret the NETWORK_PERFORMANCE inputs (TS 29.520 V18.14.0
    // §4.2.2.2.2, §4.3.2.2): target UE (anyUe only), networkArea (any TAs
    // or cells for NUM_OF_UE from the AMF's UE list, else the whole served
    // area, I-11), and the types — event-filter nwPerfTypes on
    // AnalyticsInfo, nwPerfRequs on a subscription. `filter` is the
    // event-filter or the EventSubscription (null when absent).
    static std::optional<Rejection> interpretNetworkPerformance(
        const nlohmann::json* target, const std::string& target_at,
        const nlohmann::json* filter, const std::string& filter_at,
        const nlohmann::json* req, const std::string& req_at,
        bool subscription, const NwdafConfig& config,
        Nwdaf3gppAdapter::NwPerfQuery& query);
    static std::optional<Rejection> nwPerfHistoryCovers(const Nwdaf3gppAdapter::NwPerfQuery& query,
                                                        const NwdafReportInputs& in,
                                                        const NwdafConfig& config,
                                                        const std::string& req_at);

    // H1.1: interpret the UE_MOBILITY inputs (TS 29.520 V18.14.0 §4.2.2.2.2,
    // §4.3.2.2; I-12): the target UEs (supis; intGroupIds can't be resolved),
    // the area of interest (networkArea tais / ncgis) and the period.
    // `filter` is the event-filter or the EventSubscription (null when absent).
    static std::optional<Rejection> interpretUeMobility(
        const nlohmann::json* target, const std::string& target_at,
        const nlohmann::json* filter, const std::string& filter_at,
        const nlohmann::json* req, const std::string& req_at,
        Nwdaf3gppAdapter::UeMobilityQuery& query);
    static std::optional<Rejection> ueMobilityHistoryCovers(const Nwdaf3gppAdapter::UeMobilityQuery& query,
                                                            const NwdafReportInputs& in,
                                                            const NwdafConfig& config,
                                                            const std::string& req_at);

    // H1.2: UNAVAILABLE_DATA when the requested period starts before the
    // held metrics history.
    static std::optional<Rejection> sliceHistoryCovers(const Nwdaf3gppAdapter::SliceQuery& query,
                                                       const NwdafReportInputs& in,
                                                       const NwdafConfig& config,
                                                       const std::string& req_at);

    // The EventNotifications for one accepted EventSubscription, from current
    // measurements; empty when no analytics data exists now. Shared by the
    // immediate report and the notifier. SLICE_LOAD_LEVEL yields one per
    // distinct load level (EventNotification carries a single level).
    static std::vector<nlohmann::json> eventReports(const nlohmann::json& event_subscription,
                                                    const NwdafReportInputs& in,
                                                    const NwdafConfig& config);

    // Current measurements for event reports; the notifier gathers them the
    // same way once per poll.
    static NwdafReportInputs gatherInputs(const NwdafAnalyticsEngine& engine, const NwdafNfMonitor& nf_monitor);
    NwdafReportInputs inputs() const;

    // H1.7: THRESHOLD / ON_EVENT_DETECTION reporting (TS 29.520 V18.14.0
    // §4.2.2.2.2, Table 5.1.6.2.3-1; TS 23.288 §6.1.3; I-10).
    //
    // True when an event is reported on threshold crossings: evtReq
    // notifMethod ON_EVENT_DETECTION, or the event's notificationMethod
    // THRESHOLD or omitted (its default).
    static bool thresholdMode(const nlohmann::json& evt_req, const nlohmann::json& event_subscription);
    // The notification method in effect for one event: evtReq's notifMethod
    // supersedes the event's own (§4.2.2.2.2 NOTE 1), whose default is
    // THRESHOLD (Table 5.1.6.2.3-1 NOTE 2).
    static std::string effectiveMethod(const nlohmann::json& evt_req, const nlohmann::json& event_subscription);

    // The event's reporting thresholds: nfLoadLvlThds (NF_LOAD, nfCpuUsage
    // only), loadLevelThreshold (SLICE_LOAD_LEVEL), nsiLevelThrds
    // (NSI_LOAD_LEVEL). MandatoryMissing when absent (Table 5.1.6.2.3-1
    // NOTE 4), Unsupported for a level this NWDAF does not measure.
    static std::optional<Rejection> interpretThresholds(const nlohmann::json& event_subscription,
                                                        const std::string& at);

    // Last reported value per entity (NF instance ID or S-NSSAI key) of one
    // event subscription.
    using ThresholdState = std::map<std::string, int>;

    // The EventNotifications for the entities whose value crossed a threshold
    // in the matching direction since the previous call; `state` then holds
    // the current values. An entity seen for the first time only sets its
    // baseline (I-10).
    static std::vector<nlohmann::json> thresholdReports(const nlohmann::json& event_subscription,
                                                        const NwdafReportInputs& in,
                                                        const NwdafConfig& config,
                                                        ThresholdState& state);

private:
    SbiResponse nfLoadInfo(std::map<std::string, nlohmann::json>& values,
                           const std::optional<NwdafFeatureSet>& consumer,
                           const NwdafFeatureSet& local);
    SbiResponse ueMobilityInfo(std::map<std::string, nlohmann::json>& values,
                               const std::optional<NwdafFeatureSet>& consumer,
                               const NwdafFeatureSet& local);
    SbiResponse nwPerfInfo(std::map<std::string, nlohmann::json>& values,
                           const std::optional<NwdafFeatureSet>& consumer,
                           const NwdafFeatureSet& local);
    SbiResponse sliceLoadInfo(const std::string& event,
                              std::map<std::string, nlohmann::json>& values,
                              const std::optional<NwdafFeatureSet>& consumer,
                              const NwdafFeatureSet& local);

    NwdafAnalyticsEngine&   engine_;
    NwdafSubscriptionStore& subs_;
    NwdafConfig             config_;
    NwdafSchemaValidator    validator_;
    std::shared_ptr<NwdafNfMonitor> nf_monitor_;
    bool                    available_ = false;
};
