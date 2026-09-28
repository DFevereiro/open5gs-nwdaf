#pragma once
#include "nwdaf_analytics.hpp"
#include "nwdaf_subscription.hpp"
#include "nwdaf_config.hpp"
#include "nwdaf_ratelimit.hpp"
#include "nwdaf_sbi.hpp"
#ifdef NWDAF_USE_TLS
#include "nwdaf_oauth.hpp"
#include <openssl/ssl.h>
#endif
#ifdef NWDAF_USE_HTTP2
#include "nwdaf_h2_server.hpp"
#endif
#include <httplib.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

// PROD-03: analytics values updated atomically after each compute() call.
// Protected by NwdafServer::metrics_mutex_.
struct NwdafMetricsCache {
    double anomaly_pct               = 0.0;
    double mos_score                 = 2.0;
    double network_performance_score = 0.0;
};

class NwdafServer {
public:
    NwdafServer(NwdafAnalyticsEngine& engine,
                NwdafSubscriptionStore& subs,
                const NwdafConfig& config,
                std::shared_ptr<NwdafNfIdResolver> resolver = nullptr);   // H1.9

    void start();
    void stop();

    // H1.8: "rel18-sbi" when the 3GPP interfaces are served over HTTP/2,
    // "dev-legacy" (explicitly transport non-compliant) otherwise.
    static const char* transportProfile(const NwdafConfig& config);

    // PROD-03: notification counters incremented by NwdafNotifier
    std::atomic<uint64_t> notif_total_{0};
    std::atomic<uint64_t> notif_failures_{0};

private:
    // ARCH-05: unique_ptr allows substituting SSLServer at runtime when
    // tls_enabled=true without changing the rest of the server code.
    std::unique_ptr<httplib::Server> svr_;

    void setupRoutes();

    void handleHealth(const httplib::Request&, httplib::Response&);
    void handleReady(const httplib::Request& req, httplib::Response& res);

    void handleGetAnalytics(const httplib::Request& req, httplib::Response& res);
    void handlePostAnalytics(const httplib::Request&, httplib::Response&);
    void handleCreateSubscription(const httplib::Request&, httplib::Response&);
    void handleGetSubscription(const httplib::Request&, httplib::Response&);
    void handleDeleteSubscription(const httplib::Request&, httplib::Response&);
    void handleListSubscriptions(const httplib::Request&, httplib::Response&);
    void handleTrainModel(const httplib::Request&, httplib::Response&);
    void handleMetrics(const httplib::Request&, httplib::Response&);  // PROD-03
    void handleOpenApi(const httplib::Request&, httplib::Response&);  // H1.6
    
    // Traffic simulator endpoints
    void handleTrafficStart(const httplib::Request&, httplib::Response&);
    void handleTrafficStop(const httplib::Request&, httplib::Response&);
    void handleTrafficStatus(const httplib::Request&, httplib::Response&);

    // H1.7: adapts an httplib request to the transport-agnostic 3GPP service.
    void handle3gpp(const httplib::Request&, httplib::Response&);
    // Rate limiting, the OAuth check and 3GPP dispatch, shared by the HTTP/1.1
    // and HTTP/2 listeners.
    SbiResponse serve3gpp(const SbiRequest& req, const std::string& remote_addr);

    NwdafAnalyticsEngine&   engine_;
    NwdafSubscriptionStore& subs_;
    NwdafConfig             config_;

    // H1.7: 3GPP Nnwdaf_AnalyticsInfo / Nnwdaf_EventsSubscription (TS 29.520)
    NwdafSbiService         sbi_;
#ifdef NWDAF_USE_TLS
    NwdafSchemaValidator    oauth_schema_;                       // H1.10: AccessTokenClaims
    std::unique_ptr<NwdafAccessTokenValidator> token_validator_;  // null = OAuth off
#endif
#ifdef NWDAF_USE_HTTP2
    std::unique_ptr<NwdafH2Server> h2_;   // H1.8: 3GPP interfaces over HTTP/2
#endif

    // PROD-06: per-IP + global token-bucket rate limiter
    RateLimiter rate_limiter_;

    // PROD-03: per-analytics-ID request counters + last-computed value cache
    mutable std::mutex              metrics_mutex_;
    std::map<std::string, uint64_t> analytics_request_counts_;
    NwdafMetricsCache               metrics_cache_;

    // Traffic simulator state
    std::mutex  traffic_mutex_;
    bool        traffic_running_{false};
    std::string current_traffic_task_{"none"};
};
