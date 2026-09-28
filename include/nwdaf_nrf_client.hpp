#pragma once
#include "nwdaf_config.hpp"
#include "nwdaf_http_client.hpp"
#include <nlohmann/json.hpp>
#include <string>

// H1.9: NRF NFManagement client (TS 29.510 V18.11.0) — NFRegister,
// NFUpdate (heartbeat) and NFDeregister over NwdafHttpClient (HTTP/2).
//
// The registered profile is truthful: it is built from code and configuration
// only (never transient data availability) and carries only what the NWDAF
// actually supports — both Nnwdaf services at their pinned API versions, the
// HTTP/2 endpoint, the local supported-feature bitmasks, and an nwdafInfo
// listing the advertised analytics. Capability-affecting settings are
// restart-only; a restart re-registers, and NFRegister (PUT) replaces the
// profile the NRF holds.
class NwdafNrfClient {
public:
    explicit NwdafNrfClient(const NwdafConfig& config);

    // The NFProfile this NWDAF registers.
    nlohmann::json profile() const;

    // NFRegister (PUT). On success returns true and adopts the heartbeat
    // interval the NRF assigns (heartbeatSeconds()).
    bool registerNf();

    enum class Heartbeat { Ok, ReRegistered, Failed };
    // NFUpdate heartbeat (PATCH nfStatus). A 404 means the NRF no longer holds
    // the profile, so the NWDAF registers again at once.
    Heartbeat heartbeat();

    // NFDeregister (DELETE), on shutdown.
    bool deregister();

    int heartbeatSeconds() const { return heartbeat_s_; }

private:
    std::string instanceUrl() const;

    NwdafConfig     config_;
    NwdafHttpClient http_;
    int             heartbeat_s_;
};
