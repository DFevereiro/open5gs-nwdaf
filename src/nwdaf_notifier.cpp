#include "nwdaf_notifier.hpp"
#include "nwdaf_http_client.hpp"
#include "nwdaf_sbi.hpp"
#include <spdlog/spdlog.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <optional>
#include <set>

using json = nlohmann::json;

// Split "http://host:port/path/..." → {"http://host:port", "/path/..."}
static std::pair<std::string, std::string> splitUrl(const std::string& url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return {url, "/"};
    auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) return {url, "/"};
    return {url.substr(0, path_start), url.substr(path_start)};
}

static std::string nowISO() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    gmtime_r(&t, &tm_buf);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

NwdafNotifier::NwdafNotifier(NwdafSubscriptionStore& subs,
                              NwdafAnalyticsEngine&   engine,
                              int poll_interval_seconds,
                              std::atomic<uint64_t>*  notif_total,
                              std::atomic<uint64_t>*  notif_failures,
                              const NwdafConfig&      config,
                              std::shared_ptr<NwdafNfMonitor> nf_monitor,
                              std::shared_ptr<NwdafLiveConfig> live)
    : config_(config),
      live_(live ? std::move(live) : std::make_shared<NwdafLiveConfig>(config)),
      nf_monitor_(nf_monitor ? std::move(nf_monitor) : std::make_shared<NwdafNfMonitor>(config)),
      subs_(subs), engine_(engine), poll_interval_s_(poll_interval_seconds),
      notif_total_(notif_total), notif_failures_(notif_failures)
{}

void NwdafNotifier::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&NwdafNotifier::deliveryLoop, this);
    spdlog::info("NwdafNotifier started (poll interval {}s)", poll_interval_s_);
}

void NwdafNotifier::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    spdlog::info("NwdafNotifier stopped");
}

void NwdafNotifier::deliveryLoop() {
    while (running_) {
        // Sleep in 1-second increments for clean shutdown
        for (int i = 0; i < poll_interval_s_ && running_; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_) break;

        auto subs = subs_.listAll();
        auto now  = std::chrono::steady_clock::now();

        // Drop the threshold baselines of subscriptions that no longer exist.
        {
            std::set<std::string> live;
            for (const auto& s : subs) live.insert(s.sub_id);
            std::lock_guard<std::mutex> lk(ts_mutex_);
            for (auto it = threshold_state_.begin(); it != threshold_state_.end();)
                it = live.count(it->first.substr(0, it->first.find('#'))) ? std::next(it)
                                                                         : threshold_state_.erase(it);
        }

        // Rel-18 reports read the same measurements: gather them once per poll.
        std::optional<NwdafReportInputs> rel18_inputs;
        for (const auto& sub : subs) {
            if (sub.status != "ACTIVE") continue;
            if (sub.notif_uri.empty())  continue;
            if (sub.kind == "rel18") {
                if (!rel18_inputs) rel18_inputs = NwdafSbiService::gatherInputs(engine_, *nf_monitor_);
                deliverRel18(sub, *rel18_inputs);
                continue;
            }

            std::chrono::steady_clock::time_point last;
            {
                std::lock_guard<std::mutex> lk(ts_mutex_);
                auto it = last_delivered_.find(sub.sub_id);
                if (it != last_delivered_.end()) last = it->second;
            }

            auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
                now - last).count();
            if (last != std::chrono::steady_clock::time_point{} &&
                elapsed_s < sub.rep_period_seconds)
                continue;  // not due yet

            deliver(sub);
        }
    }
}

void NwdafNotifier::deliver(const Subscription& sub) {
    json anal_data;
    try {
        anal_data = engine_.compute(sub.analytics_id);
    } catch (const std::exception& e) {
        spdlog::warn("Notifier: compute failed for sub {} ({}): {}",
                     sub.sub_id, sub.analytics_id, e.what());
        return;
    }

    json body = {
        {"subId",       sub.sub_id},
        {"notifId",     sub.notif_id.empty() ? sub.sub_id : sub.notif_id},
        {"analyticsId", sub.analytics_id},
        {"ts",          nowISO()},
        {"analData",    anal_data}
    };

    auto [base, path] = splitUrl(sub.notif_uri);
    try {
        httplib::Client cli(base);
        cli.set_connection_timeout(3);
        cli.set_read_timeout(5);
        auto res = cli.Post(path, body.dump(), "application/json");

        if (res && res->status >= 200 && res->status < 300) {
            // PROD-03: increment shared notification counter
            if (notif_total_) ++(*notif_total_);

            // Update last-delivered timestamp
            {
                std::lock_guard<std::mutex> lk(ts_mutex_);
                last_delivered_[sub.sub_id] = std::chrono::steady_clock::now();
            }

            int new_count = subs_.incrementReportCount(sub.sub_id);
            spdlog::debug("Notifier: delivered {} to {} (count={})",
                          sub.analytics_id, sub.notif_uri, new_count);

            // Auto-delete when max_report_nbr reached
            if (sub.max_report_nbr > 0 && new_count >= sub.max_report_nbr) {
                subs_.remove(sub.sub_id);
                spdlog::info("Notifier: auto-deleted sub {} after {} reports",
                             sub.sub_id, new_count);
            }
        } else {
            // PROD-03: increment shared failure counter
            if (notif_failures_) ++(*notif_failures_);
            spdlog::warn("Notifier: delivery failed for sub {} → {} (HTTP {})",
                         sub.sub_id, sub.notif_uri,
                         res ? std::to_string(res->status) : "no response");
        }
    } catch (const std::exception& e) {
        if (notif_failures_) ++(*notif_failures_);
        spdlog::warn("Notifier: delivery exception for sub {}: {}", sub.sub_id, e.what());
    }
}

void NwdafNotifier::deliverRel18(const Subscription& sub, const NwdafReportInputs& in) {
    const auto cfg = live_->get();   // QOL-05: one configuration per delivery
    json rep;
    try { rep = json::parse(sub.rel18_json); }
    catch (const json::parse_error&) {
        spdlog::warn("Notifier: sub {} has an unreadable Rel-18 representation", sub.sub_id);
        return;
    }
    const json evt_req = rep.value("evtReq", json::object());

    // monDur (ReportingInformation): the subscription ends when it elapses.
    if (evt_req.contains("monDur")) {
        auto end = NwdafSbiService::parseDateTime(evt_req["monDur"].get<std::string>());
        if (end && std::chrono::system_clock::now() >= *end) {
            subs_.remove(sub.sub_id);
            spdlog::info("Notifier: Rel-18 sub {} ended (monDur elapsed)", sub.sub_id);
            return;
        }
    }

    // Effective method and period: evtReq supersedes the event's own
    // (TS 29.520 V18.14.0 §4.2.2.2.2 NOTE 1). THRESHOLD / ON_EVENT_DETECTION
    // events are evaluated on every poll (I-10).
    const bool one_time = evt_req.value("notifMethod", std::string()) == "ONE_TIME";
    const auto now = std::chrono::steady_clock::now();

    json reports = json::array();
    std::vector<std::string> due_keys;
    // Threshold baselines to keep once the report is delivered: a failed
    // delivery leaves the old baseline, so the crossing is reported again.
    std::vector<std::pair<std::string, NwdafSbiService::ThresholdState>> pending;
    const auto& events = rep["eventSubscriptions"];
    for (size_t i = 0; i < events.size(); ++i) {
        const json& es = events[i];
        const std::string tkey = sub.sub_id + "#" + std::to_string(i);
        if (NwdafSbiService::thresholdMode(evt_req, es)) {
            NwdafSbiService::ThresholdState next;
            {
                std::lock_guard<std::mutex> lk(ts_mutex_);
                next = threshold_state_[tkey];
            }
            auto rs = NwdafSbiService::thresholdReports(es, in, *cfg, next);
            if (rs.empty()) {
                std::lock_guard<std::mutex> lk(ts_mutex_);
                threshold_state_[tkey] = std::move(next);   // new baselines, nothing to report
                continue;
            }
            for (auto& r : rs) reports.push_back(std::move(r));
            pending.emplace_back(tkey, std::move(next));
            continue;
        }
        const int period = evt_req.contains("notifMethod")
            ? evt_req.value("repPeriod", 0) : es.value("repetitionPeriod", 0);
        const std::string key = sub.sub_id + "#" + std::to_string(i);
        {
            std::lock_guard<std::mutex> lk(ts_mutex_);
            auto it = last_delivered_.find(key);
            if (it != last_delivered_.end() &&
                (one_time || now - it->second < std::chrono::seconds(period)))
                continue;   // not due
        }
        // No data now → nothing to report for this event (failNotifyCode
        // UNAVAILABLE_DATA needs StatisticsFailure, which is not supported).
        auto rs = NwdafSbiService::eventReports(es, in, *cfg);
        if (!rs.empty()) {
            for (auto& r : rs) reports.push_back(std::move(r));
            due_keys.push_back(key);
        }
    }
    if (reports.empty()) return;

    const json body = {{"subscriptionId", sub.sub_id}, {"eventNotifications", reports}};
    // H1.8: over HTTP/2 to 3GPP consumers (TS 29.500 §5.2); TS 29.500
    // §5.2.3.2.3: the callback type is the notify service operation.
    const auto res = NwdafHttpClient(config_).request(
        "POST", sub.notif_uri, body.dump(), "application/json",
        {{"3gpp-Sbi-Callback", "Nnwdaf_EventsSubscription_Notify"}});
    if (!res || res.status < 200 || res.status >= 300) {
        if (notif_failures_) ++(*notif_failures_);
        spdlog::warn("Notifier: Rel-18 delivery failed for sub {} → {} ({})",
                     sub.sub_id, sub.notif_uri,
                     res ? "HTTP " + std::to_string(res.status) : res.error);
        return;
    }

    if (notif_total_) ++(*notif_total_);
    {
        std::lock_guard<std::mutex> lk(ts_mutex_);
        for (const auto& k : due_keys) last_delivered_[k] = now;
        for (auto& [k, st] : pending) threshold_state_[k] = std::move(st);
    }
    const int count = subs_.incrementReportCount(sub.sub_id);
    const int max_reports = evt_req.value("maxReportNbr", 0);
    if (one_time || (max_reports > 0 && count >= max_reports)) {
        subs_.remove(sub.sub_id);
        spdlog::info("Notifier: Rel-18 sub {} ended after {} report(s)", sub.sub_id, count);
    }
}

