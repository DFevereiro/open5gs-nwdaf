// QOL-05 — SIGHUP reload of the analytics settings: only the reloadable
// settings change, and the 3GPP interfaces, collector and NF monitor use the
// new values without a restart.
#include <catch2/catch_test_macros.hpp>
#include "mock_open5gs.hpp"
#include "nwdaf_analytics.hpp"
#include "nwdaf_analytics_catalogue.hpp"
#include "nwdaf_live_config.hpp"
#include "nwdaf_nf_monitor.hpp"
#include "nwdaf_sbi.hpp"
#include "nwdaf_subscription.hpp"
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>
#include <thread>

using json = nlohmann::json;

static const char* AMF_URL = "http://127.0.0.5:9090/metrics";
static const char* AMF_URL_2 = "http://127.0.0.15:9090/metrics";

static NwdafConfig reloadConfig() {
    NwdafConfig cfg;
    cfg.nf_instance_id = "7c1f4e2a-9b3d-4a6e-8f10-2d3c4b5a6e7f";
    cfg.plmn_mcc = "999"; cfg.plmn_mnc = "70";
    cfg.sbi_port = 7779; cfg.sbi_h2_port = 7780;
    cfg.nf_service_names = {{"AMF", "amfd"}, {"SMF", "smfd"}};
    cfg.collection_interval_seconds = 1;
    cfg.throughput_history_size = 10;
    cfg.oam_metrics_endpoints = {{"AMF", AMF_URL}};
    cfg.openapi_3gpp_dir = NWDAF_3GPP_OPENAPI_DIR;
    cfg.model_dir = "/tmp/nwdaf_reload_models";
    cfg.log_level = "warn"; cfg.log_file = "/tmp/nwdaf_reload_test.log";
    return cfg;
}

TEST_CASE("QOL-05: a reload takes only the reloadable settings") {
    const NwdafConfig running = reloadConfig();
    NwdafConfig fresh = running;
    fresh.sbi_port = 9999;                                  // restart-only
    fresh.tls_enabled = true;                               // restart-only
    fresh.slice_capacity = {NwdafSliceCapacity{1, "", 10, 0}};
    fresh.log_level = "debug";
    std::vector<std::string> changed;
    const NwdafConfig merged = NwdafLiveConfig::merge(running, fresh, changed);
    REQUIRE(changed == std::vector<std::string>{"log_level", "slice_capacity"});
    REQUIRE(merged.sbi_port == 7779);
    REQUIRE_FALSE(merged.tls_enabled);
    REQUIRE(merged.slice_capacity == fresh.slice_capacity);
    REQUIRE(merged.log_level == "debug");
}

TEST_CASE("QOL-05: RELOADABLE names exactly the settings merge() reloads") {
    const NwdafConfig running = reloadConfig();
    NwdafConfig fresh = running;   // every reloadable setting different
    fresh.log_level = "debug";
    fresh.collection_interval_seconds = 7;
    fresh.ewma_alpha = 0.7;
    fresh.anomaly_contamination = 0.2;
    fresh.oam_metrics_endpoints = {{"AMF", AMF_URL_2}};
    fresh.amf_ue_info_endpoint = "http://127.0.0.5:9090/ue-info";
    fresh.slice_capacity = {NwdafSliceCapacity{1, "", 10, 0}};
    fresh.served_tai_list = {{"999", "70", "000001"}};
    fresh.nf_instance_ids = {{"AMF", "11111111-1111-4111-8111-111111111111"}};
    fresh.slice_load_window_seconds = 11;
    fresh.network_performance_window_seconds = 12;
    fresh.ue_mobility_window_seconds = 13;
    fresh.prediction_horizon_seconds = 14;
    fresh.prediction_min_samples = 15;
    fresh.prediction_tolerance = 16;
    std::vector<std::string> changed;
    (void)NwdafLiveConfig::merge(running, fresh, changed);
    REQUIRE(changed == NwdafLiveConfig::RELOADABLE);
}

// A service over a mock collector that scraped the AMF once: 5 UEs on slice 1.
struct ReloadFixture {
    NwdafConfig                      cfg;
    MockNwdafCollector               collector;
    NwdafAnalyticsEngine             engine;
    NwdafSubscriptionStore           subs;
    std::shared_ptr<NwdafNfMonitor>  monitor;
    std::shared_ptr<NwdafLiveConfig> live;
    NwdafSbiService                  sbi;

    ReloadFixture()
        : cfg(reloadConfig()), collector(cfg), engine(collector, cfg),
          monitor(std::make_shared<NwdafNfMonitor>(cfg)), live(std::make_shared<NwdafLiveConfig>(cfg)),
          sbi(engine, subs, cfg, monitor, live) {
        collector.setOamMetrics(AMF_URL, "fivegs_amffunction_rm_registeredsubnbr{plmnid=\"99970\",snssai=\"1\"} 5\n");
        collector.setOamMetrics(AMF_URL_2, "fivegs_amffunction_rm_registeredsubnbr{plmnid=\"99970\",snssai=\"1\"} 8\n");
        tick();
    }
    void tick() {
        collector.startBackgroundCollection();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        collector.stopBackgroundCollection();
    }
    SbiResponse sliceLoad() {
        SbiRequest r;
        r.method = "GET";
        r.path = std::string(NwdafSbiService::ANALYTICS_INFO_ROOT) + "/analytics";
        r.query = {{"event-id", "LOAD_LEVEL_INFORMATION"}, {"event-filter", R"({"anySlice":true})"}};
        r.api_root = "http://127.0.0.1:7780";
        return sbi.dispatch(r);
    }
};

TEST_CASE("QOL-05: a reload changes what the 3GPP interfaces serve, without a restart") {
    ReloadFixture f;
    REQUIRE(f.sliceLoad().status == 400);   // no slice_capacity: not advertised

    NwdafConfig fresh = f.cfg;
    fresh.slice_capacity = {NwdafSliceCapacity{1, "", 10, 0}};
    const auto r = nwdafApplyReload(fresh, *f.live, f.collector, f.engine, *f.monitor, nullptr);
    REQUIRE(r.changed == std::vector<std::string>{"slice_capacity"});
    REQUIRE(r.profile_changed);   // SLICE_LOAD_LEVEL and NSI_LOAD_LEVEL now advertised
    REQUIRE_FALSE(r.nrf_updated);

    const SbiResponse res = f.sliceLoad();
    INFO(res.body);
    REQUIRE(res.status == 200);
    REQUIRE(json::parse(res.body)["sliceLoadLevelInfos"][0]["loadLevelInformation"] == 50);
}

TEST_CASE("QOL-05: raising log_level on reload reaches the log sinks") {
    // As setupLogging() builds it: the sink filters at the start-up level too.
    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    sink->set_level(spdlog::level::info);
    auto logger = std::make_shared<spdlog::logger>("reload-test", sink);
    logger->set_level(spdlog::level::info);
    const auto previous = spdlog::default_logger();
    spdlog::set_default_logger(logger);

    ReloadFixture f;
    NwdafConfig fresh = f.cfg;
    fresh.log_level = "debug";
    (void)nwdafApplyReload(fresh, *f.live, f.collector, f.engine, *f.monitor, nullptr);
    const bool sink_debug = sink->level() == spdlog::level::debug;
    const bool logger_debug = logger->level() == spdlog::level::debug;
    spdlog::set_default_logger(previous);
    REQUIRE(sink_debug);
    REQUIRE(logger_debug);
}

TEST_CASE("QOL-05: reloaded sources and NF instance IDs are used from the next tick") {
    ReloadFixture f;
    NwdafConfig fresh = f.cfg;
    fresh.oam_metrics_endpoints = {{"AMF", AMF_URL_2}};
    fresh.nf_instance_ids = {{"AMF", "11111111-1111-4111-8111-111111111111"}};
    (void)nwdafApplyReload(fresh, *f.live, f.collector, f.engine, *f.monitor, nullptr);
    f.tick();

    const auto sources = f.engine.getOamSources();
    REQUIRE(sources.size() == 1);
    REQUIRE(sources[0].endpoint == AMF_URL_2);
    REQUIRE(sources[0].up);
    REQUIRE(f.engine.getOamHistory("AMF").back().samples[0].value == 8);
    REQUIRE(f.monitor->ids().at("AMF") == "11111111-1111-4111-8111-111111111111");
}
