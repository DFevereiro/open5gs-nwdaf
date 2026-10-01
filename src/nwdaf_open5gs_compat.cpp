#include "nwdaf_open5gs_compat.hpp"
#include <algorithm>
#include <spdlog/spdlog.h>

const std::vector<std::string> NwdafOpen5gsCompat::VERIFIED_VERSIONS = {"2.8.0"};

const std::vector<NwdafOpen5gsQuirk>& NwdafOpen5gsCompat::quirks() {
    static const std::vector<NwdafOpen5gsQuirk> list = {
        {"O5GS-01", "NFListRetrieval nests totalItemCount inside _links",
                    "the NF list is read from _links.item only", "2026-09-29"},
        {"O5GS-02", "no NF lists NWDAF in allowedNfTypes, so NFDiscover and NFStatusSubscribe return nothing",
                    "NF instance IDs and NF status come from NFListRetrieval and NFProfileRetrieval", "2026-09-29"},
        {"O5GS-03", "the NRF discards the load NFs send in heartbeats",
                    "NF_LOAD reports the CPU measured from /proc (I-5)", "2026-09-28"},
        {"O5GS-04", "the SMF counts fivegs_smffunction_sm_pdusessioncreationsucc twice per session",
                    "SESS_SUCC_RATIO is (Req - Fail) / Req from the unlabelled request series (I-11)", "2026-09-29"},
        {"O5GS-05", "the AMF's registered-UE gauge exists only per subscribed S-NSSAI",
                    "the per-slice series are summed for NUM_OF_UE, and used as is for slice load (A.6)", "2026-09-29"},
        {"O5GS-06", "the UPF's N3 packet and per-QFI volume counters are compiled out",
                    "throughput is read from /sys/class/net", "2026-09-30"},
        {"O5GS-07", "the AMF lists UEs only as non-standard JSON (/ue-info), timestamp 0 before a location",
                    "UE locations are polled from /ue-info, every page, skipping timestamp 0 (I-12)", "2026-09-30"},
        {"O5GS-08", "no NF event-exposure services and no OAuth 2.0",
                    "inputs come from implementation-specific OAM sources; oauth_enabled stays off", "2026-09-28"},
    };
    return list;
}

bool NwdafOpen5gsCompat::verified(const std::string& version) {
    return std::find(VERIFIED_VERSIONS.begin(), VERIFIED_VERSIONS.end(), version) != VERIFIED_VERSIONS.end();
}

void NwdafOpen5gsCompat::announce(const NwdafConfig& cfg) {
    std::string ids;
    for (const auto& q : quirks()) ids += (ids.empty() ? "" : ", ") + std::string(q.id);
    if (verified(cfg.open5gs_version)) {
        spdlog::info("Open5GS {}: verified; handling {}", cfg.open5gs_version, ids);
        return;
    }
    std::string known;
    for (const auto& v : VERIFIED_VERSIONS) known += (known.empty() ? "" : ", ") + v;
    spdlog::warn("Open5GS {} is not a verified version ({}): the workarounds {} may no longer match it; "
                 "check them with demo/interop-check.sh and see the interoperability records",
                 cfg.open5gs_version, known, ids);
}
