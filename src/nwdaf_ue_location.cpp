#include "nwdaf_ue_location.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace {

constexpr size_t MAX_STAYS_PER_UE = 10000;

bool digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

}  // namespace

NwdafUeLocationTracker::NwdafUeLocationTracker(std::chrono::seconds retention) : retention_(retention) {}

std::optional<std::vector<NwdafUeInfoItem>> NwdafUeLocationTracker::parsePage(const std::string& body,
                                                                            bool& has_next) {
    has_next = false;
    const json page = json::parse(body, nullptr, false);
    if (page.is_discarded() || !page.is_object() || !page.contains("items") || !page["items"].is_array())
        return std::nullopt;
    if (page.contains("pager") && page["pager"].is_object()) has_next = page["pager"].contains("next");

    std::vector<NwdafUeInfoItem> out;
    for (const auto& ue : page["items"]) {
        if (!ue.is_object() || !ue.contains("supi") || !ue["supi"].is_string()) continue;
        if (!ue.contains("location") || !ue["location"].is_object()) continue;
        const json& loc = ue["location"];
        // The AMF emits a location for every UE; timestamp 0 means it has
        // none yet.
        const double us = loc.value("timestamp", 0.0);
        if (us <= 0 || !loc.contains("nr_tai") || !loc.contains("nr_cgi")) continue;
        const std::string plmn = loc["nr_tai"].value("plmn", std::string());
        std::string tac = loc["nr_tai"].value("tac_hex", std::string());
        if (!digits(plmn) || (plmn.size() != 5 && plmn.size() != 6) || tac.empty() || tac.size() > 6) continue;
        for (auto& c : tac) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        tac.insert(0, 6 - tac.size(), '0');

        NwdafUeInfoItem item;
        item.supi = ue["supi"].get<std::string>();
        item.loc.mcc = plmn.substr(0, 3);
        item.loc.mnc = plmn.substr(3);
        item.loc.tac = tac;
        item.loc.nci = static_cast<uint64_t>(loc["nr_cgi"].value("nci", 0.0)) & 0xFFFFFFFFFULL;
        item.detected = Clock::time_point(std::chrono::duration_cast<Clock::duration>(
            std::chrono::microseconds(static_cast<int64_t>(us))));
        out.push_back(std::move(item));
    }
    return out;
}

void NwdafUeLocationTracker::observe(Clock::time_point now, const std::vector<NwdafUeInfoItem>& items) {
    std::lock_guard<std::mutex> lk(m_);
    std::set<std::string> present;
    for (const auto& item : items) {
        if (!present.insert(item.supi).second) continue;   // listed twice in one poll
        auto& stays = ues_[item.supi];
        const bool open = present_.count(item.supi) && !stays.empty();
        if (open && stays.back().loc == item.loc) {
            stays.back().to = now;
            continue;
        }
        // Table 6.7.2.2-1: the stay starts when the AMF detected the UE at
        // this location, bounded by what is already known about the UE.
        Clock::time_point lower = now - retention_;
        if (!stays.empty()) lower = open ? stays.back().from : stays.back().to;
        const Clock::time_point from = std::clamp(item.detected, std::min(lower, now), now);
        if (open) stays.back().to = from;   // there until detected elsewhere
        stays.push_back({item.loc, from, now});
        if (stays.size() > MAX_STAYS_PER_UE) stays.pop_front();
    }
    // A UE no longer listed (deregistered) keeps its last stay closed at the
    // previous poll.
    present_ = std::move(present);
    if (!first_poll_) first_poll_ = now;
    last_poll_ = now;
    prune(now);
}

void NwdafUeLocationTracker::prune(Clock::time_point now) {
    const auto cutoff = now - retention_;
    for (auto it = ues_.begin(); it != ues_.end();) {
        auto& stays = it->second;
        while (!stays.empty() && stays.front().to < cutoff) stays.pop_front();
        if (stays.empty() && !present_.count(it->first)) it = ues_.erase(it);
        else ++it;
    }
}

std::vector<NwdafUeStay> NwdafUeLocationTracker::stays(const std::string& supi, Clock::time_point from,
                                                       Clock::time_point to) const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<NwdafUeStay> out;
    const auto it = ues_.find(supi);
    if (it == ues_.end()) return out;
    for (const auto& s : it->second) {
        if (s.to <= s.from || s.to <= from || s.from >= to) continue;
        out.push_back({s.loc, std::max(s.from, from), std::min(s.to, to)});
    }
    return out;
}

std::vector<NwdafUeStay> NwdafUeLocationTracker::allStays(Clock::time_point from, Clock::time_point to) const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<NwdafUeStay> out;
    for (const auto& [supi, stays] : ues_)
        for (const auto& s : stays) {
            if (s.to <= s.from || s.to <= from || s.from >= to) continue;
            out.push_back({s.loc, std::max(s.from, from), std::min(s.to, to)});
        }
    return out;
}

std::optional<NwdafUeLocationTracker::Clock::time_point> NwdafUeLocationTracker::heldSince(Clock::time_point now) const {
    std::lock_guard<std::mutex> lk(m_);
    if (!first_poll_) return std::nullopt;
    return std::max(*first_poll_, now - retention_);
}

std::optional<NwdafUeLocationTracker::Clock::time_point> NwdafUeLocationTracker::lastPoll() const {
    std::lock_guard<std::mutex> lk(m_);
    return last_poll_;
}

size_t NwdafUeLocationTracker::ueCount() const {
    std::lock_guard<std::mutex> lk(m_);
    return present_.size();
}
