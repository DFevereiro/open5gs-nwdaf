#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

// H1.1: UE locations from the AMF (UE_MOBILITY input, TS 23.288 V18.13.0
// Table 6.7.2.2-1: the TA and cell a UE enters, with the time the AMF
// detected it), per interpretation I-12.
//
// Open5GS v2.8.0 exposes no Namf_EventExposure, but its AMF lists every UE on
// the metrics server (/ue-info, non-standard JSON, 100 per page) with the
// last known NR location and the AMF's timestamp of it. Polling that list
// every collection interval gives each UE's trajectory as a sequence of stays.

// An NR location: TAI (PLMN + TAC) and NR cell (NCI).
struct NwdafUeLocation {
    std::string mcc, mnc;
    std::string tac;     // 6 lower-case hex digits
    uint64_t    nci = 0; // 36-bit NR Cell Identity
    bool operator==(const NwdafUeLocation& o) const {
        return mcc == o.mcc && mnc == o.mnc && tac == o.tac && nci == o.nci;
    }
    bool operator!=(const NwdafUeLocation& o) const { return !(*this == o); }
};

// One entry of an AMF /ue-info page.
struct NwdafUeInfoItem {
    std::string     supi;
    NwdafUeLocation loc;
    std::chrono::system_clock::time_point detected;   // the AMF's location timestamp
};

// A UE's stay at one location: from when the AMF detected it there, to when
// it was detected elsewhere or last seen.
struct NwdafUeStay {
    NwdafUeLocation loc;
    std::chrono::system_clock::time_point from, to;
};

class NwdafUeLocationTracker {
public:
    using Clock = std::chrono::system_clock;

    explicit NwdafUeLocationTracker(std::chrono::seconds retention);

    // Parse one /ue-info page. Items without a location yet (timestamp 0) are
    // skipped. `has_next` is set when pager.next is present. nullopt when the
    // body isn't a /ue-info page.
    static std::optional<std::vector<NwdafUeInfoItem>> parsePage(const std::string& body, bool& has_next);

    // Apply one complete poll (every page) taken at `now`. A UE at the same
    // location extends its stay; a new location closes the previous stay at
    // the new detection time; a UE no longer listed has its stay closed at
    // the previous poll.
    void observe(Clock::time_point now, const std::vector<NwdafUeInfoItem>& items);

    // The UE's stays overlapping [from, to], clipped to it, oldest first.
    std::vector<NwdafUeStay> stays(const std::string& supi, Clock::time_point from, Clock::time_point to) const;

    // Since when trajectories are complete: the first poll, or the start of
    // the retention window. nullopt before the first poll.
    std::optional<Clock::time_point> heldSince(Clock::time_point now) const;

    // UEs in the last poll.
    size_t ueCount() const;

private:
    void prune(Clock::time_point now);   // mutex_ held

    mutable std::mutex m_;
    std::chrono::seconds retention_;
    std::map<std::string, std::deque<NwdafUeStay>> ues_;
    std::set<std::string> present_;   // listed in the last poll: their last stay is open
    std::optional<Clock::time_point> first_poll_, last_poll_;
};
