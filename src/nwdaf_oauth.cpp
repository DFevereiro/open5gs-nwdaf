#include "nwdaf_oauth.hpp"

#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

namespace {

// RFC 4648 §5 base64url, unpadded (JWS).
bool base64UrlDecode(const std::string& in, std::string& out) {
    out.clear();
    unsigned int buf = 0;
    int bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z')      v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-')             v = 62;
        else if (c == '_')             v = 63;
        else if (c == '=')             break;
        else return false;
        buf = (buf << 6) | static_cast<unsigned int>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return true;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool digestVerify(EVP_PKEY* key, const std::string& input, const std::string& sig) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    const bool ok = ctx &&
        EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, key) == 1 &&
        EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(sig.data()), sig.size(),
                         reinterpret_cast<const unsigned char*>(input.data()), input.size()) == 1;
    EVP_MD_CTX_free(ctx);
    return ok;
}

}  // namespace

// ── Key providers (deployment mechanism) ────────────────────────────────────

NwdafPemKeyProvider::NwdafPemKeyProvider(const std::string& pem_file) {
    FILE* f = std::fopen(pem_file.c_str(), "r");
    if (!f) throw std::runtime_error("OAuth2: cannot read NRF public key " + pem_file);
    key_ = PEM_read_PUBKEY(f, nullptr, nullptr, nullptr);
    std::fclose(f);
    if (!key_) throw std::runtime_error("OAuth2: " + pem_file + " is not a PEM public key");
}

NwdafPemKeyProvider::~NwdafPemKeyProvider() {
    EVP_PKEY_free(static_cast<EVP_PKEY*>(key_));
}

bool NwdafPemKeyProvider::verify(const std::string& alg, const std::string& input,
                                 const std::string& sig) const {
    auto* key = static_cast<EVP_PKEY*>(key_);
    if (alg == "RS256" && EVP_PKEY_base_id(key) == EVP_PKEY_RSA)
        return digestVerify(key, input, sig);
    if (alg == "ES256" && EVP_PKEY_base_id(key) == EVP_PKEY_EC) {
        // JWS carries ECDSA as raw R||S (RFC 7518 §3.4); OpenSSL wants DER.
        if (sig.size() != 64) return false;
        ECDSA_SIG* es = ECDSA_SIG_new();
        BIGNUM* r = BN_bin2bn(reinterpret_cast<const unsigned char*>(sig.data()), 32, nullptr);
        BIGNUM* s = BN_bin2bn(reinterpret_cast<const unsigned char*>(sig.data()) + 32, 32, nullptr);
        ECDSA_SIG_set0(es, r, s);
        unsigned char* der = nullptr;
        const int der_len = i2d_ECDSA_SIG(es, &der);
        ECDSA_SIG_free(es);
        const bool ok = der_len > 0 &&
            digestVerify(key, input, std::string(reinterpret_cast<char*>(der), static_cast<size_t>(der_len)));
        OPENSSL_free(der);
        return ok;
    }
    return false;   // algorithm does not match the configured key
}

NwdafSharedSecretProvider::NwdafSharedSecretProvider(const std::string& secret_file)
    : secret_(readFile(secret_file)) {
    while (!secret_.empty() && (secret_.back() == '\n' || secret_.back() == '\r')) secret_.pop_back();
    if (secret_.empty()) throw std::runtime_error("OAuth2: shared secret " + secret_file + " is empty");
}

bool NwdafSharedSecretProvider::verify(const std::string& alg, const std::string& input,
                                       const std::string& sig) const {
    if (alg != "HS256") return false;
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), secret_.data(), static_cast<int>(secret_.size()),
         reinterpret_cast<const unsigned char*>(input.data()), input.size(), mac, &len);
    return sig.size() == len && CRYPTO_memcmp(mac, sig.data(), len) == 0;
}

// ── Validator (TS 33.501 §13.4.1.1.2 step 2) ────────────────────────────────

NwdafAccessTokenValidator::NwdafAccessTokenValidator(
    const NwdafConfig& config, std::shared_ptr<const NwdafTokenKeyProvider> keys,
    const NwdafSchemaValidator* official)
    : config_(config), keys_(std::move(keys)), official_(official) {}

std::shared_ptr<const NwdafTokenKeyProvider>
NwdafAccessTokenValidator::keysFromConfig(const NwdafConfig& config) {
    if (!config.oauth_nrf_public_key_file.empty())
        return std::make_shared<NwdafPemKeyProvider>(config.oauth_nrf_public_key_file);
    if (!config.oauth_shared_secret_file.empty())
        return std::make_shared<NwdafSharedSecretProvider>(config.oauth_shared_secret_file);
    throw std::runtime_error("oauth_enabled needs oauth_nrf_public_key_file or "
                             "oauth_shared_secret_file to verify access tokens");
}

NwdafTokenCheck NwdafAccessTokenValidator::check(const std::string& authorization,
                                                 const std::string& service,
                                                 const std::string& client_nf_instance_id) const {
    NwdafTokenCheck out;
    auto invalid = [&](std::string why) { out.outcome = NwdafTokenCheck::Invalid; out.reason = std::move(why); return out; };

    if (authorization.empty()) {
        out.outcome = NwdafTokenCheck::Missing;
        out.reason = "no access token";
        return out;
    }
    if (authorization.rfind("Bearer ", 0) != 0) return invalid("not a Bearer token");
    const std::string token = authorization.substr(7);

    // JWS compact serialization: header.payload.signature
    const auto d1 = token.find('.');
    const auto d2 = d1 == std::string::npos ? d1 : token.find('.', d1 + 1);
    if (d2 == std::string::npos || token.find('.', d2 + 1) != std::string::npos)
        return invalid("malformed JWS");
    std::string header_raw, payload_raw, signature;
    if (!base64UrlDecode(token.substr(0, d1), header_raw) ||
        !base64UrlDecode(token.substr(d1 + 1, d2 - d1 - 1), payload_raw) ||
        !base64UrlDecode(token.substr(d2 + 1), signature))
        return invalid("malformed JWS encoding");

    json header, claims;
    try {
        header = json::parse(header_raw);
        claims = json::parse(payload_raw);
    } catch (const json::exception&) {
        return invalid("malformed JWS JSON");
    }

    // 1. Integrity: signature with the NRF's key, or MAC with the shared secret.
    const std::string alg = header.value("alg", "");
    if (alg == "none" || alg.empty()) return invalid("unsigned token");
    if (!keys_->verify(alg, token.substr(0, d2), signature)) return invalid("signature verification failed");

    // Claims: mandatory members, then the official schema.
    for (const char* c : {"iss", "sub", "aud", "scope", "exp"})
        if (!claims.contains(c)) out.missing_claims.push_back(c);
    if (!out.missing_claims.empty()) return invalid("mandatory claim missing");
    if (!claims["exp"].is_number_integer() || !claims["scope"].is_string())
        return invalid("exp must be an integer and scope a string");
    if (official_) {
        auto v = official_->validate(claims, "TS29510_Nnrf_AccessToken.yaml#/components/schemas/AccessTokenClaims");
        if (!v.empty()) return invalid("claims do not match AccessTokenClaims: " + v.front().pointer + " " + v.front().reason);
    }

    // 2. Subject: the NF Instance ID in the consumer's TLS client certificate.
    if (!client_nf_instance_id.empty() && claims["sub"] != client_nf_instance_id)
        return invalid("sub does not match the client certificate");

    // 3. Audience: this NF instance, or the NWDAF NF type.
    const json& aud = claims["aud"];
    bool aud_ok = aud.is_string() ? (aud == "NWDAF" || aud == config_.nf_instance_id) : false;
    if (aud.is_array())
        for (const auto& a : aud) aud_ok = aud_ok || a == config_.nf_instance_id;
    if (!aud_ok) return invalid("audience does not match this NWDAF");

    // Slice / set restrictions must include what this NWDAF serves.
    if (claims.contains("producerSnssaiList")) {
        bool served = false;
        for (const auto& s : claims["producerSnssaiList"])
            served = served || (s.value("sst", -1) == config_.served_snssai_sst &&
                                (!s.contains("sd") || s["sd"] == config_.served_snssai_sd));
        if (!served) return invalid("producerSnssaiList excludes the S-NSSAI this NWDAF serves");
    }
    for (const char* c : {"producerNsiList", "producerNfSetId", "producerNfServiceSetId"})
        if (claims.contains(c)) return invalid(std::string(c) + " does not match this NWDAF (no NSI or NF set)");

    // 5. Expiry.
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (claims["exp"].get<long long>() <= now) return invalid("token expired");

    // 4. Scope: the requested service.
    std::istringstream scopes(claims["scope"].get<std::string>());
    bool in_scope = false;
    for (std::string s; scopes >> s;) in_scope = in_scope || s == service;
    if (!in_scope) {
        out.outcome = NwdafTokenCheck::InsufficientScope;
        out.reason = "scope does not include " + service;
        return out;
    }

    out.outcome = NwdafTokenCheck::Ok;
    out.claims = std::move(claims);
    return out;
}

std::string nwdafNfInstanceIdFromCert(void* x509) {
    auto* cert = static_cast<X509*>(x509);
    if (!cert) return "";
    std::string id;
    auto* names = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    for (int i = 0; names && i < sk_GENERAL_NAME_num(names); ++i) {
        const GENERAL_NAME* n = sk_GENERAL_NAME_value(names, i);
        if (n->type != GEN_URI) continue;
        const std::string uri(reinterpret_cast<const char*>(ASN1_STRING_get0_data(n->d.uniformResourceIdentifier)),
                              static_cast<size_t>(ASN1_STRING_length(n->d.uniformResourceIdentifier)));
        if (uri.rfind("urn:uuid:", 0) == 0) { id = uri.substr(9); break; }
    }
    GENERAL_NAMES_free(names);
    return id;
}
