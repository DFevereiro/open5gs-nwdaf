#pragma once
#include "nwdaf_config.hpp"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class NwdafAnalyticsEngine;
class NwdafCollector;
class NwdafNfMonitor;
class NwdafNrfClient;

// PROD-04 / QOL-05: the configuration as SIGHUP reloads it.
//
// Components that read reloadable settings while serving take a snapshot per
// request or poll (get()), so one answer never mixes two configurations.
// Settings that need a restart keep their running values on reload.
class NwdafLiveConfig {
public:
    explicit NwdafLiveConfig(NwdafConfig config);

    std::shared_ptr<const NwdafConfig> get() const;
    void set(NwdafConfig config);

    // The settings a reload applies: logging and cadence, the analytics data
    // sources and capability settings, and the analytics windows. The rest
    // (ports, TLS, OAuth, the NRF and its polling, retention) need a restart.
    static const std::vector<std::string> RELOADABLE;

    // `running` with the reloadable settings taken from `fresh`; `changed`
    // gets the names of those that differ.
    static NwdafConfig merge(const NwdafConfig& running, const NwdafConfig& fresh,
                             std::vector<std::string>& changed);

private:
    mutable std::mutex m_;
    std::shared_ptr<const NwdafConfig> config_;
};

// One reload: what changed, and whether the NRF profile was updated.
struct NwdafReloadResult {
    std::vector<std::string> changed;
    bool profile_changed = false;   // the NF profile (advertisement, TAIs, features) differs
    bool nrf_updated = false;       // NFUpdate (complete replacement) succeeded
};

// Apply a freshly loaded configuration to the running components. `nrf` is
// null when the NWDAF doesn't register; with it, a changed profile is sent as
// NFUpdate (TS 29.510 V18.11.0 §5.2.2.3.1A).
NwdafReloadResult nwdafApplyReload(const NwdafConfig& fresh, NwdafLiveConfig& live,
                                   NwdafCollector& collector, NwdafAnalyticsEngine& engine,
                                   NwdafNfMonitor& nf_monitor, NwdafNrfClient* nrf);
