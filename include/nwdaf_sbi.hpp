#pragma once
#include "nwdaf_3gpp_adapter.hpp"
#include "nwdaf_analytics.hpp"
#include "nwdaf_config.hpp"
#include "nwdaf_schema_validator.hpp"
#include "nwdaf_subscription.hpp"
#include "nwdaf_supported_features.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <map>
#include <optional>
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

    NwdafSbiService(NwdafAnalyticsEngine& engine,
                    NwdafSubscriptionStore& subs,
                    const NwdafConfig& config);

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
    // Why an NF_LOAD request cannot be served as asked; each operation maps a
    // kind to its own spec-mandated response (Appendix A.2 / A.4).
    struct Rejection {
        enum Kind {
            TargetMissing,     // tgt-ue / tgtUe absent (conditionally mandatory)
            TargetIncorrect,   // neither anyUe nor supis, or a target form not implemented
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

    // The EventNotification for one accepted EventSubscription, from current
    // measurements; nullopt when no analytics data exists now. Shared by the
    // immediate report and the notifier.
    static std::optional<nlohmann::json> eventReport(const nlohmann::json& event_subscription,
                                                     const std::vector<NfMetric>& metrics,
                                                     const NwdafConfig& config);

private:
    SbiResponse nfLoadInfo(std::map<std::string, nlohmann::json>& values,
                           const std::optional<NwdafFeatureSet>& consumer,
                           const NwdafFeatureSet& local);

    NwdafAnalyticsEngine&   engine_;
    NwdafSubscriptionStore& subs_;
    NwdafConfig             config_;
    NwdafSchemaValidator    validator_;
    bool                    available_ = false;
};
