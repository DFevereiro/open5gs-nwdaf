#pragma once
#include "nwdaf_collector.hpp"
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// H1.4: NETWORK_PERFORMANCE statistics (TS 23.288 V18.13.0 §6.6) from the
// OAM measurements Open5GS exposes (TS 28.552), per interpretation I-11.
//
// - NUM_OF_UE, "average number of UEs observed in the area": the mean over the
//   period of RM.RegisteredSubNbrMean, which Open5GS exposes only per S-NSSAI
//   (fivegs_amffunction_rm_registeredsubnbr{plmnid,snssai}), so the per-slice
//   series of this PLMN are summed.
// - SESS_SUCC_RATIO, "average ratio of successful setup of PDU Sessions":
//   successful ÷ requested creations over the period. TS 28.552 counts
//   SM.PduSessionCreationReq on receipt of Nsmf_PDUSession_CreateSMContext
//   Request, and Succ and Fail on transmission of its response, so
//   Succ = Req − Fail. Open5GS counts every request once without labels
//   (fivegs_smffunction_sm_pdusessioncreationreq{plmnid="",snssai=""}) and
//   failures per cause; its Succ counter is also incremented after the UDM
//   registration, counting each session twice, so it is not used.
class NwdafNetworkPerformanceCalculator {
public:
    // Average of the summed per-slice registered-UE series of `plmn_id` over
    // the scrapes in [from, to]; nullopt without a scrape in the period.
    static std::optional<double> numOfUe(const std::vector<NwdafOamScrape>& amf, const std::string& plmn_id,
                                         std::chrono::system_clock::time_point from,
                                         std::chrono::system_clock::time_point to);

    // Successful PDU session creations as a percentage of requests over
    // [from, to]; nullopt when no request was made in the period or fewer
    // than two scrapes cover it.
    static std::optional<double> sessSuccRatio(const std::vector<NwdafOamScrape>& smf,
                                               std::chrono::system_clock::time_point from,
                                               std::chrono::system_clock::time_point to);

    // The increase of a cumulative counter (sum of the matching series) over
    // the scrapes in [from, to]. A drop is a counter reset (the NF restarted):
    // the value after it counts from zero. nullopt with fewer than two scrapes.
    template <typename Match>
    static std::optional<std::uint64_t> increase(const std::vector<NwdafOamScrape>& scrapes,
                                                 std::chrono::system_clock::time_point from,
                                                 std::chrono::system_clock::time_point to, Match match) {
        std::optional<double> prev;
        double total = 0.0;
        size_t n = 0;
        for (const auto& s : scrapes) {
            if (s.at < from || s.at > to) continue;
            double v = 0.0;
            for (const auto& p : s.samples)
                if (match(p)) v += p.value;
            if (prev) total += v >= *prev ? v - *prev : v;
            prev = v;
            ++n;
        }
        if (n < 2) return std::nullopt;
        return static_cast<std::uint64_t>(total);
    }
};
