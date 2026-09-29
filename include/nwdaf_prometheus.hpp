#pragma once
#include <map>
#include <string>
#include <vector>

// H1.1: one sample of the Prometheus text exposition format (0.0.4).
struct NwdafPromSample {
    std::string name;
    std::map<std::string, std::string> labels;
    double value = 0.0;
};

// H1.1: parser for the Prometheus text format that Open5GS NFs serve on their
// `metrics` endpoint. The measurements carry TS 28.552 names (for example
// fivegs_amffunction_rm_registeredsubnbr{plmnid,snssai}), which TS 23.288
// V18.13.0 accepts as OAM input (e.g. Table 6.3.2A-1).
class NwdafPrometheusText {
public:
    // The samples in `text`. Comments (# HELP, # TYPE) and malformed lines are
    // skipped; a sample's optional timestamp is ignored.
    static std::vector<NwdafPromSample> parse(const std::string& text);
};
