// H1.7 — NwdafSchemaValidator against the official 3GPP Rel-18 artifacts
// pinned in docs/frozen-standards.md (materialised by cmake/Nwdaf3gppOpenApi.cmake).
#include <catch2/catch_test_macros.hpp>
#include "nwdaf_schema_validator.hpp"
#include "nwdaf_analytics_catalogue.hpp"
#include <set>

using json = nlohmann::json;

static const std::string EVTS = "TS29520_Nnwdaf_EventsSubscription.yaml";
static const std::string ANLY = "TS29520_Nnwdaf_AnalyticsInfo.yaml";

static NwdafSchemaValidator& official() {
    static NwdafSchemaValidator v(NWDAF_3GPP_OPENAPI_DIR);
    return v;
}

static std::string sch(const std::string& file, const std::string& name) {
    return file + "#/components/schemas/" + name;
}

TEST_CASE("H1.7: the pinned artifacts are the TS 29.520 Rel-18 APIs this NWDAF targets") {
    // docs/frozen-standards.md: EventsSubscription 1.3.3, AnalyticsInfo 1.3.5.
    REQUIRE(official().document(EVTS)["info"]["version"] == "1.3.3");
    REQUIRE(official().document(ANLY)["info"]["version"] == "1.3.5");
}

TEST_CASE("H1.7: KNOWN_REL18 equals the official NwdafEvent enumeration") {
    // NwdafEvent is an extensible enum: anyOf [ {enum: [...]}, {type: string} ].
    std::set<std::string> official_ids;
    for (const auto& branch : official().schema(sch(EVTS, "NwdafEvent"))["anyOf"])
        if (branch.contains("enum"))
            for (const auto& v : branch["enum"]) official_ids.insert(v.get<std::string>());
    REQUIRE(official_ids == NwdafAnalyticsCatalogue::KNOWN_REL18);
}

TEST_CASE("H1.7: a minimal NnwdafEventsSubscription is schema-valid") {
    json sub = {{"eventSubscriptions", {{{"event", "NF_LOAD"}}}},
                {"notificationURI", "http://127.0.0.1:9999/cb"}};
    auto v = official().validate(sub, sch(EVTS, "NnwdafEventsSubscription"));
    INFO((v.empty() ? std::string() : v.front().pointer + " " + v.front().reason));
    REQUIRE(v.empty());
}

TEST_CASE("H1.7: missing mandatory attributes are reported as JSON Pointers") {
    auto v = official().validate(json::object(), sch(EVTS, "NnwdafEventsSubscription"));
    REQUIRE(v.size() == 1);
    REQUIRE(v.front().pointer == "/eventSubscriptions");

    json sub = {{"eventSubscriptions", {json::object()}}};
    v = official().validate(sub, sch(EVTS, "NnwdafEventsSubscription"));
    REQUIRE(v.size() == 1);
    REQUIRE(v.front().pointer == "/eventSubscriptions/0/event");
}

TEST_CASE("H1.7: an unknown event value is schema-valid (extensible enum)") {
    // TS 29.500 §6.6.2: unknown values are the capability layer's concern, not
    // a schema violation. The validator must not be stricter than Rel-18.
    json sub = {{"eventSubscriptions", {{{"event", "SOME_FUTURE_EVENT"}}}}};
    REQUIRE(official().validate(sub, sch(EVTS, "NnwdafEventsSubscription")).empty());
}

TEST_CASE("H1.7: cross-document references are followed (TS 29.571 SupportedFeatures)") {
    json sub = {{"eventSubscriptions", {{{"event", "NF_LOAD"}}}},
                {"supportedFeatures", "not-hex"}};
    auto v = official().validate(sub, sch(EVTS, "NnwdafEventsSubscription"));
    REQUIRE(v.size() == 1);
    REQUIRE(v.front().pointer == "/supportedFeatures");

    sub["supportedFeatures"] = "1A0";
    REQUIRE(official().validate(sub, sch(EVTS, "NnwdafEventsSubscription")).empty());
}

TEST_CASE("H1.7: oneOf requires exactly one matching alternative") {
    // NnwdafEventsSubscriptionNotification: oneOf [eventNotifications] |
    // [resourceUri + oldSubscriptionId].
    const auto ref = sch(EVTS, "NnwdafEventsSubscriptionNotification");
    json n = {{"subscriptionId", "s1"},
              {"eventNotifications", {{{"event", "NF_LOAD"}}}}};
    REQUIRE(official().validate(n, ref).empty());

    n["resourceUri"] = "http://nwdaf/x";
    n["oldSubscriptionId"] = "s0";
    REQUIRE_FALSE(official().validate(n, ref).empty());   // both branches match

    json neither = {{"subscriptionId", "s1"}};
    REQUIRE_FALSE(official().validate(neither, ref).empty());
}

TEST_CASE("H1.7: wrong types are reported, not coerced") {
    json sub = {{"eventSubscriptions", {{{"event", 7}}}}};
    auto v = official().validate(sub, sch(EVTS, "NnwdafEventsSubscription"));
    REQUIRE_FALSE(v.empty());
    REQUIRE(v.front().pointer == "/eventSubscriptions/0/event");
}

TEST_CASE("H1.7: an unresolvable reference is an error, not a pass") {
    REQUIRE_THROWS(official().validate(json::object(), sch(EVTS, "NoSuchSchema")));
    REQUIRE_THROWS(official().validate(json::object(), "NoSuchFile.yaml#/x"));
}
