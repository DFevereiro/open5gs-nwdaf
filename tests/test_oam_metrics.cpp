// H1.1 — OAM input from the NFs' Prometheus metrics endpoints: the text-format
// parser, and the collector's scraping, against output captured from Open5GS
// v2.8.0 (tests/data/open5gs-v2.8.0-amf-metrics.txt).
#include <catch2/catch_test_macros.hpp>
#include "mock_open5gs.hpp"
#include "nwdaf_prometheus.hpp"
#include <httplib.h>
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>

static std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static const NwdafPromSample* find(const std::vector<NwdafPromSample>& all, const std::string& name,
                                   const std::map<std::string, std::string>& labels = {}) {
    for (const auto& s : all)
        if (s.name == name && s.labels == labels) return &s;
    return nullptr;
}

// The per-slice form Open5GS emits once UEs register (src/amf/metrics.c,
// labels plmnid and snssai).
static const char* SLICE_LINES =
    "# HELP fivegs_amffunction_rm_registeredsubnbr Number of registered state subscribers per AMF\n"
    "# TYPE fivegs_amffunction_rm_registeredsubnbr gauge\n"
    "fivegs_amffunction_rm_registeredsubnbr{plmnid=\"99970\",snssai=\"1-000001\"} 3\n"
    "fivegs_amffunction_rm_registeredsubnbr{plmnid=\"99970\",snssai=\"2\"} 1\n";

TEST_CASE("H1.1: Prometheus text from an Open5GS v2.8.0 AMF parses") {
    const std::string text = readFile(std::string(NWDAF_TEST_DATA_DIR) + "/open5gs-v2.8.0-amf-metrics.txt");
    REQUIRE_FALSE(text.empty());
    const auto samples = NwdafPrometheusText::parse(text);
    REQUIRE(find(samples, "gnb"));
    REQUIRE(find(samples, "ran_ue"));
    REQUIRE(find(samples, "fivegs_amffunction_rm_reginitreq"));
    REQUIRE(find(samples, "process_virtual_memory_max_bytes")->value == -1);
    for (const auto& s : samples) REQUIRE(s.name.rfind("#", 0) == std::string::npos);

    const auto slices = NwdafPrometheusText::parse(SLICE_LINES);
    REQUIRE(slices.size() == 2);
    REQUIRE(find(slices, "fivegs_amffunction_rm_registeredsubnbr",
                 {{"plmnid", "99970"}, {"snssai", "1-000001"}})->value == 3);
}

TEST_CASE("H1.1: Prometheus label escapes, special values and timestamps") {
    const auto s = NwdafPrometheusText::parse(
        "a{path=\"C:\\\\dir\",quote=\"say \\\"hi\\\"\",nl=\"x\\ny\",} 1.5 1700000000000\n"
        "b +Inf\n"
        "c NaN\r\n"
        "  d{} 2e3\n");
    REQUIRE(s.size() == 4);
    REQUIRE(s[0].labels.at("path") == "C:\\dir");
    REQUIRE(s[0].labels.at("quote") == "say \"hi\"");
    REQUIRE(s[0].labels.at("nl") == "x\ny");
    REQUIRE(s[0].value == 1.5);
    REQUIRE(std::isinf(s[1].value));
    REQUIRE(std::isnan(s[2].value));
    REQUIRE(s[3].name == "d");
    REQUIRE(s[3].value == 2000);
}

TEST_CASE("H1.1: malformed Prometheus lines are skipped") {
    const auto s = NwdafPrometheusText::parse(
        "ok 1\n"
        "1bad 2\n"               // name starts with a digit
        "novalue\n"
        "unterminated{a=\"x 3\n"
        "nolabelquote{a=x} 4\n"
        "notanumber 12abc\n"
        "fine{a=\"1\"} 5\n");
    REQUIRE(s.size() == 2);
    REQUIRE(s[0].name == "ok");
    REQUIRE(s[1].name == "fine");
}

static NwdafConfig oamConfig() {
    NwdafConfig cfg;
    cfg.nf_instance_id = "oam-test";
    cfg.nf_service_names = {{"AMF", "amfd"}};
    cfg.collection_interval_seconds = 1;
    cfg.throughput_history_size = 2;
    cfg.oam_metrics_endpoints = {{"AMF", "http://127.0.0.5:9090/metrics"},
                                 {"SMF", "http://127.0.0.4:9090/metrics"}};
    cfg.model_dir = "/tmp";
    cfg.log_level = "warn"; cfg.log_file = "/tmp/nwdaf_oam_test.log";
    return cfg;
}

static void runOnce(MockNwdafCollector& c) {
    c.startBackgroundCollection();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    c.stopBackgroundCollection();
}

TEST_CASE("H1.1: the collector scrapes each configured metrics endpoint") {
    MockNwdafCollector c(oamConfig());
    // Before the first scrape each endpoint is listed, not up.
    auto before = c.getOamSources();
    REQUIRE(before.size() == 2);
    REQUIRE_FALSE(before[0].up);

    c.setOamMetrics("http://127.0.0.5:9090/metrics", std::string(SLICE_LINES));
    // SMF unreachable (no body registered).
    runOnce(c);
    const auto sources = c.getOamSources();
    REQUIRE(sources.size() == 2);
    REQUIRE(sources[0].nf_type == "AMF");
    REQUIRE(sources[0].up);
    REQUIRE(sources[0].samples.size() == 2);
    REQUIRE(sources[0].last_success.time_since_epoch().count() != 0);
    REQUIRE(sources[1].nf_type == "SMF");
    REQUIRE_FALSE(sources[1].up);
    REQUIRE(c.getOamHistory("AMF").size() == 1);
    REQUIRE(c.getOamHistory("SMF").empty());
}

TEST_CASE("H1.1: a failed scrape keeps the last samples, marked down, out of the history") {
    MockNwdafCollector c(oamConfig());
    c.setOamMetrics("http://127.0.0.5:9090/metrics", std::string(SLICE_LINES));
    runOnce(c);
    c.setOamMetrics("http://127.0.0.5:9090/metrics", std::nullopt);
    runOnce(c);
    const auto amf = c.getOamSources()[0];
    REQUIRE_FALSE(amf.up);
    REQUIRE(amf.samples.size() == 2);   // last known
    REQUIRE(c.getOamHistory("AMF").size() == 1);

    // The history is bounded by throughput_history_size (2 here).
    c.setOamMetrics("http://127.0.0.5:9090/metrics", std::string(SLICE_LINES));
    for (int i = 0; i < 3; ++i) runOnce(c);
    REQUIRE(c.getOamHistory("AMF").size() == 2);
}

TEST_CASE("H1.1: the real scraper fetches a metrics endpoint over HTTP") {
    httplib::Server srv;
    srv.Get("/metrics", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(SLICE_LINES, "text/plain; version=0.0.4");
    });
    std::thread t([&] { srv.listen("127.0.0.1", 17791); });
    srv.wait_until_ready();

    NwdafConfig cfg = oamConfig();
    cfg.oam_metrics_endpoints = {{"AMF", "http://127.0.0.1:17791/metrics"},
                                 {"SMF", "http://127.0.0.1:17791/nothing-here"}};
    NwdafCollector c(cfg);
    const auto sources = c.collectOamMetrics();
    srv.stop();
    t.join();
    REQUIRE(sources.size() == 2);
    REQUIRE(sources[0].up);
    REQUIRE(sources[0].samples.size() == 2);
    REQUIRE_FALSE(sources[1].up);   // 404
}
