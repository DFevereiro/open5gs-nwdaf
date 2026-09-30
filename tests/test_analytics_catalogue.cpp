// H1.7 — analytics-ID catalogue: the known / operator / implemented /
// advertised sets and their invariants (docs/3gpp-rel18-compliance.md §4).
#include <catch2/catch_test_macros.hpp>
#include "nwdaf_analytics_catalogue.hpp"
#include "nwdaf_subscription.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
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

TEST_CASE("H1.7: advertisement follows configured capability, not runtime data") {
    // rel18Advertised takes configuration only — there is no runtime input
    // that could make it flap. NF_LOAD needs an NF instance-ID source.
    NwdafConfig cfg;
    REQUIRE(Cat::REL18_IMPLEMENTED.count("NF_LOAD") == 1);
    REQUIRE(Cat::rel18Advertised(cfg).count("NF_LOAD") == 0);
    cfg.nf_instance_ids = {{"AMF", "11111111-1111-4111-8111-111111111111"}};
    REQUIRE(Cat::rel18Advertised(cfg).count("NF_LOAD") == 1);
}

TEST_CASE("QOL-03: each implemented ID is either advertised or withheld with a reason") {
    NwdafConfig cfg;
    auto check = [&] {
        const auto adv = Cat::rel18Advertised(cfg);
        const auto withheld = Cat::rel18NotAdvertised(cfg);
        for (const auto& id : Cat::REL18_IMPLEMENTED) {
            INFO("analyticsId=" << id);
            REQUIRE(adv.count(id) + withheld.count(id) == 1);
        }
        for (const auto& [id, reason] : withheld) {
            REQUIRE(Cat::REL18_IMPLEMENTED.count(id) == 1);
            REQUIRE_FALSE(reason.empty());
        }
    };
    check();
    REQUIRE(Cat::rel18NotAdvertised(cfg).at("NETWORK_PERFORMANCE") ==
            "served_tai_list (or amf_ue_info_endpoint) is not configured");
    REQUIRE(Cat::rel18NotAdvertised(cfg).at("SLICE_LOAD_LEVEL") == "slice_capacity is not configured");

    cfg.served_tai_list.push_back({"999", "70", "000001"});
    check();
    REQUIRE(Cat::rel18NotAdvertised(cfg).at("NETWORK_PERFORMANCE") == "no AMF or SMF in oam_metrics_endpoints");
    cfg.oam_metrics_endpoints["SMF"] = "http://127.0.0.4:9090/metrics";
    cfg.nrf_nf_discovery = true;
    check();
    REQUIRE(Cat::rel18NotAdvertised(cfg).count("NETWORK_PERFORMANCE") == 0);
    REQUIRE(Cat::rel18NotAdvertised(cfg).count("NF_LOAD") == 0);
}

TEST_CASE("H1.7: nf_instance_ids must name monitored NF types and hold UUIDs") {
    const std::string path = "/tmp/nwdaf_nf_ids_config.yaml";
    auto load = [&](const std::string& ids) {
        std::ofstream(path) << "nwdaf:\n"
                            << "  nf_instance_id: \"00000000-0000-0000-0000-000000000000\"\n"
                            << "  nf_instance_ids:\n" << ids;
        return NwdafConfig::load(path);
    };
    REQUIRE(load("    AMF: \"11111111-1111-4111-8111-111111111111\"\n")
                .nf_instance_ids.at("AMF") == "11111111-1111-4111-8111-111111111111");
    REQUIRE_THROWS(load("    AMF: \"not-a-uuid\"\n"));
    REQUIRE_THROWS(load("    XYZ: \"11111111-1111-4111-8111-111111111111\"\n"));
    std::remove(path.c_str());
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

#ifdef NWDAF_HAS_SQLITE
#include <sqlite3.h>

TEST_CASE("H1.7: a pre-Rel-18 subscription database is migrated in place") {
    const std::string path = "/tmp/nwdaf_subs_migration.db";
    std::remove(path.c_str());
    {   // The schema and a row as an earlier release wrote them.
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        REQUIRE(sqlite3_exec(db,
            "CREATE TABLE nwdaf_subscriptions (sub_id TEXT PRIMARY KEY,"
            " analytics_id TEXT, notif_uri TEXT, notif_id TEXT, rep_period INTEGER,"
            " max_report_nbr INTEGER, report_count INTEGER, created_at TEXT, status TEXT);"
            "INSERT INTO nwdaf_subscriptions VALUES ('sub-old','NF_LOAD','http://x/cb','',60,0,0,"
            "'2026-01-01T00:00:00Z','ACTIVE');",
            nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    {
        NwdafSubscriptionStore store(std::make_shared<SqliteSubscriptionBackend>(path));
        REQUIRE(store.get("sub-old").kind == "legacy");
        json rep = {{"eventSubscriptions", {{{"event", "NF_LOAD"}}}},
                    {"notificationURI", "http://127.0.0.1:9/n"}};
        const std::string id = store.createRel18(rep);
        REQUIRE(store.get(id).kind == "rel18");
    }
    {   // Both rows survive a restart with their kind.
        NwdafSubscriptionStore store(std::make_shared<SqliteSubscriptionBackend>(path));
        int legacy = 0, rel18 = 0;
        for (const auto& s : store.listAll()) (s.kind == "rel18" ? rel18 : legacy)++;
        REQUIRE(legacy == 1);
        REQUIRE(rel18 == 1);
    }
    std::remove(path.c_str());
}
#endif
