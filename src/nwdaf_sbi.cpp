#include "nwdaf_sbi.hpp"
#include "nwdaf_analytics_catalogue.hpp"
#include <spdlog/spdlog.h>
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

// ── Construction / routing ──────────────────────────────────────────────────

NwdafSbiService::NwdafSbiService(NwdafAnalyticsEngine& engine,
                                 NwdafSubscriptionStore& subs,
                                 const NwdafConfig& config)
    : engine_(engine), subs_(subs), config_(config),
      validator_(config.openapi_3gpp_dir)
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
        // POST on this path is the deprecated, non-standard operator form and
        // is served by the operator API, not here.
        if (req.method == "GET") return getAnalytics(req);
        return methodNotAllowed("GET, POST");
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

    // Capability: an analytics ID is served only when its gating feature is
    // part of this NWDAF's own feature set (I-1).
    const std::string event = values["event-id"].get<std::string>();
    const auto feature = NwdafSupportedFeatures::eventFeature(NnwdafApi::AnalyticsInfo, event);
    if (!feature || !local.has(*feature))
        return problem(400, "MANDATORY_QUERY_PARAM_INCORRECT",
                       "analytics " + event + " is not supported by this NWDAF",
                       json::array({invalidParam("event-id", "analytics not supported by this NWDAF")}),
                       local.toHex());

    // Unreachable until per-ID Rel-18 mappings are registered (H1.7 Step 3):
    // an advertised ID always has a mapping.
    spdlog::error("AnalyticsInfo: {} is advertised but has no Rel-18 mapping", event);
    return problem(500, "SYSTEM_FAILURE", "no Rel-18 mapping for " + event);
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
        if (m != "PERIODIC" && m != "ONE_TIME")
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
    const auto& subs = body["eventSubscriptions"];
    for (size_t i = 0; i < subs.size(); ++i) {
        const json& es = subs[i];
        const std::string event = es["event"].get<std::string>();

        // §4.2.2.2.2: statistics and prediction together reject the request.
        if (es.contains("extraReportReq") && bothStatisticsAndPrediction(es["extraReportReq"]))
            return problem(400, "BOTH_STAT_PRED_NOT_ALLOWED",
                           "the analytics target period of " + event +
                           " starts in the past and ends in the future");

        const auto feature = NwdafSupportedFeatures::eventFeature(NnwdafApi::EventsSubscription, event);
        if (!feature || !local.has(*feature)) {
            out.failed.push_back({{"event", event}, {"failureCode", "OTHER"}});
            continue;
        }

        // Effective notification method: evtReq supersedes the event's own
        // (§4.2.2.2.2 NOTE 1); the event's default is THRESHOLD (Table
        // 5.1.6.2.3-1 NOTE 2), which this NWDAF does not support yet.
        const std::string method = evt_req.contains("notifMethod")
            ? evt_req["notifMethod"].get<std::string>()
            : es.value("notificationMethod", std::string("THRESHOLD"));
        if (method == "THRESHOLD") {
            out.failed.push_back({{"event", event}, {"failureCode", "OTHER"}});
            continue;
        }
        const bool has_period = evt_req.contains("notifMethod") ? evt_req.contains("repPeriod")
                                                                 : es.contains("repetitionPeriod");
        if (method == "PERIODIC" && !has_period)
            return problem(400, "MANDATORY_IE_MISSING",
                           "PERIODIC reporting requires a repetition period",
                           json::array({invalidParam(evt_req.contains("notifMethod")
                                                         ? "/evtReq/repPeriod"
                                                         : "/eventSubscriptions/" + std::to_string(i) + "/repetitionPeriod",
                                                     "mandatory for PERIODIC reporting")}));
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

SbiResponse NwdafSbiService::createSubscription(const SbiRequest& req) {
    SubscriptionOutcome out;
    if (auto err = evaluateSubscription(req, out)) return *err;
    // Unreachable until an analytics ID is advertised (H1.7 Step 3), which is
    // when subscription storage and Rel-18 notifications land with it.
    return problem(500, "SYSTEM_FAILURE", "Rel-18 subscriptions are not yet available");
}

SbiResponse NwdafSbiService::modifySubscription(const SbiRequest& req, const std::string& id) {
    (void)req;
    // No Individual NWDAF Event Subscription resource can exist before an
    // analytics ID is advertised (see createSubscription).
    return problem(404, "SUBSCRIPTION_NOT_FOUND", "no subscription " + id);
}

SbiResponse NwdafSbiService::deleteSubscription(const std::string& id) {
    return problem(404, "SUBSCRIPTION_NOT_FOUND", "no subscription " + id);
}
