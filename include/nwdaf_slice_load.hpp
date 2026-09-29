#pragma once
#include "nwdaf_collector.hpp"
#include "nwdaf_config.hpp"
#include <chrono>
#include <optional>
#include <string>
#include <vector>

// H1.2: average and variance of a count over the samples of a period.
struct NwdafNumberStat {
    double average  = 0.0;
    double variance = 0.0;   // population variance
    size_t samples  = 0;
};

// H1.2: the load of one configured network slice over a period (I-9).
struct NwdafSliceLoad {
    NwdafSliceCapacity              slice;
    std::optional<NwdafNumberStat>  ues;            // when max_ues is configured
    std::optional<NwdafNumberStat>  pdu_sessions;   // when max_pdu_sessions is configured
    int                             load_level = 0; // 0–100
};

// H1.2: slice load level from the per-slice OAM counts Open5GS exposes
// (TS 23.288 V18.13.0 Table 6.3.2A-1: UEs registered and PDU sessions
// established per S-NSSAI, TS 28.552).
//
// I-9: TS 29.520 fixes loadLevelInformation to 0–100 but leaves its meaning to
// the implementation. This NWDAF follows Network Slice Admission Control:
// an NSACF is configured with the maximum number of UEs and of PDU sessions
// per S-NSSAI (TS 23.501 §5.15.11.0) and reports occupancy as a percentage of
// them (TS 29.571 SACInfo percValueNumUes / percValueNumPduSess). The load
// level is the higher of the two percentages over the period (average count ÷
// configured maximum × 100, rounded, clamped to 0–100), counting only the
// dimensions configured for the slice.
class NwdafSliceLoadCalculator {
public:
    // Samples: AMF fivegs_amffunction_rm_registeredsubnbr and SMF
    // fivegs_smffunction_sm_sessionnbr with labels plmnid = `plmn_id`
    // ("99970") and snssai = slice.key(). A successful scrape without the
    // series counts as 0 (Open5GS creates it on first use).
    //
    // nullopt when a configured dimension has no scrape in [from, to].
    static std::optional<NwdafSliceLoad> compute(const NwdafSliceCapacity& slice,
                                                 const std::string& plmn_id,
                                                 const std::vector<NwdafOamScrape>& amf,
                                                 const std::vector<NwdafOamScrape>& smf,
                                                 std::chrono::system_clock::time_point from,
                                                 std::chrono::system_clock::time_point to);

    // The PLMN ID as Open5GS labels it: MCC followed by the MNC.
    static std::string plmnLabel(const NwdafConfig& cfg) { return cfg.plmn_mcc + cfg.plmn_mnc; }
};
