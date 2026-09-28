// H1.7 — supported-features negotiation (TS 29.500 V18.10.0 §6.6.2) and the
// TS 29.571 V18.12.0 SupportedFeatures encoding.
#include <catch2/catch_test_macros.hpp>
#include "nwdaf_supported_features.hpp"

using FS = NwdafFeatureSet;
using SF = NwdafSupportedFeatures;

TEST_CASE("H1.7: SupportedFeatures parses the TS 29.571 examples") {
    // "if only the first feature … is set … "1", or … "001""
    REQUIRE(*FS::parse("1") == FS{1});
    REQUIRE(*FS::parse("001") == FS{1});
    // "32 features defined, and only the last … "80000000""
    REQUIRE(*FS::parse("80000000") == FS{32});
    // Table 5.2.2-3: one character carries features n..n+3, n+3 most significant.
    REQUIRE(*FS::parse("A") == FS{2, 4});
    REQUIRE(*FS::parse("1F") == (FS{1, 2, 3, 4, 5}));
}

TEST_CASE("H1.7: SupportedFeatures accepts either case and the empty string") {
    REQUIRE(*FS::parse("aB") == *FS::parse("Ab"));
    REQUIRE(FS::parse("")->empty());
}

TEST_CASE("H1.7: SupportedFeatures rejects non-hexadecimal characters") {
    REQUIRE_FALSE(FS::parse("G"));
    REQUIRE_FALSE(FS::parse("0x1"));
    REQUIRE_FALSE(FS::parse("1 2"));
}

TEST_CASE("H1.7: SupportedFeatures encodes minimally and round-trips") {
    REQUIRE(FS{}.toHex() == "0");
    REQUIRE(FS{1}.toHex() == "1");
    REQUIRE(FS{32}.toHex() == "80000000");
    for (const char* hex : {"1", "A", "1F", "80000000", "400000000000000"}) {
        INFO("hex=" << hex);
        REQUIRE(FS::parse(hex)->toHex() == hex);
    }
    REQUIRE(FS::parse("00A")->toHex() == "A");
}

TEST_CASE("H1.7: negotiation is the intersection") {
    const FS nwdaf{7, 8, 17};
    const FS consumer{1, 7, 17, 53};
    REQUIRE(nwdaf.intersect(consumer) == (FS{7, 17}));
    REQUIRE(nwdaf.intersect(consumer) == consumer.intersect(nwdaf));
    REQUIRE(FS{}.intersect(consumer).empty());
}

TEST_CASE("H1.7: feature numbers follow each API's own table") {
    using A = NnwdafApi;
    // Same feature, different bit per API (Appendix A.3).
    REQUIRE(SF::eventFeature(A::AnalyticsInfo, "NF_LOAD") == 8u);
    REQUIRE(SF::eventFeature(A::EventsSubscription, "NF_LOAD") == 7u);
    REQUIRE(SF::eventFeature(A::AnalyticsInfo, "SERVICE_EXPERIENCE") == 4u);
    REQUIRE(SF::eventFeature(A::EventsSubscription, "SERVICE_EXPERIENCE") == 1u);
    REQUIRE(SF::eventFeature(A::AnalyticsInfo, "RED_TRANS_EXP") == 18u);
    REQUIRE(SF::eventFeature(A::EventsSubscription, "RED_TRANS_EXP") == 19u);
    REQUIRE(SF::number(A::AnalyticsInfo, SF::Feature::EneNA) == 10u);
    REQUIRE(SF::number(A::EventsSubscription, SF::Feature::EneNA) == 11u);
    REQUIRE(SF::number(A::EventsSubscription, SF::Feature::StatisticsFailure) == 53u);
    REQUIRE_FALSE(SF::number(A::AnalyticsInfo, SF::Feature::StatisticsFailure));
}

TEST_CASE("H1.7: S-1 — SM_CONGESTION has no feature on Nnwdaf_EventsSubscription") {
    REQUIRE(SF::eventFeature(NnwdafApi::AnalyticsInfo, "SM_CONGESTION") == 15u);
    REQUIRE_FALSE(SF::eventFeature(NnwdafApi::EventsSubscription, "SM_CONGESTION"));
}

TEST_CASE("H1.7: the local bitmask covers exactly the advertised analytics") {
    // Without NF instance IDs NF_LOAD is not advertised: no feature is claimed.
    NwdafConfig cfg;
    REQUIRE(SF::local(NnwdafApi::AnalyticsInfo, cfg).toHex() == "0");
    REQUIRE(SF::local(NnwdafApi::EventsSubscription, cfg).toHex() == "0");

    // With them, NfLoad — bit 8 on AnalyticsInfo, bit 7 on EventsSubscription.
    cfg.nf_instance_ids = {{"AMF", "11111111-1111-4111-8111-111111111111"}};
    REQUIRE(SF::local(NnwdafApi::AnalyticsInfo, cfg).toHex() == "80");
    REQUIRE(SF::local(NnwdafApi::EventsSubscription, cfg).toHex() == "40");
}
