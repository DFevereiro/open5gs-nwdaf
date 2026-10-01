#include "nwdaf_live_config.hpp"
#include "nwdaf_analytics.hpp"
#include "nwdaf_analytics_catalogue.hpp"
#include "nwdaf_collector.hpp"
#include "nwdaf_nf_monitor.hpp"
#include "nwdaf_nrf_client.hpp"
#include <spdlog/spdlog.h>

NwdafLiveConfig::NwdafLiveConfig(NwdafConfig config)
    : config_(std::make_shared<const NwdafConfig>(std::move(config))) {}

std::shared_ptr<const NwdafConfig> NwdafLiveConfig::get() const {
    std::lock_guard<std::mutex> lk(m_);
    return config_;
}

void NwdafLiveConfig::set(NwdafConfig config) {
    auto next = std::make_shared<const NwdafConfig>(std::move(config));
    std::lock_guard<std::mutex> lk(m_);
    config_ = std::move(next);
}

const std::vector<std::string> NwdafLiveConfig::RELOADABLE = {
    "log_level", "collection_interval_seconds", "ewma_alpha", "anomaly_contamination",
    "oam_metrics_endpoints", "amf_ue_info_endpoint", "slice_capacity", "served_tai_list", "nf_instance_ids",
    "slice_load_window_seconds", "network_performance_window_seconds", "ue_mobility_window_seconds",
    "prediction_horizon_seconds", "prediction_min_samples", "prediction_tolerance",
};

NwdafConfig NwdafLiveConfig::merge(const NwdafConfig& running, const NwdafConfig& fresh,
                                   std::vector<std::string>& changed) {
    NwdafConfig out = running;
#define NWDAF_RELOAD(field)                                   \
    if (!(out.field == fresh.field)) {                        \
        out.field = fresh.field;                              \
        changed.push_back(#field);                            \
    }
    NWDAF_RELOAD(log_level)
    NWDAF_RELOAD(collection_interval_seconds)
    NWDAF_RELOAD(ewma_alpha)
    NWDAF_RELOAD(anomaly_contamination)
    NWDAF_RELOAD(oam_metrics_endpoints)
    NWDAF_RELOAD(amf_ue_info_endpoint)
    NWDAF_RELOAD(slice_capacity)
    NWDAF_RELOAD(served_tai_list)
    NWDAF_RELOAD(nf_instance_ids)
    NWDAF_RELOAD(slice_load_window_seconds)
    NWDAF_RELOAD(network_performance_window_seconds)
    NWDAF_RELOAD(ue_mobility_window_seconds)
    NWDAF_RELOAD(prediction_horizon_seconds)
    NWDAF_RELOAD(prediction_min_samples)
    NWDAF_RELOAD(prediction_tolerance)
#undef NWDAF_RELOAD
    return out;
}

namespace {

std::string joined(const std::set<std::string>& ids) {
    std::string out;
    for (const auto& id : ids) out += (out.empty() ? "" : ",") + id;
    return out.empty() ? "none" : out;
}

spdlog::level::level_enum logLevel(const std::string& name) {
    if (name == "trace") return spdlog::level::trace;
    if (name == "debug") return spdlog::level::debug;
    if (name == "warn")  return spdlog::level::warn;
    if (name == "error") return spdlog::level::err;
    return spdlog::level::info;
}

}  // namespace

NwdafReloadResult nwdafApplyReload(const NwdafConfig& fresh, NwdafLiveConfig& live,
                                   NwdafCollector& collector, NwdafAnalyticsEngine& engine,
                                   NwdafNfMonitor& nf_monitor, NwdafNrfClient* nrf) {
    NwdafReloadResult r;
    const auto before = live.get();
    const NwdafConfig merged = NwdafLiveConfig::merge(*before, fresh, r.changed);
    if (r.changed.empty()) {
        spdlog::info("Config reload: no reloadable setting changed");
        return r;
    }
    std::string names;
    for (const auto& c : r.changed) names += (names.empty() ? "" : ", ") + c;
    spdlog::info("Config reload: {}", names);

    spdlog::default_logger()->set_level(logLevel(merged.log_level));
    collector.updateConfig(merged.collection_interval_seconds, merged.ewma_alpha);
    collector.updateSources(merged.oam_metrics_endpoints, merged.amf_ue_info_endpoint);
    engine.updateConfig(merged.anomaly_contamination);
    nf_monitor.setConfiguredIds(merged.nf_instance_ids);

    const nlohmann::json old_profile = nrf ? nrf->profile() : nlohmann::json();
    const auto old_adv = NwdafAnalyticsCatalogue::rel18Advertised(*before);
    live.set(merged);   // the SBI, notifier and NRF client read from here on
    const auto new_adv = NwdafAnalyticsCatalogue::rel18Advertised(merged);
    if (old_adv != new_adv)
        spdlog::info("Config reload: advertised Rel-18 analytics {} -> {}", joined(old_adv), joined(new_adv));

    // The NF profile reflects capability only (advertised analytics, served
    // TAIs, feature bitmasks): when it changes, the NRF is told at once.
    if (nrf) {
        r.profile_changed = nrf->profile() != old_profile;
        if (r.profile_changed) r.nrf_updated = nrf->updateProfile();
    } else {
        r.profile_changed = old_adv != new_adv || !(before->served_tai_list == merged.served_tai_list);
    }
    return r;
}
