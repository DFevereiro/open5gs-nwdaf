// H1.7 — THRESHOLD / ON_EVENT_DETECTION reporting (TS 29.520 V18.14.0
// §4.2.2.2.2, Tables 5.1.6.2.3-1 and 5.1.6.3.12-1; TS 23.288 §6.1.3; I-10):
// crossing detection per matching direction, and schema-valid notifications.
#include <catch2/catch_test_macros.hpp>
#include "official_schema.hpp"
#include "nwdaf_sbi.hpp"
#include "nwdaf_schema_validator.hpp"

using json = nlohmann::json;
using Clock = std::chrono::system_clock;
using Svc = NwdafSbiService;

static const char* AMF_ID = "11111111-1111-4111-8111-111111111111";

static void requireNotification(const json& n) {
    requireOfficialSchema(n, "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/EventNotification");
}

TEST_CASE("H1.7: THRESHOLD is the default method; evtReq ON_EVENT_DETECTION selects it for every event") {
    REQUIRE(Svc::thresholdMode(json::object(), {{"event", "NF_LOAD"}}));
    REQUIRE(Svc::thresholdMode(json::object(), {{"event", "NF_LOAD"}, {"notificationMethod", "THRESHOLD"}}));
    REQUIRE_FALSE(Svc::thresholdMode(json::object(), {{"event", "NF_LOAD"}, {"notificationMethod", "PERIODIC"}}));
    // evtReq supersedes the event's own method (§4.2.2.2.2 NOTE 1).
    REQUIRE(Svc::thresholdMode({{"notifMethod", "ON_EVENT_DETECTION"}},
                               {{"event", "NF_LOAD"}, {"notificationMethod", "PERIODIC"}}));
    REQUIRE_FALSE(Svc::thresholdMode({{"notifMethod", "PERIODIC"}}, {{"event", "NF_LOAD"}}));
}

TEST_CASE("H1.7: threshold inputs per event") {
    // NF_LOAD: nfLoadLvlThds, CPU usage only (I-5).
    auto r = Svc::interpretThresholds({{"event", "NF_LOAD"}}, "/e");
    REQUIRE(r->kind == Svc::Rejection::MandatoryMissing);
    REQUIRE(r->where == "/e/nfLoadLvlThds");
    REQUIRE(Svc::interpretThresholds({{"event", "NF_LOAD"}, {"nfLoadLvlThds", {{{"nfLoadLevel", 50}}}}}, "/e")->kind ==
            Svc::Rejection::Unsupported);
    REQUIRE_FALSE(Svc::interpretThresholds({{"event", "NF_LOAD"}, {"nfLoadLvlThds", {{{"nfCpuUsage", 50}}}}}, "/e"));
    // SLICE_LOAD_LEVEL: loadLevelThreshold.
    REQUIRE(Svc::interpretThresholds({{"event", "SLICE_LOAD_LEVEL"}, {"anySlice", true}}, "/e")->kind ==
            Svc::Rejection::MandatoryMissing);
    // NSI_LOAD_LEVEL: one threshold, or one per nsiIdInfos entry.
    const json nsi = {{"event", "NSI_LOAD_LEVEL"}, {"nsiIdInfos", {{{"snssai", {{"sst", 1}}}}, {{"snssai", {{"sst", 2}}}}}}};
    REQUIRE(Svc::interpretThresholds(nsi, "/e")->kind == Svc::Rejection::MandatoryMissing);
    json ok = nsi;  ok["nsiLevelThrds"] = {40};
    REQUIRE_FALSE(Svc::interpretThresholds(ok, "/e"));
    ok["nsiLevelThrds"] = {40, 60};
    REQUIRE_FALSE(Svc::interpretThresholds(ok, "/e"));
    ok["nsiLevelThrds"] = {40, 60, 80};
    REQUIRE(Svc::interpretThresholds(ok, "/e")->kind == Svc::Rejection::MandatoryIncorrect);
}

static NwdafReportInputs cpu(double pct) {
    NwdafReportInputs in;
    in.metrics = {{"AMF", "active", 101, 1.0, 1000, pct, "LOW"}};
    in.nf_instance_ids = {{"AMF", AMF_ID}};
    return in;
}

// The CPU values seen in turn, and which of them produced a report.
static std::vector<bool> run(const json& es, const std::vector<double>& values) {
    NwdafConfig cfg;
    Svc::ThresholdState state;
    std::vector<bool> out;
    for (double v : values) {
        auto rs = Svc::thresholdReports(es, cpu(v), cfg, state);
        for (const auto& n : rs) requireNotification(n);
        out.push_back(!rs.empty());
    }
    return out;
}

TEST_CASE("H1.7: NF_LOAD thresholds follow the matching direction (I-10)") {
    json es = {{"event", "NF_LOAD"}, {"tgtUe", {{"anyUe", true}}}, {"nfLoadLvlThds", {{{"nfCpuUsage", 50}}}}};
    // The first value is a baseline only; 50 counts as reached ("met or exceeded").
    const std::vector<double> values = {30, 50, 70, 40, 60};

    es["matchingDir"] = "ASCENDING";
    REQUIRE(run(es, values) == std::vector<bool>{false, true, false, false, true});
    es["matchingDir"] = "DESCENDING";
    REQUIRE(run(es, values) == std::vector<bool>{false, false, false, true, false});
    es.erase("matchingDir");   // default CROSSED
    REQUIRE(run(es, values) == std::vector<bool>{false, true, false, true, true});
}

TEST_CASE("H1.7: an NF_LOAD threshold report carries the NFs that crossed") {
    const json es = {{"event", "NF_LOAD"}, {"tgtUe", {{"anyUe", true}}},
                     {"nfLoadLvlThds", {{{"nfCpuUsage", 80}}, {{"nfCpuUsage", 20}}}}};
    NwdafConfig cfg;
    Svc::ThresholdState state;
    REQUIRE(Svc::thresholdReports(es, cpu(50), cfg, state).empty());
    const auto rs = Svc::thresholdReports(es, cpu(10), cfg, state);   // crosses 20 downwards
    REQUIRE(rs.size() == 1);
    REQUIRE(rs[0]["event"] == "NF_LOAD");
    REQUIRE(rs[0]["nfLoadLevelInfos"].size() == 1);
    REQUIRE(rs[0]["nfLoadLevelInfos"][0]["nfInstanceId"] == AMF_ID);
    REQUIRE(rs[0]["nfLoadLevelInfos"][0]["nfCpuUsage"] == 10);
}

// Slice 1 has max 10 UEs; a scrape now with `ues` registered UEs.
static NwdafReportInputs slices(int ues1, int ues2) {
    NwdafReportInputs in;
    in.amf_oam = {{Clock::now() - std::chrono::seconds(1),
                   {{"fivegs_amffunction_rm_registeredsubnbr", {{"plmnid", "99970"}, {"snssai", "1"}}, double(ues1)},
                    {"fivegs_amffunction_rm_registeredsubnbr", {{"plmnid", "99970"}, {"snssai", "2"}}, double(ues2)}}}};
    return in;
}

static NwdafConfig sliceConfig() {
    NwdafConfig cfg;
    cfg.plmn_mcc = "999"; cfg.plmn_mnc = "70";
    cfg.slice_load_window_seconds = 5;
    NwdafSliceCapacity s1; s1.sst = 1; s1.max_ues = 10;
    NwdafSliceCapacity s2; s2.sst = 2; s2.max_ues = 10;
    cfg.slice_capacity = {s1, s2};
    return cfg;
}

TEST_CASE("H1.7: SLICE_LOAD_LEVEL reports the slices whose level crossed loadLevelThreshold") {
    const NwdafConfig cfg = sliceConfig();
    const json es = {{"event", "SLICE_LOAD_LEVEL"}, {"anySlice", true}, {"loadLevelThreshold", 50}};
    Svc::ThresholdState state;
    REQUIRE(Svc::thresholdReports(es, slices(4, 4), cfg, state).empty());   // baselines 40, 40
    const auto rs = Svc::thresholdReports(es, slices(6, 4), cfg, state);    // slice 1 → 60
    REQUIRE(rs.size() == 1);
    requireNotification(rs[0]);
    REQUIRE(rs[0]["sliceLoadLevelInfo"] == json{{"loadLevelInformation", 60}, {"snssais", {{{"sst", 1}}}}});
    REQUIRE(Svc::thresholdReports(es, slices(6, 4), cfg, state).empty());   // no new crossing
}

TEST_CASE("H1.7: NSI_LOAD_LEVEL applies nsiLevelThrds per nsiIdInfos entry") {
    const NwdafConfig cfg = sliceConfig();
    const json es = {{"event", "NSI_LOAD_LEVEL"},
                     {"nsiIdInfos", {{{"snssai", {{"sst", 1}}}}, {{"snssai", {{"sst", 2}}}}}},
                     {"nsiLevelThrds", {70, 30}}};
    Svc::ThresholdState state;
    REQUIRE(Svc::thresholdReports(es, slices(5, 2), cfg, state).empty());   // 50, 20
    const auto rs = Svc::thresholdReports(es, slices(6, 3), cfg, state);    // 60 (< 70), 30 (reaches 30)
    REQUIRE(rs.size() == 1);
    requireNotification(rs[0]);
    REQUIRE(rs[0]["nsiLoadLevelInfos"] == json::array({{{"loadLevelInformation", 30}, {"snssai", {{"sst", 2}}}}}));
}
