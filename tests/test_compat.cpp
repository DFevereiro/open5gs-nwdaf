// COMPAT-01 / COMPAT-02 — what a core can feed decides what is advertised;
// the Open5GS workarounds are registered, tagged in the code and verified.
#include <catch2/catch_test_macros.hpp>
#include "nwdaf_analytics_catalogue.hpp"
#include "nwdaf_data_sources.hpp"
#include "nwdaf_open5gs_compat.hpp"
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using Cat = NwdafAnalyticsCatalogue;
using Src = NwdafDataSources;

TEST_CASE("COMPAT-01: every implemented analytics names its inputs") {
    for (const auto& id : Cat::REL18_IMPLEMENTED) {
        INFO("analyticsId=" << id);
        REQUIRE(Src::analyticsRequirements().count(id) == 1);
    }
}

TEST_CASE("COMPAT-01: advertisement follows the inputs the configuration provides") {
    NwdafConfig cfg;   // no sources at all
    REQUIRE(Cat::rel18Advertised(cfg).empty());

    // The AMF's UE list alone: UE locations, so UE_MOBILITY and NUM_OF_UE.
    cfg.amf_ue_info_endpoint = "http://127.0.0.5:9090/ue-info";
    REQUIRE(Cat::rel18Advertised(cfg) == std::set<std::string>{"NETWORK_PERFORMANCE", "UE_MOBILITY"});

    // A slice capacity without a count source can't be served, so isn't advertised.
    cfg.slice_capacity = {NwdafSliceCapacity{1, "", 10, 0}};
    REQUIRE(Cat::rel18Advertised(cfg).count("SLICE_LOAD_LEVEL") == 0);
    cfg.oam_metrics_endpoints = {{"SMF", "http://127.0.0.4:9090/metrics"}};
    REQUIRE(Cat::rel18Advertised(cfg).count("SLICE_LOAD_LEVEL") == 1);
    REQUIRE(Cat::rel18Advertised(cfg).count("NSI_LOAD_LEVEL") == 1);

    cfg.nf_instance_ids = {{"AMF", "11111111-1111-4111-8111-111111111111"}};
    REQUIRE(Cat::rel18Advertised(cfg).count("NF_LOAD") == 1);
}

TEST_CASE("COMPAT-01: each input reports its source, or none") {
    NwdafConfig cfg;
    for (auto input : Src::all()) REQUIRE_FALSE(Src::sourceOf(input, cfg));
    cfg.amf_ue_info_endpoint = "http://127.0.0.5:9090/ue-info";
    cfg.oam_metrics_endpoints = {{"AMF", "http://127.0.0.5:9090/metrics"}};
    cfg.nrf_nf_discovery = true;
    REQUIRE(*Src::sourceOf(NwdafInput::UeLocations, cfg) == "open5gs:amf-ue-info");
    REQUIRE(*Src::sourceOf(NwdafInput::UeCountPerSlice, cfg) == "open5gs:amf-metrics");
    REQUIRE(*Src::sourceOf(NwdafInput::NfInstanceIds, cfg) == "nrf");
    cfg.nf_instance_ids = {{"AMF", "11111111-1111-4111-8111-111111111111"}};
    REQUIRE(*Src::sourceOf(NwdafInput::NfInstanceIds, cfg) == "config");   // configured wins
    REQUIRE_FALSE(Src::sourceOf(NwdafInput::SessionSetupCounters, cfg));
}

static std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

TEST_CASE("COMPAT-02: every Open5GS workaround is tagged in the code and has an interoperability record") {
    const std::filesystem::path root = NWDAF_SOURCE_DIR;
    std::string code;
    for (const char* dir : {"src", "include"})
        for (const auto& e : std::filesystem::recursive_directory_iterator(root / dir))
            if (e.is_regular_file() && e.path().filename().string().find("open5gs_compat") == std::string::npos)
                code += readAll(e.path());
    const std::string records = readAll(root / "docs" / "3gpp-rel18-compliance.md");

    std::set<std::string> ids;
    for (const auto& q : NwdafOpen5gsCompat::quirks()) {
        INFO(q.id);
        REQUIRE(ids.insert(q.id).second);                                    // unique
        REQUIRE(code.find(q.id) != std::string::npos);                       // tagged at the handling code
        REQUIRE(records.find(std::string("| ") + q.record + " |") != std::string::npos);   // a dated record
    }
}

TEST_CASE("COMPAT-02: only the verified Open5GS versions count as verified") {
    REQUIRE(NwdafOpen5gsCompat::verified("2.8.0"));
    REQUIRE_FALSE(NwdafOpen5gsCompat::verified("2.7.6"));
    REQUIRE_FALSE(NwdafOpen5gsCompat::verified(""));
}
