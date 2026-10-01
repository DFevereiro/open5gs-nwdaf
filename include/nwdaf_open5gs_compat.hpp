#pragma once
#include "nwdaf_config.hpp"
#include <string>
#include <vector>

// COMPAT-02: the Open5GS behaviours this NWDAF works around, in one place.
//
// Each quirk names the code that handles it (tagged with its ID, e.g.
// O5GS-04) and the interoperability record that verified it
// (docs/3gpp-rel18-compliance.md). They were verified on the Open5GS
// versions in VERIFIED_VERSIONS; a deployment declares its version in
// `open5gs_version`, and an unverified one is warned about at startup, since
// a shim may then no longer match.
struct NwdafOpen5gsQuirk {
    const char* id;         // O5GS-NN, the tag at the handling code
    const char* behaviour;  // what Open5GS does
    const char* handling;   // what the NWDAF does about it
    const char* record;     // the interoperability record (date)
};

class NwdafOpen5gsCompat {
public:
    static const std::vector<std::string> VERIFIED_VERSIONS;
    static const std::vector<NwdafOpen5gsQuirk>& quirks();
    static bool verified(const std::string& version);
    // Logs the declared version and the quirks handled; warns when the
    // version hasn't been verified.
    static void announce(const NwdafConfig& cfg);
};
