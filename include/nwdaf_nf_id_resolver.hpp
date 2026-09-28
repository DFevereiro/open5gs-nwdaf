#pragma once
#include "nwdaf_config.hpp"
#include "nwdaf_http_client.hpp"
#include <map>
#include <mutex>
#include <string>

// H1.9: NF instance IDs of the monitored NFs, as NfLoadLevelInformation
// (TS 29.520) requires. Configured IDs (nf_instance_ids) take precedence; with
// nrf_nf_discovery enabled, the remaining NF types are resolved through
// Nnrf_NFDiscovery (TS 29.510 V18.11.0, GET /nnrf-disc/v1/nf-instances).
//
// A discovered ID is used only when the NRF reports exactly one instance of
// that NF type: with several, which one is the locally measured process
// cannot be told, so the type stays unresolved rather than guessed.
class NwdafNfIdResolver {
public:
    explicit NwdafNfIdResolver(const NwdafConfig& config);

    // NF type → NF instance ID: configured, then discovered.
    std::map<std::string, std::string> ids() const;

    // Query the NRF for every monitored NF type without a configured ID.
    // A failed query keeps the previous result for that type.
    void refresh();

private:
    NwdafConfig     config_;
    NwdafHttpClient http_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> discovered_;
};
