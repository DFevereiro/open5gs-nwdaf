#pragma once
#include <string>
#include <map>
#include <vector>

// H1.2: the admission capacity of one network slice (S-NSSAI), as an NSACF
// is configured with it (TS 23.501 V18 §5.15.11.0). Open5GS has no NSACF, so
// the operator supplies it; slice load level is computed against it (I-9).
struct NwdafSliceCapacity {
    int         sst = 0;
    std::string sd;                // 6 lower-case hex digits; empty = no SD
    long        max_ues = 0;           // 0 = not configured
    long        max_pdu_sessions = 0;  // 0 = not configured

    // The S-NSSAI as Open5GS labels its per-slice metrics: "1-000001" or "1".
    std::string key() const { return sd.empty() ? std::to_string(sst) : std::to_string(sst) + "-" + sd; }
};

// H1.4: one tracking area this core serves (TS 29.571 Tai).
struct NwdafTai {
    std::string mcc, mnc;
    std::string tac;   // 6 lower-case hex digits

    bool operator==(const NwdafTai& o) const { return mcc == o.mcc && mnc == o.mnc && tac == o.tac; }
};

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
    // H1.8: HTTP/2 listener for the 3GPP interfaces (TS 29.500 §5.2); 0 disables
    // it. Needs a build with NWDAF_USE_HTTP2; port 7779 stays HTTP/1.1 for the
    // operator API and dashboard.
    int         sbi_h2_port = 7780;

    std::map<std::string, std::string> nf_service_names;

    // H1.7: NF type → NF instance ID (UUID) of each monitored NF, needed by the
    // Rel-18 NfLoadLevelInformation (nfInstanceId is mandatory). An Open5GS
    // deployment mechanism until NRF discovery (H1.9) supplies the IDs; NF_LOAD
    // is advertised on the 3GPP interfaces only when this map is configured.
    std::map<std::string, std::string> nf_instance_ids;
    // H1.9: poll the NRF every interval (TS 29.510 NFListRetrieval and
    // NFProfileRetrieval) to resolve the IDs not configured above and to
    // observe each NF's NRF status.
    bool nrf_nf_discovery = false;
    int  nrf_nf_discovery_interval_seconds = 60;
    // H1.9: window over which NF_LOAD nfStatus is computed from those polls
    // (TS 29.520 NfStatus, I-8).
    int  nrf_nf_status_window_seconds = 3600;
    std::vector<std::string> throughput_interfaces;
    // H1.1: NF type → URL of the NF's Prometheus metrics endpoint (Open5GS
    // `metrics.server`), scraped every collection interval as OAM input: the
    // measurements carry TS 28.552 names. Empty = no scraping.
    std::map<std::string, std::string> oam_metrics_endpoints;
    // H1.2: per-slice admission capacity (I-9). SLICE_LOAD_LEVEL and
    // NSI_LOAD_LEVEL are served and advertised only for configured slices.
    // max_ues needs oam_metrics_endpoints.AMF; max_pdu_sessions needs .SMF.
    std::vector<NwdafSliceCapacity> slice_capacity;
    // H1.2: period of slice load statistics when the consumer gives no
    // analytics target period (startTs/endTs): the last N seconds (I-9).
    int slice_load_window_seconds = 300;
    // H1.4: the tracking areas the core serves. NETWORK_PERFORMANCE is served
    // and advertised only when set: Open5GS counts are per AMF, so they apply
    // to the whole served area (I-11).
    std::vector<NwdafTai> served_tai_list;
    // H1.4: period of NETWORK_PERFORMANCE statistics without startTs/endTs.
    int network_performance_window_seconds = 300;
    // H1.1: the AMF's per-UE JSON endpoint (Open5GS v2.8.0 /ue-info on its
    // metrics server), polled every collection interval for UE locations.
    // UE_MOBILITY is served and advertised only when set (I-12). Empty = off.
    std::string amf_ue_info_endpoint;
    // H1.1: period of UE_MOBILITY statistics without startTs/endTs.
    int ue_mobility_window_seconds = 3600;
    // H1.1: how long UE location history is kept (memory only).
    int ue_location_history_seconds = 86400;
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

    // P1-4 / H1.10: OAuth 2.0 access tokens on the 3GPP interfaces (TS 33.501
    // §13.4.1). When enabled, requests without a valid token are rejected.
    // The verification key is a deployment mechanism: the NRF's public key
    // (RS256/ES256) or a secret shared with it (HS256). Needs NWDAF_USE_TLS.
    bool        oauth_enabled = false;
    std::string oauth_nrf_public_key_file;
    std::string oauth_shared_secret_file;

    // H1.6: OpenAPI document served from GET /nwdaf-analytics/v1/openapi.
    // Optional — when the file is absent the endpoint reports 404 and the rest
    // of the SBI is unaffected, following the graceful-degradation convention.
    std::string openapi_spec_path = "/etc/open5gs/openapi/nwdaf-analytics-v1.yaml";

    // H1.7: directory of the official 3GPP Rel-18 OpenAPI artifacts pinned in
    // docs/frozen-standards.md, used to validate 3GPP-interface requests.
    std::string openapi_3gpp_dir = "/etc/open5gs/openapi/3gpp";
};
