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
// there as I-1..I-3.

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
            TargetMissing,     // conditionally mandatory target absent (tgt-ue; the slices)
            TargetIncorrect,   // target present but unusable (neither anyUe nor supis; …)
            Unsupported,       // a relevant attribute this NWDAF does not implement
            UnavailableData,   // past statistics requested; data not held (UNAVAILABLE_DATA)
        } kind;
        std::string where;     // query parameter name or JSON Pointer
        std::string reason;
    };

    // Interpret the NF_LOAD inputs (TS 29.520 V18.14.0 §4.2.2.2.2 / §4.3.2.2).
    // `target`, `filter` and `req` may be null. `*_at` label where each came
    // from, for the error. Fills `query` when nullopt is returned.
    static std::optional<Rejection> interpretNfLoad(
        const nlohmann::json* target, const std::string& target_at,
        const nlohmann::json& filter, const std::string& filter_at,
        const nlohmann::json* req, const std::string& req_at,
        Nwdaf3gppAdapter::NfLoadQuery& query);

    // H1.2: interpret the slice load inputs of SLICE_LOAD_LEVEL / NSI_LOAD_LEVEL
    // (TS 29.520 V18.14.0 §4.2.2.2.2, §4.3.2.2). `filter` is the AnalyticsInfo
    // event-filter or the EventSubscription itself (null when absent); `req`
    // the reporting requirements (may be null).
    static std::optional<Rejection> interpretSliceLoad(
        const std::string& event,
        const nlohmann::json* filter, const std::string& filter_at,
        const nlohmann::json* req, const std::string& req_at,
        Nwdaf3gppAdapter::SliceQuery& query);

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

    // Current measurements for event reports.
    NwdafReportInputs inputs() const;

private:
    SbiResponse nfLoadInfo(std::map<std::string, nlohmann::json>& values,
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
