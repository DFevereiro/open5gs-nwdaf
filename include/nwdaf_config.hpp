#pragma once
#include <string>
#include <map>
#include <vector>

class NwdafConfig {
public:
    static NwdafConfig load(const std::string& yaml_path);

    std::string nf_instance_id;
    std::string plmn_mcc, plmn_mnc;
    int         served_snssai_sst = 1;
    std::string served_snssai_sd = "000001";
    std::string served_dnn = "internet";
    std::string sbi_bind_address;
    int         sbi_port = 7779;

    std::map<std::string, std::string> nf_service_names;

    // H1.7: NF type → NF instance ID (UUID) of each monitored NF, needed by the
    // Rel-18 NfLoadLevelInformation (nfInstanceId is mandatory). An Open5GS
    // deployment mechanism until NRF discovery (H1.9) supplies the IDs; NF_LOAD
    // is advertised on the 3GPP interfaces only when this map is configured.
    std::map<std::string, std::string> nf_instance_ids;
    std::vector<std::string> throughput_interfaces;
    int    throughput_history_size      = 360;
    int    collection_interval_seconds  = 10;
    int    amf_journal_lines            = 500;
    int    smf_journal_lines            = 500;
    std::string supi_regex;

    std::string mongodb_uri;
    std::string mongodb_db;

    std::string nrf_uri;
    bool        nrf_register_on_startup         = true;
    int         nrf_heartbeat_interval_seconds  = 60;

    std::string model_dir;
    double      anomaly_contamination    = 0.10;
    unsigned int anomaly_seed            = 0;
    int         anomaly_min_samples      = 10;
    double      baseline_stddev_min_kbps = 0.5;
    double      ewma_alpha               = 0.3;

    std::string log_level;
    std::string log_file;

    // PROD-01: throughput history persistence
    std::string history_backend;    // "none" | "sqlite" | "mongodb"
    std::string history_db_path;

    // PROD-06: rate limiting
    int rate_limit_per_ip_rps = 10;
    int rate_limit_global_rps = 100;

    // ARCH-03: configurable NETWORK_PERFORMANCE scoring weights.
    // Must sum to 1.0 (±0.001); validated in NwdafConfig::load().
    double np_weight_nf_health = 0.6;
    double np_weight_dl        = 0.2;
    double np_weight_pdu       = 0.2;

    // ARCH-05: TLS / mTLS for SBI interface (TS 33.501 §13.3).
    // Requires rebuilding with -DNWDAF_USE_TLS=ON (links OpenSSL).
    bool        tls_enabled   = false;
    std::string tls_cert_file = "/etc/open5gs/tls/nwdaf.pem";
    std::string tls_key_file  = "/etc/open5gs/tls/nwdaf.key";
    // H1.10: client-certificate CA. Non-empty enables mutual TLS (TS 33.501
    // §13.1): clients without a certificate signed by this CA are refused.
    std::string tls_ca_file;

    // P1-4: OAuth 2.0 access-token auth
    bool oauth_enabled = false;

    // H1.6: OpenAPI document served from GET /nwdaf-analytics/v1/openapi.
    // Optional — when the file is absent the endpoint reports 404 and the rest
    // of the SBI is unaffected, following the graceful-degradation convention.
    std::string openapi_spec_path = "/etc/open5gs/openapi/nwdaf-analytics-v1.yaml";

    // H1.7: directory of the official 3GPP Rel-18 OpenAPI artifacts pinned in
    // docs/frozen-standards.md, used to validate 3GPP-interface requests.
    std::string openapi_3gpp_dir = "/etc/open5gs/openapi/3gpp";
};
