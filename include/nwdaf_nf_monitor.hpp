#pragma once
#include "nwdaf_3gpp_adapter.hpp"
#include "nwdaf_config.hpp"
#include "nwdaf_http_client.hpp"
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// H1.9: what the NRF knows about the monitored NFs — their instance
// IDs (NfLoadLevelInformation.nfInstanceId is mandatory) and their NRF status
// over time (NfLoadLevelInformation.nfStatus). TS 23.288 V18.13.0 §6.5.2
// Table 6.5.2-1 names the NRF as the source of both.
//
// Configured IDs (nf_instance_ids) take precedence. With nrf_nf_discovery the
// monitor polls the NRF with NFManagement NFListRetrieval and
// NFProfileRetrieval (TS 29.510 V18.11.0 §5.2.2.6, §5.2.2.7). It does not use
// NFDiscover or NFStatusSubscribe: the NRF filters both by the target
// profile's allowedNfTypes, and Open5GS v2.8.0 NFs never allow NWDAF, so an
// NWDAF requester sees no Open5GS NF through them (compliance doc,
// interoperability records).
//
// An ID is resolved only when the NRF lists exactly one instance of the type:
// with several, which one is the locally measured process cannot be told, so
// the type stays unresolved rather than guessed.
class NwdafNfMonitor {
public:
    explicit NwdafNfMonitor(const NwdafConfig& config);
    virtual ~NwdafNfMonitor() = default;

    // NF type → NF instance ID: configured, then resolved from the NRF.
    std::map<std::string, std::string> ids() const;
    // QOL-05: nf_instance_ids as reloaded on SIGHUP.
    void setConfiguredIds(const std::map<std::string, std::string>& ids);

    // NfStatus of every instance the NRF listed within the status window
    // (I-8): the share of polls that found it in each state. Empty until a
    // poll has succeeded.
    std::vector<NwdafNfStatusObservation> statuses() const;

    // Poll the NRF once for every monitored NF type. A failed query keeps the
    // previous ID and records no status sample for that type.
    void refresh();

protected:
    // Test seam for the time base of the status window.
    virtual std::chrono::system_clock::time_point now() const;

private:
    // One successful NFListRetrieval of a type: instance ID → NFStatus.
    struct Poll {
        std::chrono::system_clock::time_point at;
        std::map<std::string, std::string> status;
    };

    NwdafConfig     config_;
    NwdafHttpClient http_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> configured_;     // nf_instance_ids (QOL-05: reloadable)
    std::map<std::string, std::string> resolved_;       // NF type → ID
    std::map<std::string, std::deque<Poll>> polls_;     // NF type → polls in the window
};
