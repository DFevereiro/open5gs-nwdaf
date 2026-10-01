#include "nwdaf_config.hpp"
#include <yaml-cpp/yaml.h>
#include <stdexcept>
#include <random>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cctype>
#include <cstdio>


#include <regex>

static bool is_valid_uuid(const std::string& s) {
    static const std::regex UUID_RE(
        "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$",
        std::regex::icase
    );
    return std::regex_match(s, UUID_RE);
}

NwdafConfig NwdafConfig::load(const std::string& yaml_path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(yaml_path);
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to load config file '" + yaml_path + "': " + e.what());
    }

    auto n = root["nwdaf"];
    NwdafConfig cfg;

    cfg.nf_instance_id    = n["nf_instance_id"] ? n["nf_instance_id"].as<std::string>() : "";
    if (cfg.nf_instance_id.empty()) {
        cfg.nf_instance_id = root["nf_instance_id"] ? root["nf_instance_id"].as<std::string>() : "";
    }
    if (cfg.nf_instance_id.empty() || !is_valid_uuid(cfg.nf_instance_id)) {
        throw std::runtime_error("Invalid or missing nf_instance_id in config file. A valid UUID is required.");
    }

    cfg.plmn_mcc          = n["plmn_mcc"]          ? n["plmn_mcc"].as<std::string>()          : "999";
    cfg.plmn_mnc          = n["plmn_mnc"]          ? n["plmn_mnc"].as<std::string>()          : "70";
    cfg.sbi_bind_address  = n["sbi_bind_address"]  ? n["sbi_bind_address"].as<std::string>()  : "127.0.0.1";
    cfg.sbi_port          = n["sbi_port"]          ? n["sbi_port"].as<int>()                  : 7779;
    cfg.sbi_h2_port       = n["sbi_h2_port"]       ? n["sbi_h2_port"].as<int>()               : 7780;

    if (n["nf_service_names"]) {
        for (const auto& kv : n["nf_service_names"])
            cfg.nf_service_names[kv.first.as<std::string>()] = kv.second.as<std::string>();
    } else {
        cfg.nf_service_names = {{"AMF","amfd"},{"SMF","smfd"},{"UPF","upfd"},
                                 {"AUSF","ausfd"},{"UDM","udmd"},{"PCF","pcfd"},
                                 {"NRF","nrfd"},{"UDR","udrd"},{"BSF","bsfd"},{"NSSF","nssfd"}};
    }

    cfg.nrf_nf_discovery = n["nrf_nf_discovery"] ? n["nrf_nf_discovery"].as<bool>() : false;
    cfg.nrf_nf_discovery_interval_seconds = n["nrf_nf_discovery_interval_seconds"]
        ? n["nrf_nf_discovery_interval_seconds"].as<int>() : 60;
    cfg.nrf_nf_status_window_seconds = n["nrf_nf_status_window_seconds"]
        ? n["nrf_nf_status_window_seconds"].as<int>() : 3600;
    if (cfg.nrf_nf_discovery_interval_seconds <= 0 || cfg.nrf_nf_status_window_seconds <= 0)
        throw std::runtime_error("nrf_nf_discovery_interval_seconds and nrf_nf_status_window_seconds must be positive");

    // H1.7: NF instance IDs of the monitored NFs (Open5GS deployment mechanism
    // until NRF discovery, H1.9). Keys must be monitored NF types.
    if (n["nf_instance_ids"]) {
        for (const auto& kv : n["nf_instance_ids"]) {
            const std::string type = kv.first.as<std::string>();
            const std::string id   = kv.second.as<std::string>();
            if (!cfg.nf_service_names.count(type))
                throw std::runtime_error("nf_instance_ids: " + type +
                                         " is not a monitored NF type (see nf_service_names)");
            if (!is_valid_uuid(id))
                throw std::runtime_error("nf_instance_ids: " + type + " is not a valid UUID: " + id);
            cfg.nf_instance_ids[type] = id;
        }
    }

    // H1.1: Prometheus metrics endpoints of the NFs (OAM input).
    if (n["oam_metrics_endpoints"]) {
        for (const auto& kv : n["oam_metrics_endpoints"]) {
            const std::string type = kv.first.as<std::string>();
            const std::string url  = kv.second.as<std::string>();
            if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
                throw std::runtime_error("oam_metrics_endpoints: " + type + " is not an http(s) URL: " + url);
            cfg.oam_metrics_endpoints[type] = url;
        }
    }

    // H1.2: per-slice admission capacity (I-9).
    if (n["slice_capacity"]) {
        static const std::regex SD_RE("^[0-9a-fA-F]{6}$");
        for (const auto& e : n["slice_capacity"]) {
            NwdafSliceCapacity c;
            const auto snssai = e["snssai"];
            if (!snssai || !snssai["sst"])
                throw std::runtime_error("slice_capacity: every entry needs snssai.sst");
            c.sst = snssai["sst"].as<int>();
            if (c.sst < 0 || c.sst > 255)
                throw std::runtime_error("slice_capacity: sst must be 0-255");
            if (snssai["sd"]) {
                c.sd = snssai["sd"].as<std::string>();
                if (!std::regex_match(c.sd, SD_RE))
                    throw std::runtime_error("slice_capacity: sd must be 6 hexadecimal digits: " + c.sd);
                for (auto& ch : c.sd) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            c.max_ues          = e["max_ues"]          ? e["max_ues"].as<long>()          : 0;
            c.max_pdu_sessions = e["max_pdu_sessions"] ? e["max_pdu_sessions"].as<long>() : 0;
            if (c.max_ues < 0 || c.max_pdu_sessions < 0 || (c.max_ues == 0 && c.max_pdu_sessions == 0))
                throw std::runtime_error("slice_capacity " + c.key() +
                                         ": set max_ues and/or max_pdu_sessions to a positive number");
            if (c.max_ues > 0 && !cfg.oam_metrics_endpoints.count("AMF"))
                throw std::runtime_error("slice_capacity " + c.key() +
                                         ": max_ues needs oam_metrics_endpoints.AMF");
            if (c.max_pdu_sessions > 0 && !cfg.oam_metrics_endpoints.count("SMF"))
                throw std::runtime_error("slice_capacity " + c.key() +
                                         ": max_pdu_sessions needs oam_metrics_endpoints.SMF");
            for (const auto& other : cfg.slice_capacity)
                if (other.key() == c.key())
                    throw std::runtime_error("slice_capacity: " + c.key() + " is configured twice");
            cfg.slice_capacity.push_back(c);
        }
    }

    // H1.4: the served tracking areas (I-11). tac: 4 or 6 hex digits, or a number.
    if (n["served_tai_list"]) {
        static const std::regex TAC_RE("^([0-9a-fA-F]{4}|[0-9a-fA-F]{6})$");
        for (const auto& e : n["served_tai_list"]) {
            NwdafTai t;
            t.mcc = e["mcc"] ? e["mcc"].as<std::string>() : cfg.plmn_mcc;
            t.mnc = e["mnc"] ? e["mnc"].as<std::string>() : cfg.plmn_mnc;
            if (!e["tac"])
                throw std::runtime_error("served_tai_list: every entry needs a tac");
            const std::string tac = e["tac"].as<std::string>();
            // An unquoted number is decimal, as Open5GS amf.yaml writes TACs
            // (tac: 1000); a quoted string of 4 or 6 digits is hex ("0003e8").
            const bool digits = !tac.empty() && tac.find_first_not_of("0123456789") == std::string::npos;
            const bool quoted = e["tac"].Tag() == "!";
            long value;
            if (digits && !quoted) value = std::stol(tac);
            else if (std::regex_match(tac, TAC_RE)) value = std::stol(tac, nullptr, 16);
            else if (digits) value = std::stol(tac);
            else throw std::runtime_error("served_tai_list: tac must be a number, or 4 or 6 hex digits quoted: " + tac);
            if (value < 0 || value > 0xFFFFFF)
                throw std::runtime_error("served_tai_list: tac out of range: " + tac);
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%06lx", value);
            t.tac = buf;
            cfg.served_tai_list.push_back(t);
        }
    }
    cfg.network_performance_window_seconds = n["network_performance_window_seconds"]
        ? n["network_performance_window_seconds"].as<int>() : 300;
    if (cfg.network_performance_window_seconds <= 0)
        throw std::runtime_error("network_performance_window_seconds must be positive");

    if (n["amf_ue_info_endpoint"]) {
        cfg.amf_ue_info_endpoint = n["amf_ue_info_endpoint"].as<std::string>();
        if (!cfg.amf_ue_info_endpoint.empty() && cfg.amf_ue_info_endpoint.rfind("http://", 0) != 0 &&
            cfg.amf_ue_info_endpoint.rfind("https://", 0) != 0)
            throw std::runtime_error("amf_ue_info_endpoint is not an http(s) URL: " + cfg.amf_ue_info_endpoint);
    }
    cfg.ue_mobility_window_seconds = n["ue_mobility_window_seconds"]
        ? n["ue_mobility_window_seconds"].as<int>() : 3600;
    if (cfg.ue_mobility_window_seconds <= 0)
        throw std::runtime_error("ue_mobility_window_seconds must be positive");
    cfg.ue_location_history_seconds = n["ue_location_history_seconds"]
        ? n["ue_location_history_seconds"].as<int>() : 86400;
    if (cfg.ue_location_history_seconds <= 0)
        throw std::runtime_error("ue_location_history_seconds must be positive");

    if (n["open5gs_version"]) cfg.open5gs_version = n["open5gs_version"].as<std::string>();
    cfg.prediction_horizon_seconds = n["prediction_horizon_seconds"]
        ? n["prediction_horizon_seconds"].as<int>() : 900;
    cfg.prediction_min_samples = n["prediction_min_samples"] ? n["prediction_min_samples"].as<int>() : 10;
    cfg.prediction_tolerance   = n["prediction_tolerance"] ? n["prediction_tolerance"].as<double>() : 10.0;
    if (cfg.prediction_horizon_seconds < 0 || cfg.prediction_min_samples < 2 || cfg.prediction_tolerance <= 0)
        throw std::runtime_error("prediction_horizon_seconds must be >= 0, prediction_min_samples >= 2 "
                                 "and prediction_tolerance > 0");

    cfg.slice_load_window_seconds = n["slice_load_window_seconds"]
        ? n["slice_load_window_seconds"].as<int>() : 300;
    if (cfg.slice_load_window_seconds <= 0)
        throw std::runtime_error("slice_load_window_seconds must be positive");

    if (n["throughput_interfaces"]) {
        for (const auto& iface : n["throughput_interfaces"])
            cfg.throughput_interfaces.push_back(iface.as<std::string>());
    } else {
        cfg.throughput_interfaces = {"ogstun"};
    }

    cfg.throughput_history_size      = n["throughput_history_size"]      ? n["throughput_history_size"].as<int>()      : 360;
    cfg.collection_interval_seconds  = n["collection_interval_seconds"]  ? n["collection_interval_seconds"].as<int>()  : 10;
    cfg.amf_journal_lines            = n["amf_journal_lines"]            ? n["amf_journal_lines"].as<int>()            : 500;
    cfg.smf_journal_lines            = n["smf_journal_lines"]            ? n["smf_journal_lines"].as<int>()            : 500;
    cfg.supi_regex                   = n["supi_regex"]                   ? n["supi_regex"].as<std::string>()           : "imsi-(\\d{15})";

    cfg.mongodb_uri = n["mongodb_uri"] ? n["mongodb_uri"].as<std::string>() : "mongodb://127.0.0.1:27017";
    cfg.mongodb_db  = n["mongodb_db"]  ? n["mongodb_db"].as<std::string>()  : "open5gs";

    cfg.nrf_uri                        = n["nrf_uri"]                        ? n["nrf_uri"].as<std::string>()               : "http://127.0.0.1:7777";
    cfg.nrf_register_on_startup        = n["nrf_register_on_startup"]        ? n["nrf_register_on_startup"].as<bool>()       : true;
    cfg.nrf_heartbeat_interval_seconds = n["nrf_heartbeat_interval_seconds"] ? n["nrf_heartbeat_interval_seconds"].as<int>() : 60;

    cfg.model_dir                = n["model_dir"]                ? n["model_dir"].as<std::string>()       : "/opt/nwdaf/models";
    cfg.anomaly_contamination    = n["anomaly_contamination"]    ? n["anomaly_contamination"].as<double>() : 0.10;
    cfg.anomaly_seed             = n["anomaly_seed"]             ? n["anomaly_seed"].as<unsigned int>()     : 0;
    cfg.anomaly_min_samples      = n["anomaly_min_samples"]      ? n["anomaly_min_samples"].as<int>()      : 10;
    cfg.baseline_stddev_min_kbps = n["baseline_stddev_min_kbps"] ? n["baseline_stddev_min_kbps"].as<double>() : 0.5;
    cfg.ewma_alpha               = n["ewma_alpha"]               ? n["ewma_alpha"].as<double>()             : 0.3;

    cfg.log_level = n["log_level"] ? n["log_level"].as<std::string>() : "info";
    cfg.log_file  = n["log_file"]  ? n["log_file"].as<std::string>()  : "/var/log/open5gs/nwdaf.log";

    // PROD-01
    cfg.history_backend = n["history_backend"] ? n["history_backend"].as<std::string>() : "none";
    cfg.history_db_path = n["history_db_path"] ? n["history_db_path"].as<std::string>() : "/opt/nwdaf/history.db";

    // PROD-06
    cfg.rate_limit_per_ip_rps = n["rate_limit_per_ip_rps"] ? n["rate_limit_per_ip_rps"].as<int>() : 10;
    cfg.rate_limit_global_rps = n["rate_limit_global_rps"] ? n["rate_limit_global_rps"].as<int>() : 100;

    // ARCH-03: configurable NETWORK_PERFORMANCE weights
    if (n["network_performance_weights"]) {
        auto w = n["network_performance_weights"];
        cfg.np_weight_nf_health = w["nf_health"] ? w["nf_health"].as<double>() : 0.6;
        cfg.np_weight_dl        = w["dl_score"]  ? w["dl_score"].as<double>()  : 0.2;
        cfg.np_weight_pdu       = w["pdu_score"] ? w["pdu_score"].as<double>() : 0.2;
    }
    // Validate that the weights sum to 1.0 (±0.001)
    double weight_sum = cfg.np_weight_nf_health + cfg.np_weight_dl + cfg.np_weight_pdu;
    if (std::abs(weight_sum - 1.0) > 0.001)
        throw std::runtime_error(
            "network_performance_weights must sum to 1.0, got " + std::to_string(weight_sum));

    // ARCH-05: TLS / mTLS configuration
    cfg.tls_enabled   = n["tls_enabled"]   ? n["tls_enabled"].as<bool>()          : false;
    cfg.tls_cert_file = n["tls_cert_file"] ? n["tls_cert_file"].as<std::string>() : "/etc/open5gs/tls/nwdaf.pem";
    cfg.tls_key_file  = n["tls_key_file"]  ? n["tls_key_file"].as<std::string>()  : "/etc/open5gs/tls/nwdaf.key";
    cfg.tls_ca_file   = n["tls_ca_file"]   ? n["tls_ca_file"].as<std::string>()   : "";

    // P1-4: OAuth 2.0
    cfg.oauth_enabled = n["oauth_enabled"] ? n["oauth_enabled"].as<bool>()        : false;
    cfg.oauth_nrf_public_key_file = n["oauth_nrf_public_key_file"]
        ? n["oauth_nrf_public_key_file"].as<std::string>() : "";
    cfg.oauth_shared_secret_file = n["oauth_shared_secret_file"]
        ? n["oauth_shared_secret_file"].as<std::string>() : "";

    // H1.6: path to the OpenAPI document served from the SBI.
    cfg.openapi_spec_path = n["openapi_spec_path"]
        ? n["openapi_spec_path"].as<std::string>()
        : "/etc/open5gs/openapi/nwdaf-analytics-v1.yaml";

    // H1.7: official 3GPP Rel-18 OpenAPI artifacts (docs/frozen-standards.md).
    cfg.openapi_3gpp_dir = n["openapi_3gpp_dir"]
        ? n["openapi_3gpp_dir"].as<std::string>()
        : "/etc/open5gs/openapi/3gpp";

    return cfg;
}
