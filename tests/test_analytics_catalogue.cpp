// H1.7 — analytics-ID catalogue: the known / operator / implemented /
// advertised sets and their invariants (docs/3gpp-rel18-compliance.md §4).
#include <catch2/catch_test_macros.hpp>
#include "nwdaf_analytics_catalogue.hpp"
#include "nwdaf_subscription.hpp"
#include <algorithm>
#include <memory>

using Cat = NwdafAnalyticsCatalogue;

static bool isSubset(const std::set<std::string>& a, const std::set<std::string>& b) {
    return std::includes(b.begin(), b.end(), a.begin(), a.end());
}

TEST_CASE("H1.7: KNOWN_REL18 is the 21-value Rel-18 NwdafEvent enum") {
    REQUIRE(Cat::KNOWN_REL18.size() == 21);
    // Spellings that pre-Rel-18 builds of this project got wrong.
    REQUIRE(Cat::KNOWN_REL18.count("QOS_SUSTAINABILITY") == 1);
    REQUIRE(Cat::KNOWN_REL18.count("RED_TRANS_EXP") == 1);
    REQUIRE(Cat::KNOWN_REL18.count("QoS_SUSTAINABILITY") == 0);
    REQUIRE(Cat::KNOWN_REL18.count("REDUNDANT_TRANSMISSION") == 0);
}

TEST_CASE("H1.7: every operator-API ID is a Rel-18 NwdafEvent value") {
    // Regression guard for G2: an ID the NWDAF serves must be spelled as the
    // Rel-18 enum spells it.
    for (const auto& id : Cat::OPERATOR_IDS) {
        INFO("analyticsId=" << id);
        REQUIRE(Cat::KNOWN_REL18.count(id) == 1);
    }
}

TEST_CASE("H1.7: advertised ⊆ implemented ⊆ known") {
    NwdafConfig cfg;
    REQUIRE(isSubset(Cat::REL18_IMPLEMENTED, Cat::KNOWN_REL18));
    REQUIRE(isSubset(Cat::rel18Advertised(cfg), Cat::REL18_IMPLEMENTED));
}

TEST_CASE("H1.7: legacy spellings map to Rel-18 IDs on the operator API") {
    REQUIRE(Cat::canonicalOperatorId("QoS_SUSTAINABILITY") == "QOS_SUSTAINABILITY");
    REQUIRE(Cat::canonicalOperatorId("REDUNDANT_TRANSMISSION") == "RED_TRANS_EXP");
    // Canonical and unknown input pass through untouched; validation is the
    // caller's job.
    REQUIRE(Cat::canonicalOperatorId("QOS_SUSTAINABILITY") == "QOS_SUSTAINABILITY");
    REQUIRE(Cat::canonicalOperatorId("NF_LOAD") == "NF_LOAD");
    REQUIRE(Cat::canonicalOperatorId("INVALID") == "INVALID");
}

// In-memory backend preloaded with rows as a pre-Rel-18 build persisted them.
class LegacyRowsBackend : public NwdafSubscriptionBackend {
public:
    std::vector<Subscription> rows;
    std::vector<Subscription> persisted;
    void persist(const Subscription& sub) override { persisted.push_back(sub); }
    bool remove(const std::string&) override { return true; }
    std::vector<Subscription> loadAll() override { return rows; }
};

static Subscription legacyRow(const std::string& id, const std::string& analytics_id) {
    Subscription s;
    s.sub_id = id;
    s.analytics_id = analytics_id;
    s.notif_uri = "http://127.0.0.1:9999/cb";
    s.rep_period_seconds = 60;
    s.max_report_nbr = 0;
    s.status = "ACTIVE";
    return s;
}

TEST_CASE("H1.4: persisted subscriptions with legacy IDs are migrated on load") {
    auto backend = std::make_shared<LegacyRowsBackend>();
    backend->rows = {legacyRow("sub-a", "QoS_SUSTAINABILITY"),
                     legacyRow("sub-b", "REDUNDANT_TRANSMISSION"),
                     legacyRow("sub-c", "NF_LOAD")};
    NwdafSubscriptionStore store(backend);

    REQUIRE(store.get("sub-a").analytics_id == "QOS_SUSTAINABILITY");
    REQUIRE(store.get("sub-b").analytics_id == "RED_TRANS_EXP");
    REQUIRE(store.get("sub-c").analytics_id == "NF_LOAD");
    // Only the migrated rows are written back.
    REQUIRE(backend->persisted.size() == 2);
}

TEST_CASE("H1.4: a subscription created with a legacy ID is stored canonically") {
    NwdafSubscriptionStore store;
    auto id = store.create({{"analyticsId", "QoS_SUSTAINABILITY"},
                            {"notifUri", "http://127.0.0.1:9999/cb"}});
    REQUIRE(store.get(id).analytics_id == "QOS_SUSTAINABILITY");
}
