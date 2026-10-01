#pragma once
#include "nwdaf_config.hpp"
#include "nwdaf_prometheus.hpp"
#include "nwdaf_ue_location.hpp"
#include "ml/ewma_predictor.hpp"
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <cstdint>
#include <utility>
#include <chrono>
#include <functional>
#include <unordered_set>
#include <optional>

#ifdef NWDAF_HAS_MONGODB
#include <mongocxx/client.hpp>
#endif

#ifdef NWDAF_HAS_SQLITE
// Forward-declare sqlite3 to avoid including <sqlite3.h> in this header.
struct sqlite3;
#endif

struct AmfEvent {
    std::string event_type;
    std::string supi;
    std::string raw_line;
    std::string timestamp_iso;
};

struct SmfEvent {
    std::string event_type;
    std::string supi;
    // ARCH-04: opaque session key (SUPI or SUPI+DNN) used by the stateful tracker
    std::string session_id;
    std::string raw_line;
    std::string timestamp_iso;
};

struct ThroughputSample {
    std::string timestamp_iso;
    double      total_dl_bps;
    double      total_ul_bps;
    double      total_dl_kbps;
    double      total_ul_kbps;
    std::map<std::string, std::pair<double,double>> per_iface;
};

struct NfMetric {
    std::string nf_type;
    std::string status;
    int         pid;
    double      cpu_seconds;
    long        mem_kb;
    double      load_pct;
    std::string load_label;
};

// H1.7: the NF loads of one collection tick, kept for predictions (I-13).
struct NwdafNfLoadScrape {
    std::chrono::system_clock::time_point at;
    std::vector<NfMetric> metrics;
};

// H1.1: one NF's Prometheus metrics endpoint (OAM input).
struct NwdafOamSource {
    std::string nf_type;
    std::string endpoint;
    bool        up = false;   // the last scrape succeeded
    std::chrono::system_clock::time_point last_success{};   // epoch = never
    std::vector<NwdafPromSample> samples;                    // of the last success
    size_t count = 0;   // samples, or for the AMF /ue-info the UEs listed
};

// H1.1: one successful scrape, kept for statistics over a time window.
struct NwdafOamScrape {
    std::chrono::system_clock::time_point at;
    std::vector<NwdafPromSample> samples;
};

class NwdafCollector {
public:
    explicit NwdafCollector(const NwdafConfig& config);
    ~NwdafCollector();

    std::vector<AmfEvent>      collectAmfEvents();
    std::vector<SmfEvent>      collectSmfEvents();
    ThroughputSample           collectUPFThroughput();
    // Virtual so tests can inject NF metrics: the real implementation shells
    // out to systemctl, which is unavailable in a test environment.
    virtual std::vector<NfMetric> collectNfLoad();
    int                        getSubscriberCount();
    // BUG-06: the subscriber count in mongosh output — the last line that is
    // a plain number (0 when there is none).
    static int parseCountOutput(const std::string& output);
    // H1.1: scrape every configured oam_metrics_endpoints entry once.
    std::vector<NwdafOamSource> collectOamMetrics();
    // H1.1: poll amf_ue_info_endpoint (every page) and feed the UE location
    // tracker; false when it is not configured or a page failed (I-12).
    bool collectUeLocations();

    void startBackgroundCollection();
    void stopBackgroundCollection();

    // PROD-04: hot-reloadable settings (safe to call from SIGHUP handler)
    void updateConfig(int collection_interval_seconds, double ewma_alpha);

    std::vector<AmfEvent>         getRecentAmfEvents(int n = 100) const;
    std::vector<SmfEvent>         getRecentSmfEvents(int n = 100) const;
    std::vector<ThroughputSample> getThroughputHistory(int n = 60) const;
    std::vector<NfMetric>         getCachedNfMetrics() const;
    // H1.7: the NF loads of the last ticks (at most throughput_history_size).
    std::vector<NwdafNfLoadScrape> getNfLoadHistory() const;
    // H1.1: the configured metrics endpoints as last scraped by bgLoop, and
    // the successful scrapes of one NF type (at most throughput_history_size).
    std::vector<NwdafOamSource>   getOamSources() const;
    std::vector<NwdafOamScrape>   getOamHistory(const std::string& nf_type) const;
    // H1.1: UE trajectories from the AMF /ue-info (I-12); thread-safe.
    std::shared_ptr<const NwdafUeLocationTracker> ueLocations() const { return ue_locations_; }

    // BUG-02: EWMA predictions updated by bgLoop, read-only for analytics
    double getDlEwmaPrediction() const;
    double getUlEwmaPrediction() const;

    // ARCH-04: stateful PDU session count (survives log rotation / service restart)
    int getActivePduSessionCount() const;

protected:
    virtual std::vector<std::string> readJournalLines(const std::string& unit, int n);
    virtual std::pair<long,long>     readProcStat(int pid);
    virtual long                     readProcMemKb(int pid);
    virtual std::pair<uint64_t,uint64_t> readNetStats(const std::string& iface);
    virtual int                      querySubscriberCountFromMongo();
    // H1.1: body of an HTTP GET of a metrics endpoint; nullopt on failure.
    virtual std::optional<std::string> readOamMetrics(const std::string& url);

    // BUG-01: injectable clock for testability
    virtual std::chrono::steady_clock::time_point getCpuNow() const;

    // H1.4 test seam: append a throughput sample directly to the history ring,
    // as bgLoop would. Analytics that need a multi-sample window (DISPERSION,
    // RED_TRANS_EXP) are otherwise only reachable by running the
    // background loop for real time. Protected so no production API changes;
    // MockNwdafCollector re-exposes it for tests.
    void appendThroughputSample(const ThroughputSample& s);

    // BUG-01: rate-based CPU utilisation (callable by subclass test wrappers)
    double computeCpuPct(int pid);

    NwdafConfig config_;

private:
    mutable std::mutex           mutex_;
    std::deque<AmfEvent>         amf_events_;
    std::deque<SmfEvent>         smf_events_;
    std::deque<ThroughputSample> throughput_history_;
    std::vector<NfMetric>        nf_metrics_;
    std::deque<NwdafNfLoadScrape> nf_history_;   // H1.7
    std::map<std::string, NwdafOamSource>             oam_sources_;   // H1.1
    std::map<std::string, std::deque<NwdafOamScrape>> oam_history_;   // H1.1
    std::shared_ptr<NwdafUeLocationTracker> ue_locations_;            // H1.1, own lock
    NwdafOamSource                          ue_info_source_;          // H1.1, under mutex_

    // BUG-01: per-PID CPU snapshot for delta computation
    struct CpuSnapshot {
        long ticks;
        std::chrono::steady_clock::time_point ts;
    };
    std::map<int, CpuSnapshot> cpu_snapshots_;

    // BUG-02: EWMA predictors updated once per collection interval
    EwmaPredictor dl_ewma_;
    EwmaPredictor ul_ewma_;

    // ARCH-04: stateful PDU session tracker — persists across log rotation.
    // Keyed by session_id (= SUPI, or SUPI+DNN when extractable).
    std::unordered_set<std::string> active_sessions_;

    // BUG-07: journal lines read on the previous tick, per NF.
    std::unordered_set<std::string> seen_amf_lines_;
    std::unordered_set<std::string> seen_smf_lines_;
    template <typename Event>
    static std::vector<Event> freshEvents(std::vector<Event> events,
                                          std::unordered_set<std::string>& seen);

    // PROD-08: pre-snapshot for non-blocking throughput measurement
    struct NetSnapshot {
        std::map<std::string, std::pair<uint64_t,uint64_t>> readings;
        std::chrono::steady_clock::time_point ts;
    };
    NetSnapshot prev_net_snapshot_;
    bool        has_net_snapshot_ = false;

    std::thread        bg_thread_;
    std::atomic<bool>  running_{false};
    void               bgLoop();

    void initMongo();

#ifdef NWDAF_HAS_MONGODB
    std::unique_ptr<mongocxx::client> mongo_client_;
#endif

    // PROD-01: optional SQLite-backed throughput history persistence
#ifdef NWDAF_HAS_SQLITE
    sqlite3* history_db_ = nullptr;
    void initHistoryDb();
    void persistThroughputSample(const ThroughputSample& s);
    void warmFromHistoryDb();
#else
    void warmFromHistoryDb() {}  // no-op when SQLite not compiled in
#endif
};
