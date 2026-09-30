#include "nwdaf_sbi.hpp"
#include "nwdaf_3gpp_adapter.hpp"
#include "nwdaf_analytics_catalogue.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <ctime>
#include <regex>
#include <set>

using json = nlohmann::json;

namespace {

const std::string EVTS  = "TS29520_Nnwdaf_EventsSubscription.yaml";
const std::string ANLY  = "TS29520_Nnwdaf_AnalyticsInfo.yaml";
const std::string CDATA = "TS29571_CommonData.yaml";

std::string schemaRef(const std::string& file, const std::string& name) {
    return file + "#/components/schemas/" + name;
}

const char* reasonPhrase(int status) {
    switch (status) {
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 415: return "Unsupported Media Type";
    case 500: return "Internal Server Error";
    default:  return "Error";
    }
}

// TS 29.571 ProblemDetails with a TS 29.500 / TS 29.520 application `cause`.
SbiResponse problem(int status, const std::string& cause, const std::string& detail,
                    const json& invalid_params = json(),
                    const std::string& supported_features = "") {
    json p = {{"title", reasonPhrase(status)}, {"status", status},
              {"detail", detail}, {"cause", cause}};
    if (invalid_params.is_array() && !invalid_params.empty()) p["invalidParams"] = invalid_params;
    if (!supported_features.empty()) p["supportedFeatures"] = supported_features;
    return {status, "application/problem+json", p.dump(), {}};
}

// H1.10 (I-6): the access token's analyticsIdList does not cover the request
// — TS 29.500 §6.7.3 insufficient_scope.
SbiResponse analyticsNotAuthorized(const SbiRequest& req, const char* root, const char* scope) {
    return {403, "", "", {{"WWW-Authenticate", "Bearer realm=\"" + req.api_root + root +
                           "\", error=\"insufficient_scope\", scope=\"" + scope + "\""}}};
}

SbiResponse methodNotAllowed(const std::string& allow) {
    // TS 29.500 §5.2.7.2: 405 with an Allow header; no cause needed.
    return {405, "", "", {{"Allow", allow}}};
}

json invalidParam(const std::string& param, const std::string& reason) {
    return {{"param", param}, {"reason", reason}};
}

// Schema violations of a JSON body → invalidParams with JSON Pointers.
json toInvalidParams(const std::vector<SchemaViolation>& v) {
    json out = json::array();
    for (const auto& e : v) out.push_back(invalidParam(e.pointer.empty() ? "/" : e.pointer, e.reason));
    return out;
}

bool onlyMissing(const std::vector<SchemaViolation>& v) {
    for (const auto& e : v) if (e.reason != "mandatory attribute is missing") return false;
    return true;
}

// TS 29.520 V18.14.0 §4.2.2.2.2 / §4.3.2.2: a target period starting in the
// past and ending in the future requests both statistics and predictions.
bool bothStatisticsAndPrediction(const json& req) {
    if (!req.is_object() || !req.contains("startTs") || !req.contains("endTs")) return false;
    auto start = NwdafSbiService::parseDateTime(req["startTs"].get<std::string>());
    auto end   = NwdafSbiService::parseDateTime(req["endTs"].get<std::string>());
    const auto now = std::chrono::system_clock::now();
    return start && end && *start < now && *end > now;
}

}  // namespace

namespace {

using Rejection = NwdafSbiService::Rejection;

// Analytics output stamps: generated now, valid until the collectors next
// refresh the measurements.
json timeStamps(const NwdafConfig& config) {
    const auto now = std::chrono::system_clock::now();
    return {{"timeStampGen", NwdafSbiService::formatDateTime(now)},
            {"expiry", NwdafSbiService::formatDateTime(
                           now + std::chrono::seconds(config.collection_interval_seconds))}};
}

// Nnwdaf_AnalyticsInfo: the response to a rejected query (Appendix A.2, A.4).
SbiResponse queryRejection(const Rejection& r, const NwdafFeatureSet& local) {
    const json ip = json::array({invalidParam(r.where, r.reason)});
    switch (r.kind) {
    case Rejection::MandatoryMissing:
        return problem(400, "MANDATORY_QUERY_PARAM_MISSING", r.reason, ip);
    case Rejection::MandatoryIncorrect:
        return problem(400, "MANDATORY_QUERY_PARAM_INCORRECT", r.reason, ip, local.toHex());
    case Rejection::Unsupported:
        return problem(400, "OPTIONAL_QUERY_PARAM_INCORRECT", r.reason, ip, local.toHex());
    case Rejection::UnavailableData:   // §4.3.2.2: past statistics not held
        return problem(500, "UNAVAILABLE_DATA", r.reason);
    }
    return problem(500, "SYSTEM_FAILURE", r.reason);
}

// Nnwdaf_EventsSubscription: the response that rejects the whole request, or
// nullopt when only this event fails (I-3), which is added to `failed`.
std::optional<SbiResponse> subscriptionRejection(const Rejection& r, const NwdafFeatureSet& local,
                                                 const std::string& event, json& failed) {
    const json ip = json::array({invalidParam(r.where, r.reason)});
    switch (r.kind) {
    case Rejection::MandatoryMissing:
        return problem(400, "MANDATORY_IE_MISSING", r.reason, ip);
    case Rejection::MandatoryIncorrect:
        return problem(400, "MANDATORY_IE_INCORRECT", r.reason, ip, local.toHex());
    case Rejection::Unsupported:
        failed.push_back({{"event", event}, {"failureCode", "OTHER"}});
        return std::nullopt;
    case Rejection::UnavailableData:   // §4.2.2.2.2
        return problem(500, "UNAVAILABLE_DATA", r.reason);
    }
    return std::nullopt;
}

const json* member(const json& j, const char* key) { return j.contains(key) ? &j[key] : nullptr; }

// The queries of an accepted EventSubscription (validated when it was created).
std::optional<Nwdaf3gppAdapter::NfLoadQuery> nfLoadQueryOf(const json& es) {
    Nwdaf3gppAdapter::NfLoadQuery q;
    if (NwdafSbiService::interpretNfLoad(member(es, "tgtUe"), "tgtUe", es, "",
                                         member(es, "extraReportReq"), "extraReportReq", q))
        return std::nullopt;
    return q;
}

std::optional<Nwdaf3gppAdapter::SliceQuery> sliceQueryOf(const json& es) {
    Nwdaf3gppAdapter::SliceQuery q;
    if (NwdafSbiService::interpretSliceLoad(es.value("event", ""), &es, "",
                                            member(es, "extraReportReq"), "extraReportReq", q))
        return std::nullopt;
    return q;
}

// EventNotifications for slice loads. EventNotification carries one
// SliceLoadLevelInformation, whose single level applies to all its snssais,
// so SLICE_LOAD_LEVEL gives one per distinct level; NSI_LOAD_LEVEL one in all.
std::optional<Nwdaf3gppAdapter::NwPerfQuery> nwPerfQueryOf(const json& es, const NwdafConfig& config) {
    Nwdaf3gppAdapter::NwPerfQuery q;
    if (NwdafSbiService::interpretNetworkPerformance(member(es, "tgtUe"), "tgtUe", &es, "",
                                                     member(es, "extraReportReq"), "extraReportReq",
                                                     true, config, q))
        return std::nullopt;
    return q;
}

std::optional<Nwdaf3gppAdapter::UeMobilityQuery> ueMobilityQueryOf(const json& es) {
    Nwdaf3gppAdapter::UeMobilityQuery q;
    if (NwdafSbiService::interpretUeMobility(member(es, "tgtUe"), "tgtUe", &es, "",
                                             member(es, "extraReportReq"), "extraReportReq", q))
        return std::nullopt;
    return q;
}

// UNAVAILABLE_DATA when a period starts before the held metrics history of a
// needed source (one collection interval of slack).
std::optional<Rejection> historyCovers(const std::optional<std::chrono::system_clock::time_point>& from,
                                       bool need_amf, bool need_smf, const NwdafReportInputs& in,
                                       const NwdafConfig& config, const std::string& req_at,
                                       const std::string& what) {
    if (!from) return std::nullopt;
    const auto slack = std::chrono::seconds(config.collection_interval_seconds);
    for (const auto* h : {need_amf ? &in.amf_oam : nullptr, need_smf ? &in.smf_oam : nullptr}) {
        if (!h) continue;
        if (h->empty() || h->front().at > *from + slack)
            return Rejection{Rejection::UnavailableData, req_at, what + " statistics for that period are not held"};
    }
    return std::nullopt;
}

std::vector<json> sliceNotifications(const std::vector<NwdafSliceLoad>& loads, const json& head) {
    std::vector<json> out;
    if (loads.empty()) return out;
    if (head["event"] == "NSI_LOAD_LEVEL") {
        json n = head;
        n["nsiLoadLevelInfos"] = Nwdaf3gppAdapter::nsiLoadLevelInfos(loads);
        out.push_back(n);
        return out;
    }
    for (const auto& group : Nwdaf3gppAdapter::sliceLoadLevelGroups(loads)) {
        json n = head;
        n["sliceLoadLevelInfo"] = group;
        out.push_back(n);
    }
    return out;
}

}  // namespace

// ── Construction / routing ──────────────────────────────────────────────────

NwdafSbiService::NwdafSbiService(NwdafAnalyticsEngine& engine,
                                 NwdafSubscriptionStore& subs,
                                 const NwdafConfig& config,
                                 std::shared_ptr<NwdafNfMonitor> nf_monitor)
    : engine_(engine), subs_(subs), config_(config),
      validator_(config.openapi_3gpp_dir),
      nf_monitor_(nf_monitor ? std::move(nf_monitor) : std::make_shared<NwdafNfMonitor>(config))
{
    try {
        validator_.document(EVTS);
        validator_.document(ANLY);
        available_ = true;
    } catch (const std::exception& e) {
        spdlog::error("3GPP Nnwdaf interfaces unavailable — official OpenAPI artifacts "
                      "not loadable from openapi_3gpp_dir: {}", e.what());
    }
}

bool NwdafSbiService::handles(const std::string& path) {
    static const std::string analytics = std::string(ANALYTICS_INFO_ROOT) + "/analytics";
    static const std::string subs = std::string(EVENTS_SUBSCRIPTION_ROOT) + "/subscriptions";
    if (path == analytics || path == subs) return true;
    return path.size() > subs.size() + 1 && path.compare(0, subs.size() + 1, subs + "/") == 0 &&
           path.find('/', subs.size() + 1) == std::string::npos;
}

SbiResponse NwdafSbiService::dispatch(const SbiRequest& req) {
    static const std::string analytics = std::string(ANALYTICS_INFO_ROOT) + "/analytics";
    static const std::string subs = std::string(EVENTS_SUBSCRIPTION_ROOT) + "/subscriptions";

    if (!available_)
        return problem(500, "SYSTEM_FAILURE",
                       "the official 3GPP OpenAPI artifacts are not installed on this instance");

    if (req.path == analytics) {
        // The 3GPP resource supports GET only. The deprecated, non-standard
        // JSON-body POST exists on the operator (HTTP/1.1) port alone.
        if (req.method == "GET") return getAnalytics(req);
        return methodNotAllowed("GET");
    }
    if (req.path == subs) {
        if (req.method == "POST") return createSubscription(req);
        return methodNotAllowed("POST");
    }
    const std::string id = req.path.substr(subs.size() + 1);
    if (req.method == "PUT")    return modifySubscription(req, id);
    if (req.method == "DELETE") return deleteSubscription(id);
    return methodNotAllowed("PUT, DELETE");
}

std::string NwdafSbiService::formatDateTime(std::chrono::system_clock::time_point tp) {
    const auto t = std::chrono::system_clock::to_time_t(tp);
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

std::optional<std::chrono::system_clock::time_point>
NwdafSbiService::parseDateTime(const std::string& s) {
    static const std::regex rfc3339(
        R"(^(\d{4})-(\d{2})-(\d{2})[Tt](\d{2}):(\d{2}):(\d{2})(\.\d+)?([Zz]|([+-])(\d{2}):(\d{2}))$)");
    std::smatch m;
    if (!std::regex_match(s, m, rfc3339)) return std::nullopt;
    struct tm tm_buf = {};
    tm_buf.tm_year = std::stoi(m[1]) - 1900;
    tm_buf.tm_mon  = std::stoi(m[2]) - 1;
    tm_buf.tm_mday = std::stoi(m[3]);
    tm_buf.tm_hour = std::stoi(m[4]);
    tm_buf.tm_min  = std::stoi(m[5]);
    tm_buf.tm_sec  = std::stoi(m[6]);
    if (tm_buf.tm_mon > 11 || tm_buf.tm_mday < 1 || tm_buf.tm_mday > 31 ||
        tm_buf.tm_hour > 23 || tm_buf.tm_min > 59 || tm_buf.tm_sec > 60)
        return std::nullopt;
    auto tp = std::chrono::system_clock::from_time_t(timegm(&tm_buf));
    if (m[7].matched)
        tp += std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<double>(std::stod("0" + m[7].str())));
    if (m[9].matched) {
        const auto offset = std::chrono::hours(std::stoi(m[10])) + std::chrono::minutes(std::stoi(m[11]));
        tp += (m[9] == "+") ? -offset : offset;   // local = UTC + offset
    }
    return tp;
}

// ── Nnwdaf_AnalyticsInfo: GET /analytics ────────────────────────────────────

SbiResponse NwdafSbiService::getAnalytics(const SbiRequest& req) {
    const NwdafFeatureSet local = NwdafSupportedFeatures::local(NnwdafApi::AnalyticsInfo, config_);

    // Query parameters of the operation (TS29520_Nnwdaf_AnalyticsInfo.yaml).
    // Structured ones are JSON-encoded (content: application/json).
    struct Param { const char* name; std::string ref; bool is_json; bool mandatory; };
    const std::vector<Param> params = {
        {"event-id",           schemaRef(ANLY, "EventId"),                   false, true},
        {"ana-req",            schemaRef(EVTS, "EventReportingRequirement"), true,  false},
        {"event-filter",       schemaRef(ANLY, "EventFilter"),               true,  false},
        {"supported-features", schemaRef(CDATA, "SupportedFeatures"),        false, false},
        {"tgt-ue",             schemaRef(EVTS, "TargetUeInformation"),       true,  false},
    };

    std::map<std::string, json> values;
    for (const auto& p : params) {
        auto it = req.query.find(p.name);
        const std::string cause = p.mandatory ? "MANDATORY_QUERY_PARAM_INCORRECT"
                                              : "OPTIONAL_QUERY_PARAM_INCORRECT";
        if (it == req.query.end()) {
            if (p.mandatory)
                return problem(400, "MANDATORY_QUERY_PARAM_MISSING", "event-id is mandatory",
                               json::array({invalidParam(p.name, "mandatory query parameter is missing")}));
            continue;
        }
        json value;
        if (p.is_json) {
            try { value = json::parse(it->second); }
            catch (const json::parse_error&) {
                return problem(400, cause, std::string(p.name) + " is not valid JSON",
                               json::array({invalidParam(p.name, "not a JSON value")}));
            }
        } else {
            value = it->second;
        }
        auto violations = validator_.validate(value, p.ref);
        if (!violations.empty()) {
            json ip = json::array();
            for (const auto& v : violations)
                ip.push_back(invalidParam(p.name, (v.pointer.empty() ? "" : v.pointer + ": ") + v.reason));
            return problem(400, cause, std::string(p.name) + " does not match its schema", ip);
        }
        values[p.name] = std::move(value);
    }
    // TS 29.500 §5.2.9 a): unsupported query parameters of a safe method are
    // ignored — deliberately, and logged.
    for (const auto& [k, v] : req.query) {
        bool known = false;
        for (const auto& p : params) known = known || k == p.name;
        if (!known) spdlog::debug("AnalyticsInfo: ignoring unsupported query parameter {}", k);
    }

    // TS 29.520 V18.14.0 §4.3.2.2: statistics and prediction in one request.
    if (values.count("ana-req") && bothStatisticsAndPrediction(values["ana-req"]))
        return problem(400, "BOTH_STAT_PRED_NOT_ALLOWED",
                       "the analytics target period starts in the past and ends in the future");

    // Capability: an analytics ID is served only when this NWDAF advertises
    // it — which also puts its gating feature, if any, in the local set (I-1).
    // AnalyticsInfo's LOAD_LEVEL_INFORMATION is the NwdafEvent SLICE_LOAD_LEVEL.
    const std::string event = values["event-id"].get<std::string>();
    const std::string nwdaf_event = NwdafAnalyticsCatalogue::fromAnalyticsInfoEventId(event);
    if (req.authorized_analytics && !req.authorized_analytics->count(nwdaf_event))
        return analyticsNotAuthorized(req, ANALYTICS_INFO_ROOT, "nnwdaf-analyticsinfo");
    if (!NwdafAnalyticsCatalogue::rel18Advertised(config_).count(nwdaf_event))
        return problem(400, "MANDATORY_QUERY_PARAM_INCORRECT",
                       "analytics " + event + " is not supported by this NWDAF",
                       json::array({invalidParam("event-id", "analytics not supported by this NWDAF")}),
                       local.toHex());

    std::optional<NwdafFeatureSet> consumer;
    if (values.count("supported-features"))
        consumer = NwdafFeatureSet::parse(values["supported-features"].get<std::string>());

    if (event == "NF_LOAD") return nfLoadInfo(values, consumer, local);
    if (nwdaf_event == "SLICE_LOAD_LEVEL" || nwdaf_event == "NSI_LOAD_LEVEL")
        return sliceLoadInfo(nwdaf_event, values, consumer, local);
    if (event == "NETWORK_PERFORMANCE") return nwPerfInfo(values, consumer, local);
    if (event == "UE_MOBILITY") return ueMobilityInfo(values, consumer, local);

    // An advertised ID always has a mapping; reaching here is a defect.
    spdlog::error("AnalyticsInfo: {} is advertised but has no Rel-18 mapping", event);
    return problem(500, "SYSTEM_FAILURE", "no Rel-18 mapping for " + event);
}

// ── NF_LOAD ─────────────────────────────────────────────────────────────────

std::optional<NwdafSbiService::Rejection> NwdafSbiService::interpretNfLoad(
    const json* target, const std::string& target_at,
    const json& filter, const std::string& filter_at,
    const json* req, const std::string& req_at,
    Nwdaf3gppAdapter::NfLoadQuery& query)
{
    // Target UE(s): "shall provide … supis or anyUe" (§4.2.2.2.2, §4.3.2.2).
    // Only the network-wide form is implemented: which AMF/SMF instance
    // serves a given SUPI is not observed.
    if (!target) return Rejection{Rejection::MandatoryMissing, target_at,
                                  "the target UE (supis or anyUe) is mandatory for NF_LOAD"};
    if (target->contains("supis"))
        return Rejection{Rejection::MandatoryIncorrect, target_at,
                         "/supis: per-UE NF_LOAD is not supported by this NWDAF; use anyUe"};
    if (!target->value("anyUe", false))
        return Rejection{Rejection::MandatoryIncorrect, target_at, "NF_LOAD requires supis or anyUe=true"};
    for (auto it = target->begin(); it != target->end(); ++it)
        if (it.key() != "anyUe")
            spdlog::debug("NF_LOAD: ignoring {}/{} (not applicable to NF_LOAD)", target_at, it.key());

    // Filter attributes. Relevant to NF_LOAD per the prose: nfInstanceIds,
    // nfSetIds, nfTypes, snssais, networkArea (NfLoadExt), listOfAnaSubsets
    // (EneNA). nfLoadLvlThds and matchingDir are reporting criteria, read by
    // interpretThresholds.
    static const std::set<std::string> unimplemented = {"nfSetIds", "snssais"};
    static const std::set<std::string> unsupported_feature = {  // I-2
        "networkArea", "listOfAnaSubsets"};
    for (auto it = filter.begin(); it != filter.end(); ++it) {
        const std::string& k = it.key();
        if (k == "nfLoadLvlThds" || k == "matchingDir") continue;
        if (k == "nfInstanceIds") {
            for (const auto& id : it.value()) query.nf_instance_ids.insert(id.get<std::string>());
        } else if (k == "nfTypes") {
            for (const auto& t : it.value()) query.nf_types.insert(t.get<std::string>());
        } else if (unimplemented.count(k)) {
            return Rejection{Rejection::Unsupported, filter_at,
                             "/" + k + ": not supported for NF_LOAD by this NWDAF"};
        } else if (unsupported_feature.count(k)) {
            spdlog::info("NF_LOAD: ignoring {}/{} (feature not supported, I-2)", filter_at, k);
        } else {
            spdlog::debug("NF_LOAD: ignoring {}/{} (not applicable to NF_LOAD)", filter_at, k);
        }
    }

    // Reporting requirements (EventReportingRequirement).
    if (req) {
        static const std::set<std::string> feature_bound = {  // EneNA / Aggregation, I-2
            "accPerSubset", "offsetPeriod", "timeAnaNeeded", "histAnaTimePeriod",
            "anaMeta", "anaMetaInd"};
        for (auto it = req->begin(); it != req->end(); ++it) {
            const std::string& k = it.key();
            if (k == "startTs" || k == "endTs") continue;
            if (k == "maxObjectNbr") { query.max_objects = it.value().get<size_t>(); continue; }
            // A *preferred* accuracy level; this NWDAF has one level and
            // serves it (best effort, as "preferred" permits).
            if (k == "accuracy") continue;
            if (feature_bound.count(k)) {
                spdlog::info("NF_LOAD: ignoring {}/{} (feature not supported, I-2)", req_at, k);
                continue;
            }
            if (k == "sampRatio" || k == "maxSupiNbr")
                return Rejection{Rejection::Unsupported, req_at,
                                 "/" + k + ": not supported for NF_LOAD by this NWDAF"};
            spdlog::debug("NF_LOAD: ignoring unknown {}/{}", req_at, k);
        }
        // Analytics target period. The collectors hold the current NF load
        // only: no history for past statistics, no NF-load prediction.
        const auto now = std::chrono::system_clock::now();
        bool future = false, past = false;
        for (const char* k : {"startTs", "endTs"}) {
            if (!req->contains(k)) continue;
            auto t = parseDateTime((*req)[k].get<std::string>());
            if (!t) return Rejection{Rejection::Unsupported, req_at,
                                     std::string("/") + k + ": not an RFC 3339 date-time"};
            (*t > now ? future : past) = true;
        }
        if (future)
            return Rejection{Rejection::Unsupported, req_at,
                             "NF_LOAD predictions are not supported by this NWDAF"};
        if (past)
            return Rejection{Rejection::UnavailableData, req_at,
                             "past NF_LOAD statistics are not held; only the current load is"};
    }
    return std::nullopt;
}

SbiResponse NwdafSbiService::nfLoadInfo(std::map<std::string, json>& values,
                                        const std::optional<NwdafFeatureSet>& consumer,
                                        const NwdafFeatureSet& local) {
    Nwdaf3gppAdapter::NfLoadQuery query;
    const json no_filter = json::object();
    auto rej = interpretNfLoad(values.count("tgt-ue") ? &values["tgt-ue"] : nullptr, "tgt-ue",
                               values.count("event-filter") ? values["event-filter"] : no_filter,
                               "event-filter",
                               values.count("ana-req") ? &values["ana-req"] : nullptr, "ana-req",
                               query);
    if (rej) return queryRejection(*rej, local);

    const json infos = Nwdaf3gppAdapter::nfLoadLevelInfos(engine_.getCurrentNfMetrics(),
                                                          nf_monitor_->ids(),
                                                          nf_monitor_->statuses(), query);
    // §4.3.2.2: "If the requested NWDAF Analytics data does not exist, the
    // NWDAF shall respond with 204 No Content".
    if (infos.empty()) return {204, "", "", {}};

    json data = timeStamps(config_);
    data["nfLoadLevelInfos"] = infos;
    if (consumer) data["suppFeat"] = local.intersect(*consumer).toHex();
    return {200, "application/json", data.dump(), {}};
}

// ── H1.2: SLICE_LOAD_LEVEL / NSI_LOAD_LEVEL (I-9) ───────────────────────────

std::optional<NwdafSbiService::Rejection> NwdafSbiService::interpretSliceLoad(
    const std::string& event,
    const json* filter, const std::string& filter_at,
    const json* req, const std::string& req_at,
    Nwdaf3gppAdapter::SliceQuery& query)
{
    const bool nsi = event == "NSI_LOAD_LEVEL";
    const char* slices = nsi ? "nsiIdInfos" : "snssais";
    // §4.3.2.2 / §4.2.2.2.2: the slices via snssais (nsiIdInfos for
    // NSI_LOAD_LEVEL) or anySlice.
    if (!filter)
        return Rejection{Rejection::MandatoryMissing, filter_at,
                         std::string(slices) + " or anySlice is mandatory for " + event};
    if (filter->value("anySlice", false)) {
        query.any = true;
    } else if (filter->contains(slices)) {
        for (const auto& e : (*filter)[slices]) {
            if (!nsi) { query.keys.insert(Nwdaf3gppAdapter::snssaiKey(e)); continue; }
            // Open5GS has no network slice instances: an NSI-level request
            // cannot be served (the answer would need nsiId).
            if (e.contains("nsiIds"))
                return Rejection{Rejection::Unsupported, filter_at,
                                 "/nsiIdInfos/nsiIds: network slice instances are not supported by this NWDAF"};
            query.keys.insert(Nwdaf3gppAdapter::snssaiKey(e.at("snssai")));
        }
    } else if (filter->contains("anySlice")) {
        return Rejection{Rejection::MandatoryIncorrect, filter_at,
                         std::string("anySlice is false and no ") + slices + " is given"};
    } else {
        return Rejection{Rejection::MandatoryMissing, filter_at,
                         std::string(slices) + " or anySlice is mandatory for " + event};
    }
    // NsiLoadExt (networkArea, nfTypes) and EneNA (listOfAnaSubsets) are not
    // supported: ignored and logged (I-2).
    for (const char* k : {"networkArea", "nfTypes", "listOfAnaSubsets"})
        if (filter->contains(k)) spdlog::info("{}: ignoring {}/{} (feature not supported, I-2)", event, filter_at, k);

    if (req) {
        const auto now = std::chrono::system_clock::now();
        for (const char* k : {"startTs", "endTs"}) {
            if (!req->contains(k)) continue;
            auto t = parseDateTime((*req)[k].get<std::string>());
            if (!t) return Rejection{Rejection::Unsupported, req_at,
                                     std::string("/") + k + ": not an RFC 3339 date-time"};
            if (*t > now)
                return Rejection{Rejection::Unsupported, req_at,
                                 event + " predictions are not supported by this NWDAF"};
            (std::string(k) == "startTs" ? query.from : query.to) = *t;
        }
        for (auto it = req->begin(); it != req->end(); ++it)
            if (it.key() != "startTs" && it.key() != "endTs")
                spdlog::debug("{}: ignoring {}/{}", event, req_at, it.key());
    }
    return std::nullopt;
}

std::optional<NwdafSbiService::Rejection> NwdafSbiService::sliceHistoryCovers(
    const Nwdaf3gppAdapter::SliceQuery& query, const NwdafReportInputs& in,
    const NwdafConfig& config, const std::string& req_at)
{
    bool need_amf = false, need_pdu = false;
    for (const auto& c : config.slice_capacity) {
        need_amf = need_amf || c.max_ues > 0;
        need_pdu = need_pdu || c.max_pdu_sessions > 0;
    }
    return historyCovers(query.from, need_amf, need_pdu, in, config, req_at, "slice load");
}

// ── H1.4: NETWORK_PERFORMANCE (I-11) ────────────────────────────────────────

std::optional<NwdafSbiService::Rejection> NwdafSbiService::interpretNetworkPerformance(
    const json* target, const std::string& target_at,
    const json* filter, const std::string& filter_at,
    const json* req, const std::string& req_at,
    bool subscription, const NwdafConfig& config,
    Nwdaf3gppAdapter::NwPerfQuery& query)
{
    // Target UE(s): supis, intGroupIds or anyUe (§4.2.2.2.2, §4.3.2.2). The
    // counts are network-wide, so only anyUe is served.
    if (!target)
        return Rejection{Rejection::MandatoryMissing, target_at,
                         "the target UE (supis, intGroupIds or anyUe) is mandatory for NETWORK_PERFORMANCE"};
    if (target->contains("supis") || target->contains("intGroupIds"))
        return Rejection{Rejection::MandatoryIncorrect, target_at,
                         "per-UE or per-group NETWORK_PERFORMANCE is not supported by this NWDAF; use anyUe"};
    if (!target->value("anyUe", false))
        return Rejection{Rejection::MandatoryIncorrect, target_at,
                         "NETWORK_PERFORMANCE requires supis, intGroupIds or anyUe=true"};

    // The types: nwPerfTypes (AnalyticsInfo) or nwPerfRequs (subscription).
    const char* types_key = subscription ? "nwPerfRequs" : "nwPerfTypes";
    if (!filter || !filter->contains(types_key))
        return Rejection{Rejection::MandatoryMissing, filter_at + "/" + types_key,
                         std::string(types_key) + " is mandatory for NETWORK_PERFORMANCE"};
    for (const auto& t : (*filter)[types_key]) {
        const std::string type = subscription ? t.value("nwPerfType", std::string()) : t.get<std::string>();
        if (!Nwdaf3gppAdapter::nwPerfTypeAvailable(type, config))
            return Rejection{Rejection::Unsupported, filter_at + "/" + types_key,
                             "network performance type " + type + " is not available from this NWDAF (I-11)"};
        if (std::find(query.types.begin(), query.types.end(), type) == query.types.end())
            query.types.push_back(type);
    }

    // networkArea is mandatory with anyUe. NUM_OF_UE from the AMF's UE list
    // is computed for any area of TAs or cells; the metrics are per AMF or
    // SMF, so they describe the whole served area only (I-11).
    if (!filter->contains("networkArea"))
        return Rejection{Rejection::MandatoryMissing, filter_at + "/networkArea",
                         "networkArea is mandatory for NETWORK_PERFORMANCE with anyUe"};
    query.area = (*filter)["networkArea"];
    for (const auto& type : query.types) {
        if (Nwdaf3gppAdapter::nwPerfPerArea(type, config)) {
            for (auto it = query.area.begin(); it != query.area.end(); ++it)
                if (it.key() != "tais" && it.key() != "ncgis")
                    return Rejection{Rejection::Unsupported, filter_at + "/networkArea/" + it.key(),
                                     "the area can be given as tais or ncgis only (I-11)"};
            if (!query.area.contains("tais") && !query.area.contains("ncgis"))
                return Rejection{Rejection::Unsupported, filter_at + "/networkArea",
                                 "the area must list tais or ncgis (I-11)"};
        } else if (!Nwdaf3gppAdapter::coversServedArea(query.area, config)) {
            return Rejection{Rejection::Unsupported, filter_at + "/networkArea",
                             type + " is available only for the whole served area (every TAI in served_tai_list)"};
        }
    }

    // Feature-bound refinements this NWDAF doesn't support are ignored (I-2).
    for (const char* k : {"nwPerfReqs", "addNwPerfReqs", "spatialGranSizeTa", "spatialGranSizeCell",
                          "temporalGranSize"})
        if (filter->contains(k))
            spdlog::info("NETWORK_PERFORMANCE: ignoring {}/{} (feature not supported, I-2)", filter_at, k);

    if (req) {
        const auto now = std::chrono::system_clock::now();
        for (const char* k : {"startTs", "endTs"}) {
            if (!req->contains(k)) continue;
            auto t = parseDateTime((*req)[k].get<std::string>());
            if (!t) return Rejection{Rejection::Unsupported, req_at,
                                     std::string("/") + k + ": not an RFC 3339 date-time"};
            if (*t > now)
                return Rejection{Rejection::Unsupported, req_at,
                                 "NETWORK_PERFORMANCE predictions are not supported by this NWDAF"};
            (std::string(k) == "startTs" ? query.from : query.to) = *t;
        }
    }
    return std::nullopt;
}

std::optional<NwdafSbiService::Rejection> NwdafSbiService::nwPerfHistoryCovers(
    const Nwdaf3gppAdapter::NwPerfQuery& query, const NwdafReportInputs& in,
    const NwdafConfig& config, const std::string& req_at)
{
    const auto has = [&](const char* t) {
        return std::find(query.types.begin(), query.types.end(), t) != query.types.end();
    };
    // NUM_OF_UE from the UE list needs its history, the others the metrics'.
    const bool per_area = has("NUM_OF_UE") && Nwdaf3gppAdapter::nwPerfPerArea("NUM_OF_UE", config);
    if (per_area && query.from) {
        const auto held = in.ue_locations ? in.ue_locations->heldSince(std::chrono::system_clock::now())
                                          : std::nullopt;
        if (!held || *held > *query.from + std::chrono::seconds(config.collection_interval_seconds))
            return Rejection{Rejection::UnavailableData, req_at,
                             "network performance statistics for that period are not held"};
    }
    return historyCovers(query.from, has("NUM_OF_UE") && !per_area, has("SESS_SUCC_RATIO"), in, config, req_at,
                         "network performance");
}

SbiResponse NwdafSbiService::nwPerfInfo(std::map<std::string, json>& values,
                                        const std::optional<NwdafFeatureSet>& consumer,
                                        const NwdafFeatureSet& local) {
    Nwdaf3gppAdapter::NwPerfQuery query;
    const NwdafReportInputs in = inputs();
    auto rej = interpretNetworkPerformance(values.count("tgt-ue") ? &values["tgt-ue"] : nullptr, "tgt-ue",
                                           values.count("event-filter") ? &values["event-filter"] : nullptr,
                                           "event-filter",
                                           values.count("ana-req") ? &values["ana-req"] : nullptr, "ana-req",
                                           false, config_, query);
    if (!rej) rej = nwPerfHistoryCovers(query, in, config_, "ana-req");
    if (rej) return queryRejection(*rej, local);

    const json infos = Nwdaf3gppAdapter::nwPerfInfos(config_, query, in.amf_oam, in.smf_oam, in.ue_locations.get(),
                                                     std::chrono::system_clock::now());
    if (infos.empty()) return {204, "", "", {}};   // §4.3.2.2: no data for the period
    json data = timeStamps(config_);
    data["nwPerfs"] = infos;
    if (consumer) data["suppFeat"] = local.intersect(*consumer).toHex();
    return {200, "application/json", data.dump(), {}};
}

// ── H1.1: UE_MOBILITY (I-12) ────────────────────────────────────────────────

std::optional<NwdafSbiService::Rejection> NwdafSbiService::interpretUeMobility(
    const json* target, const std::string& target_at,
    const json* filter, const std::string& filter_at,
    const json* req, const std::string& req_at,
    Nwdaf3gppAdapter::UeMobilityQuery& query)
{
    // Target UE(s): supis or intGroupIds (§4.2.2.2.2, §4.3.2.2). Group
    // membership isn't available to this NWDAF, so only SUPIs are served.
    if (!target)
        return Rejection{Rejection::MandatoryMissing, target_at,
                         "the target UE (supis or intGroupIds) is mandatory for UE_MOBILITY"};
    if (target->contains("intGroupIds"))
        return Rejection{Rejection::Unsupported, target_at + "/intGroupIds",
                         "internal groups can't be resolved by this NWDAF; use supis (I-12)"};
    if (!target->contains("supis"))
        return Rejection{Rejection::MandatoryIncorrect, target_at,
                         "UE_MOBILITY requires supis or intGroupIds"};
    for (const auto& s : (*target)["supis"]) {
        const std::string supi = s.get<std::string>();
        if (std::find(query.supis.begin(), query.supis.end(), supi) == query.supis.end())
            query.supis.push_back(supi);
    }

    if (filter) {
        // The area of interest: the AMF reports TAs and NR cells.
        if (filter->contains("networkArea")) {
            const json& area = (*filter)["networkArea"];
            for (auto it = area.begin(); it != area.end(); ++it)
                if (it.key() != "tais" && it.key() != "ncgis")
                    return Rejection{Rejection::Unsupported, filter_at + "/networkArea/" + it.key(),
                                     "the area of interest can be given as tais or ncgis only (I-12)"};
            if (!area.contains("tais") && !area.contains("ncgis"))
                return Rejection{Rejection::Unsupported, filter_at + "/networkArea",
                                 "the area of interest must list tais or ncgis (I-12)"};
            query.area = area;
        }
        // Feature-bound refinements this NWDAF doesn't support are ignored (I-2).
        for (const char* k : {"visitedAreas", "ladnDnns", "ueMobilityReqs", "locGranularity", "locOrientation",
                              "listOfAnaSubsets", "spatialGranSizeTa", "spatialGranSizeCell",
                              "temporalGranSize", "fineGranAreas"})
            if (filter->contains(k))
                spdlog::info("UE_MOBILITY: ignoring {}/{} (feature not supported, I-2)", filter_at, k);
    }

    if (req) {
        const auto now = std::chrono::system_clock::now();
        for (auto it = req->begin(); it != req->end(); ++it) {
            const std::string& k = it.key();
            if (k == "startTs" || k == "endTs") {
                auto t = parseDateTime(it.value().get<std::string>());
                if (!t) return Rejection{Rejection::Unsupported, req_at, "/" + k + ": not an RFC 3339 date-time"};
                if (*t > now)
                    return Rejection{Rejection::Unsupported, req_at,
                                     "UE_MOBILITY predictions are not supported by this NWDAF"};
                (k == "startTs" ? query.from : query.to) = *t;
                continue;
            }
            if (k == "maxObjectNbr") { query.max_objects = it.value().get<size_t>(); continue; }
            if (k == "accuracy") continue;   // a preferred level; served best effort
            if (k == "sampRatio" || k == "maxSupiNbr")
                return Rejection{Rejection::Unsupported, req_at,
                                 "/" + k + ": not supported for UE_MOBILITY by this NWDAF"};
            spdlog::debug("UE_MOBILITY: ignoring {}/{}", req_at, k);
        }
    }
    return std::nullopt;
}

std::optional<NwdafSbiService::Rejection> NwdafSbiService::ueMobilityHistoryCovers(
    const Nwdaf3gppAdapter::UeMobilityQuery& query, const NwdafReportInputs& in,
    const NwdafConfig& config, const std::string& req_at)
{
    if (!query.from) return std::nullopt;
    const auto held = in.ue_locations ? in.ue_locations->heldSince(std::chrono::system_clock::now())
                                      : std::nullopt;
    if (!held || *held > *query.from + std::chrono::seconds(config.collection_interval_seconds))
        return Rejection{Rejection::UnavailableData, req_at, "UE location statistics for that period are not held"};
    return std::nullopt;
}

SbiResponse NwdafSbiService::ueMobilityInfo(std::map<std::string, json>& values,
                                            const std::optional<NwdafFeatureSet>& consumer,
                                            const NwdafFeatureSet& local) {
    Nwdaf3gppAdapter::UeMobilityQuery query;
    const NwdafReportInputs in = inputs();
    auto rej = interpretUeMobility(values.count("tgt-ue") ? &values["tgt-ue"] : nullptr, "tgt-ue",
                                   values.count("event-filter") ? &values["event-filter"] : nullptr,
                                   "event-filter",
                                   values.count("ana-req") ? &values["ana-req"] : nullptr, "ana-req", query);
    if (!rej) rej = ueMobilityHistoryCovers(query, in, config_, "ana-req");
    if (rej) return queryRejection(*rej, local);

    const json mobs = in.ue_locations
        ? Nwdaf3gppAdapter::ueMobilities(config_, query, *in.ue_locations, std::chrono::system_clock::now())
        : json::array();
    if (mobs.empty()) return {204, "", "", {}};   // §4.3.2.2: no data for the UE(s) and period
    json data = timeStamps(config_);
    data["ueMobs"] = mobs;
    if (consumer) data["suppFeat"] = local.intersect(*consumer).toHex();
    return {200, "application/json", data.dump(), {}};
}

NwdafReportInputs NwdafSbiService::gatherInputs(const NwdafAnalyticsEngine& engine,
                                                const NwdafNfMonitor& nf_monitor) {
    NwdafReportInputs in;
    in.metrics         = engine.getCurrentNfMetrics();
    in.nf_instance_ids = nf_monitor.ids();
    in.statuses        = nf_monitor.statuses();
    in.amf_oam         = engine.getOamHistory("AMF");
    in.smf_oam         = engine.getOamHistory("SMF");
    in.ue_locations    = engine.getUeLocations();
    return in;
}

NwdafReportInputs NwdafSbiService::inputs() const { return gatherInputs(engine_, *nf_monitor_); }

SbiResponse NwdafSbiService::sliceLoadInfo(const std::string& event,
                                           std::map<std::string, json>& values,
                                           const std::optional<NwdafFeatureSet>& consumer,
                                           const NwdafFeatureSet& local) {
    Nwdaf3gppAdapter::SliceQuery query;
    const NwdafReportInputs in = inputs();
    auto rej = interpretSliceLoad(event, values.count("event-filter") ? &values["event-filter"] : nullptr,
                                  "event-filter",
                                  values.count("ana-req") ? &values["ana-req"] : nullptr, "ana-req", query);
    if (!rej) rej = sliceHistoryCovers(query, in, config_, "ana-req");
    if (rej) return queryRejection(*rej, local);

    const auto loads = Nwdaf3gppAdapter::sliceLoads(config_, query, in.amf_oam, in.smf_oam,
                                                    std::chrono::system_clock::now());
    // §4.3.2.2: no analytics data for the request → 204 No Content. That
    // includes slices with no configured capacity (no load level, I-9).
    if (loads.empty()) return {204, "", "", {}};

    json data = timeStamps(config_);
    if (event == "SLICE_LOAD_LEVEL") data["sliceLoadLevelInfos"] = Nwdaf3gppAdapter::sliceLoadLevelInfos(loads);
    else                             data["nsiLoadLevelInfos"]   = Nwdaf3gppAdapter::nsiLoadLevelInfos(loads);
    if (consumer) data["suppFeat"] = local.intersect(*consumer).toHex();
    return {200, "application/json", data.dump(), {}};
}

// ── H1.7: THRESHOLD reporting (I-10) ────────────────────────────────────────

namespace {

enum class Direction { Ascending, Descending, Crossed };

// MatchingDirection (Table 5.1.6.3.12-1), default CROSSED.
Direction direction(const json& es) {
    const std::string d = es.value("matchingDir", std::string("CROSSED"));
    if (d == "ASCENDING")  return Direction::Ascending;
    if (d == "DESCENDING") return Direction::Descending;
    return Direction::Crossed;
}

// I-10: a threshold is crossed ascending when the value goes from below it to
// at or above it ("met or exceeded"), descending the other way.
bool crossed(int prev, int cur, int threshold, Direction dir) {
    const bool up   = prev < threshold && cur >= threshold;
    const bool down = prev >= threshold && cur < threshold;
    switch (dir) {
    case Direction::Ascending:  return up;
    case Direction::Descending: return down;
    case Direction::Crossed:    return up || down;
    }
    return false;
}

// Crossing test for one entity; updates the entity's baseline.
bool crossedAny(NwdafSbiService::ThresholdState& state, const std::string& entity, int value,
                const std::vector<int>& thresholds, Direction dir) {
    const auto it = state.find(entity);
    bool hit = false;
    if (it != state.end())
        for (int t : thresholds) hit = hit || crossed(it->second, value, t, dir);
    state[entity] = value;
    return hit;
}

// NSI_LOAD_LEVEL: nsiLevelThrds holds one threshold for every slice, or one
// per nsiIdInfos entry, in order (I-10).
std::map<std::string, int> nsiThresholds(const json& es) {
    std::map<std::string, int> out;
    const json& thrs = es["nsiLevelThrds"];
    if (thrs.size() == 1) { out[""] = thrs[0].get<int>(); return out; }
    const json& infos = es["nsiIdInfos"];
    for (size_t i = 0; i < infos.size(); ++i)
        out[Nwdaf3gppAdapter::snssaiKey(infos[i].at("snssai"))] = thrs[i].get<int>();
    return out;
}

}  // namespace

std::string NwdafSbiService::effectiveMethod(const json& evt_req, const json& es) {
    return evt_req.contains("notifMethod") ? evt_req["notifMethod"].get<std::string>()
                                           : es.value("notificationMethod", std::string("THRESHOLD"));
}

bool NwdafSbiService::thresholdMode(const json& evt_req, const json& es) {
    const std::string m = effectiveMethod(evt_req, es);
    return m == "THRESHOLD" || m == "ON_EVENT_DETECTION";
}

std::optional<NwdafSbiService::Rejection> NwdafSbiService::interpretThresholds(const json& es,
                                                                               const std::string& at) {
    const std::string event = es.value("event", "");
    if (event == "NF_LOAD") {
        if (!es.contains("nfLoadLvlThds"))
            return Rejection{Rejection::MandatoryMissing, at + "/nfLoadLvlThds",
                             "nfLoadLvlThds is mandatory for THRESHOLD reporting of NF_LOAD"};
        // Only the CPU usage is measured (I-5): a level on NRF load, memory or
        // storage cannot be evaluated.
        for (const auto& t : es["nfLoadLvlThds"]) {
            if (!t.contains("nfCpuUsage") || t.size() != 1)
                return Rejection{Rejection::Unsupported, at + "/nfLoadLvlThds",
                                 "only nfCpuUsage thresholds are supported for NF_LOAD by this NWDAF"};
        }
        return std::nullopt;
    }
    if (event == "SLICE_LOAD_LEVEL") {
        if (!es.contains("loadLevelThreshold"))
            return Rejection{Rejection::MandatoryMissing, at + "/loadLevelThreshold",
                             "loadLevelThreshold is mandatory for THRESHOLD reporting of SLICE_LOAD_LEVEL"};
        return std::nullopt;
    }
    if (event == "NSI_LOAD_LEVEL") {
        if (!es.contains("nsiLevelThrds"))
            return Rejection{Rejection::MandatoryMissing, at + "/nsiLevelThrds",
                             "nsiLevelThrds is mandatory for THRESHOLD reporting of NSI_LOAD_LEVEL"};
        const size_t n = es["nsiLevelThrds"].size();
        if (n != 1 && (!es.contains("nsiIdInfos") || es["nsiIdInfos"].size() != n))
            return Rejection{Rejection::MandatoryIncorrect, at + "/nsiLevelThrds",
                             "nsiLevelThrds must hold one threshold, or one per nsiIdInfos entry"};
        // matchingDir needs NsiLoadExt, which is not supported (I-2).
        if (es.contains("matchingDir"))
            spdlog::info("NSI_LOAD_LEVEL: ignoring {}/matchingDir (NsiLoadExt not supported, I-2)", at);
        return std::nullopt;
    }
    if (event == "NETWORK_PERFORMANCE") {
        // Table 5.1.6.2.22-1 NOTE 1: each requirement carries its threshold.
        // NUM_OF_UE is a count (absoluteNum), SESS_SUCC_RATIO a percentage
        // (relativeRatio).
        for (const auto& r : es.value("nwPerfRequs", json::array())) {
            const std::string type = r.value("nwPerfType", std::string());
            const bool ratio = type == "SESS_SUCC_RATIO";
            if (!r.contains("relativeRatio") && !r.contains("absoluteNum"))
                return Rejection{Rejection::MandatoryMissing, at + "/nwPerfRequs",
                                 "relativeRatio or absoluteNum is mandatory for THRESHOLD reporting of " + type};
            if (r.contains(ratio ? "absoluteNum" : "relativeRatio"))
                return Rejection{Rejection::Unsupported, at + "/nwPerfRequs",
                                 type + " is reported as " + (ratio ? "relativeRatio" : "absoluteNum")};
        }
        return std::nullopt;
    }
    return Rejection{Rejection::Unsupported, at, "THRESHOLD reporting is not supported for " + event};
}

std::vector<json> NwdafSbiService::thresholdReports(const json& es, const NwdafReportInputs& in,
                                                    const NwdafConfig& config, ThresholdState& state) {
    std::vector<json> out;
    const std::string event = es.value("event", "");
    json head = timeStamps(config);
    head["event"] = event;

    if (event == "NF_LOAD") {
        const auto query = nfLoadQueryOf(es);
        if (!query) return out;
        std::vector<int> levels;
        for (const auto& t : es.value("nfLoadLvlThds", json::array())) levels.push_back(t.value("nfCpuUsage", 0));
        // TS 23.288 §6.5.1: one threshold for all matching NFs; reported when
        // met for at least one of them — here, the NFs that crossed it.
        json crossed_nfs = json::array();
        for (const auto& info : Nwdaf3gppAdapter::nfLoadLevelInfos(in.metrics, in.nf_instance_ids, in.statuses, *query)) {
            if (!info.contains("nfCpuUsage")) continue;
            if (crossedAny(state, info["nfInstanceId"].get<std::string>(), info["nfCpuUsage"].get<int>(),
                           levels, direction(es)))
                crossed_nfs.push_back(info);
        }
        if (!crossed_nfs.empty()) {
            json n = head;
            n["nfLoadLevelInfos"] = crossed_nfs;
            out.push_back(n);
        }
        return out;
    }

    if (event == "NETWORK_PERFORMANCE") {
        const auto query = nwPerfQueryOf(es, config);
        if (!query) return out;
        json crossed_types = json::array();
        for (const auto& info : Nwdaf3gppAdapter::nwPerfInfos(config, *query, in.amf_oam, in.smf_oam, in.ue_locations.get(),
                                                              std::chrono::system_clock::now())) {
            const std::string type = info["nwPerfType"].get<std::string>();
            const char* key = info.contains("relativeRatio") ? "relativeRatio" : "absoluteNum";
            std::vector<int> levels;
            for (const auto& r : es["nwPerfRequs"])
                if (r.value("nwPerfType", std::string()) == type && r.contains(key)) levels.push_back(r[key].get<int>());
            if (crossedAny(state, type, info[key].get<int>(), levels, direction(es)))
                crossed_types.push_back(info);
        }
        if (!crossed_types.empty()) {
            json n = head;
            n["nwPerfs"] = crossed_types;
            out.push_back(n);
        }
        return out;
    }

    if (event == "SLICE_LOAD_LEVEL" || event == "NSI_LOAD_LEVEL") {
        const auto query = sliceQueryOf(es);
        if (!query) return out;
        const bool nsi = event == "NSI_LOAD_LEVEL";
        const auto per_slice = nsi ? nsiThresholds(es) : std::map<std::string, int>{};
        std::vector<NwdafSliceLoad> hits;
        for (const auto& load : Nwdaf3gppAdapter::sliceLoads(config, *query, in.amf_oam, in.smf_oam,
                                                             std::chrono::system_clock::now())) {
            int threshold;
            if (!nsi) {
                threshold = es.value("loadLevelThreshold", 0);
            } else {
                auto it = per_slice.find(per_slice.count("") ? "" : load.slice.key());
                if (it == per_slice.end()) continue;
                threshold = it->second;
            }
            // SLICE_LOAD_LEVEL has no matchingDir; NSI's needs NsiLoadExt: CROSSED.
            if (crossedAny(state, load.slice.key(), load.load_level, {threshold}, Direction::Crossed))
                hits.push_back(load);
        }
        return sliceNotifications(hits, head);
    }
    return out;
}

// ── Nnwdaf_EventsSubscription ───────────────────────────────────────────────

std::optional<SbiResponse>
NwdafSbiService::evaluateSubscription(const SbiRequest& req, SubscriptionOutcome& out) {
    const NwdafFeatureSet local =
        NwdafSupportedFeatures::local(NnwdafApi::EventsSubscription, config_);

    auto ct = req.headers.find("content-type");
    if (ct == req.headers.end() || ct->second.rfind("application/json", 0) != 0)
        return problem(415, "INVALID_MSG_FORMAT", "the request body must be application/json");

    try { out.request = json::parse(req.body); }
    catch (const json::parse_error&) {
        return problem(400, "INVALID_MSG_FORMAT", "the request body is not valid JSON");
    }

    // 1. Schema (official artifact).
    auto violations = validator_.validate(out.request, schemaRef(EVTS, "NnwdafEventsSubscription"));
    if (!violations.empty())
        return problem(400, onlyMissing(violations) ? "MANDATORY_IE_MISSING" : "INVALID_MSG_FORMAT",
                       "NnwdafEventsSubscription does not match its schema",
                       toInvalidParams(violations));

    const json& body = out.request;

    // H1.10 (I-6): every requested analytics must be within the token's analyticsIdList.
    if (req.authorized_analytics)
        for (const auto& es : body["eventSubscriptions"])
            if (!req.authorized_analytics->count(es["event"].get<std::string>()))
                return analyticsNotAuthorized(req, EVENTS_SUBSCRIPTION_ROOT, "nnwdaf-eventssubscription");

    // 2. Prose rules.
    // Table 5.1.6.2.2-1: notificationURI "shall be supplied … in the HTTP
    // POST … and in the HTTP PUT requests".
    if (!body.contains("notificationURI"))
        return problem(400, "MANDATORY_IE_MISSING", "notificationURI is mandatory",
                       json::array({invalidParam("/notificationURI", "mandatory attribute is missing")}));

    // 3. Capability: the subscription-wide reporting requirements.
    const json evt_req = body.value("evtReq", json::object());
    static const std::set<std::string> evt_req_supported = {
        "immRep", "notifMethod", "maxReportNbr", "monDur", "repPeriod"};
    json unsupported = json::array();
    for (auto it = evt_req.begin(); it != evt_req.end(); ++it) {
        if (evt_req_supported.count(it.key())) continue;
        if (it.key() == "notifFlag") {
            // §4.2.2.2.2: notifFlag applies only "if the EneNA feature is
            // supported"; this NWDAF does not support EneNA, so per TS 29.500
            // §6.6.2 the attribute is ignored (I-2).
            spdlog::info("EventsSubscription: ignoring evtReq/notifFlag (EneNA not supported)");
            continue;
        }
        unsupported.push_back(invalidParam("/evtReq/" + it.key(), "not supported by this NWDAF"));
    }
    if (evt_req.contains("notifMethod")) {
        const auto m = evt_req["notifMethod"].get<std::string>();
        if (m != "PERIODIC" && m != "ONE_TIME" && m != "ON_EVENT_DETECTION")
            unsupported.push_back(invalidParam("/evtReq/notifMethod",
                                               "notification method " + m + " is not supported by this NWDAF"));
    }
    if (!unsupported.empty())
        return problem(400, "OPTIONAL_IE_INCORRECT",
                       "the reporting requirements use functionality this NWDAF does not support",
                       unsupported, local.toHex());

    // 4. Supported features (TS 29.500 §6.6.2): the intersection.
    NwdafFeatureSet consumer;
    if (body.contains("supportedFeatures"))
        consumer = *NwdafFeatureSet::parse(body["supportedFeatures"].get<std::string>());
    out.negotiated = local.intersect(consumer);

    // 5. Per event.
    const auto advertised = NwdafAnalyticsCatalogue::rel18Advertised(config_);
    std::optional<NwdafReportInputs> in;   // fetched only when a slice period needs checking
    const auto& subs = body["eventSubscriptions"];
    for (size_t i = 0; i < subs.size(); ++i) {
        const json& es = subs[i];
        const std::string event = es["event"].get<std::string>();

        // §4.2.2.2.2: statistics and prediction together reject the request.
        if (es.contains("extraReportReq") && bothStatisticsAndPrediction(es["extraReportReq"]))
            return problem(400, "BOTH_STAT_PRED_NOT_ALLOWED",
                           "the analytics target period of " + event +
                           " starts in the past and ends in the future");

        if (!advertised.count(event)) {
            out.failed.push_back({{"event", event}, {"failureCode", "OTHER"}});
            continue;
        }

        const std::string at = "/eventSubscriptions/" + std::to_string(i);
        const std::string method = effectiveMethod(evt_req, es);
        if (thresholdMode(evt_req, es)) {
            if (auto rej = interpretThresholds(es, at)) {
                if (auto resp = subscriptionRejection(*rej, local, event, out.failed)) return resp;
                continue;
            }
        }
        const bool has_period = evt_req.contains("notifMethod") ? evt_req.contains("repPeriod")
                                                                 : es.contains("repetitionPeriod");
        if (method == "PERIODIC" && !has_period)
            return problem(400, "MANDATORY_IE_MISSING",
                           "PERIODIC reporting requires a repetition period",
                           json::array({invalidParam(evt_req.contains("notifMethod")
                                                         ? "/evtReq/repPeriod" : at + "/repetitionPeriod",
                                                     "mandatory for PERIODIC reporting")}));

        // Event-specific inputs.
        const std::string req_at = at + "/extraReportReq";
        std::optional<Rejection> rej;
        if (event == "NF_LOAD") {
            Nwdaf3gppAdapter::NfLoadQuery query;
            rej = interpretNfLoad(member(es, "tgtUe"), at + "/tgtUe", es, at,
                                  member(es, "extraReportReq"), req_at, query);
        } else if (event == "SLICE_LOAD_LEVEL" || event == "NSI_LOAD_LEVEL") {
            Nwdaf3gppAdapter::SliceQuery query;
            rej = interpretSliceLoad(event, &es, at, member(es, "extraReportReq"), req_at, query);
            if (!rej && query.from) {
                if (!in) in = inputs();
                rej = sliceHistoryCovers(query, *in, config_, req_at);
            }
        } else if (event == "UE_MOBILITY") {
            Nwdaf3gppAdapter::UeMobilityQuery query;
            rej = interpretUeMobility(member(es, "tgtUe"), at + "/tgtUe", &es, at,
                                      member(es, "extraReportReq"), req_at, query);
            if (!rej && query.from) {
                if (!in) in = inputs();
                rej = ueMobilityHistoryCovers(query, *in, config_, req_at);
            }
        } else if (event == "NETWORK_PERFORMANCE") {
            Nwdaf3gppAdapter::NwPerfQuery query;
            rej = interpretNetworkPerformance(member(es, "tgtUe"), at + "/tgtUe", &es, at,
                                              member(es, "extraReportReq"), req_at, true, config_, query);
            if (!rej && query.from) {
                if (!in) in = inputs();
                rej = nwPerfHistoryCovers(query, *in, config_, req_at);
            }
        }
        if (rej) {
            if (auto resp = subscriptionRejection(*rej, local, event, out.failed)) return resp;
            continue;
        }
        out.accepted.push_back(i);
    }

    // I-3: when no event at all can be served there is nothing to subscribe
    // to; reject rather than create an empty subscription.
    if (out.accepted.empty()) {
        json ip = json::array();
        for (size_t i = 0; i < subs.size(); ++i)
            ip.push_back(invalidParam("/eventSubscriptions/" + std::to_string(i) + "/event",
                                      "analytics not supported by this NWDAF"));
        return problem(400, "MANDATORY_IE_INCORRECT",
                       "none of the requested analytics is supported by this NWDAF",
                       ip, local.toHex());
    }
    return std::nullopt;
}

// The resource representation of an accepted subscription: the accepted
// events only, the negotiated features, and the per-event failures.
static json representation(const json& request, const std::vector<size_t>& accepted,
                           const json& failed, const NwdafFeatureSet& negotiated) {
    json rep = request;
    json events = json::array();
    for (size_t i : accepted) events.push_back(request["eventSubscriptions"][i]);
    rep["eventSubscriptions"] = events;
    rep["supportedFeatures"]  = negotiated.toHex();
    if (!failed.empty()) rep["failEventReports"] = failed;
    else rep.erase("failEventReports");
    rep.erase("eventNotifications");   // NWDAF-supplied, never taken from the request
    // I-2: EneNA-only attributes are not applied, so they are not echoed as if
    // they were in effect.
    if (rep.contains("notifCorrId")) {
        spdlog::info("EventsSubscription: ignoring notifCorrId (EneNA not supported)");
        rep.erase("notifCorrId");
    }
    if (rep.contains("evtReq")) rep["evtReq"].erase("notifFlag");
    return rep;
}

std::vector<json> NwdafSbiService::eventReports(const json& es, const NwdafReportInputs& in,
                                                const NwdafConfig& config) {
    const std::string event = es.value("event", "");
    json head = timeStamps(config);
    head["event"] = event;
    std::vector<json> out;
    if (event == "NF_LOAD") {
        const auto query = nfLoadQueryOf(es);
        if (!query) return out;   // cannot happen for an accepted event
        json infos = Nwdaf3gppAdapter::nfLoadLevelInfos(in.metrics, in.nf_instance_ids, in.statuses, *query);
        if (infos.empty()) return out;
        head["nfLoadLevelInfos"] = infos;
        out.push_back(head);
    } else if (event == "SLICE_LOAD_LEVEL" || event == "NSI_LOAD_LEVEL") {
        const auto query = sliceQueryOf(es);
        if (!query) return out;   // cannot happen for an accepted event
        return sliceNotifications(Nwdaf3gppAdapter::sliceLoads(config, *query, in.amf_oam, in.smf_oam,
                                                               std::chrono::system_clock::now()),
                                  head);
    } else if (event == "NETWORK_PERFORMANCE") {
        const auto query = nwPerfQueryOf(es, config);
        if (!query) return out;   // cannot happen for an accepted event
        json infos = Nwdaf3gppAdapter::nwPerfInfos(config, *query, in.amf_oam, in.smf_oam, in.ue_locations.get(),
                                                   std::chrono::system_clock::now());
        if (infos.empty()) return out;
        head["nwPerfs"] = infos;
        out.push_back(head);
    } else if (event == "UE_MOBILITY") {
        const auto query = ueMobilityQueryOf(es);
        if (!query || !in.ue_locations) return out;
        json mobs = Nwdaf3gppAdapter::ueMobilities(config, *query, *in.ue_locations,
                                                   std::chrono::system_clock::now());
        if (mobs.empty()) return out;
        head["ueMobs"] = mobs;
        out.push_back(head);
    }
    return out;
}

SbiResponse NwdafSbiService::createSubscription(const SbiRequest& req) {
    SubscriptionOutcome out;
    if (auto err = evaluateSubscription(req, out)) return *err;

    json rep = representation(out.request, out.accepted, out.failed, out.negotiated);
    const std::string id = subs_.createRel18(rep);
    const std::string location = req.api_root + EVENTS_SUBSCRIPTION_ROOT + "/subscriptions/" + id;

    // §4.2.2.2.2: with immRep, "the reports of the events subscribed, if
    // available" are included in the response.
    json body = rep;
    if (rep.contains("evtReq") && rep["evtReq"].value("immRep", false)) {
        json reports = json::array();
        const NwdafReportInputs in = inputs();
        for (const auto& es : rep["eventSubscriptions"])
            for (auto& r : eventReports(es, in, config_)) reports.push_back(std::move(r));
        if (!reports.empty()) body["eventNotifications"] = reports;
    }
    spdlog::info("EventsSubscription: created {} ({} event(s), {} failed)",
                 id, rep["eventSubscriptions"].size(), out.failed.size());
    return {201, "application/json", body.dump(), {{"Location", location}}};
}

SbiResponse NwdafSbiService::modifySubscription(const SbiRequest& req, const std::string& id) {
    if (!subs_.exists(id) || subs_.get(id).kind != "rel18")
        return problem(404, "SUBSCRIPTION_NOT_FOUND", "no subscription " + id);
    SubscriptionOutcome out;
    if (auto err = evaluateSubscription(req, out)) return *err;
    json rep = representation(out.request, out.accepted, out.failed, out.negotiated);
    if (!subs_.replaceRel18(id, rep))   // deleted concurrently
        return problem(404, "SUBSCRIPTION_NOT_FOUND", "no subscription " + id);
    return {200, "application/json", rep.dump(), {}};
}

SbiResponse NwdafSbiService::deleteSubscription(const std::string& id) {
    if (!subs_.exists(id) || subs_.get(id).kind != "rel18" || !subs_.remove(id))
        return problem(404, "SUBSCRIPTION_NOT_FOUND", "no subscription " + id);
    return {204, "", "", {}};
}
