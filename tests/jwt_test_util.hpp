#pragma once
// Test-only JWS (RFC 7515) signing: keys are generated at runtime, so no key
// material is committed. Mirrors what an NRF issuing access tokens does.
#ifdef NWDAF_USE_TLS

#include <nlohmann/json.hpp>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <chrono>
#include <cstdio>
#include <string>

namespace jwt_test {

inline std::string b64url(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    unsigned int buf = 0;
    int bits = 0;
    for (unsigned char c : in) {
        buf = (buf << 8) | c;
        bits += 8;
        while (bits >= 6) { bits -= 6; out += t[(buf >> bits) & 63]; }
    }
    if (bits > 0) out += t[(buf << (6 - bits)) & 63];
    return out;
}

// A generated signing key; its public half can be written as PEM.
struct Key {
    EVP_PKEY* pkey = nullptr;
    explicit Key(const char* type) {   // "RSA" or "EC"
        pkey = std::string(type) == "RSA" ? EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", (size_t)2048)
                                          : EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    }
    ~Key() { EVP_PKEY_free(pkey); }
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;

    void writePublicPem(const std::string& path) const {
        FILE* f = std::fopen(path.c_str(), "w");
        PEM_write_PUBKEY(f, pkey);
        std::fclose(f);
    }
    std::string sign(const std::string& input) const {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        size_t len = 0;
        EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey);
        EVP_DigestSign(ctx, nullptr, &len, reinterpret_cast<const unsigned char*>(input.data()), input.size());
        std::string sig(len, '\0');
        EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(sig.data()), &len,
                       reinterpret_cast<const unsigned char*>(input.data()), input.size());
        sig.resize(len);
        EVP_MD_CTX_free(ctx);
        if (EVP_PKEY_base_id(pkey) != EVP_PKEY_EC) return sig;
        // DER → raw R||S (RFC 7518 §3.4)
        const unsigned char* p = reinterpret_cast<const unsigned char*>(sig.data());
        ECDSA_SIG* es = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(sig.size()));
        std::string raw(64, '\0');
        BN_bn2binpad(ECDSA_SIG_get0_r(es), reinterpret_cast<unsigned char*>(raw.data()), 32);
        BN_bn2binpad(ECDSA_SIG_get0_s(es), reinterpret_cast<unsigned char*>(raw.data()) + 32, 32);
        ECDSA_SIG_free(es);
        return raw;
    }
};

inline std::string token(const std::string& alg, const nlohmann::json& claims,
                         const Key* key, const std::string& secret = "") {
    const std::string input = b64url(nlohmann::json{{"alg", alg}, {"typ", "JWT"}}.dump()) + "." +
                              b64url(claims.dump());
    std::string sig;
    if (alg == "HS256") {
        unsigned char mac[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
             reinterpret_cast<const unsigned char*>(input.data()), input.size(), mac, &len);
        sig.assign(reinterpret_cast<char*>(mac), len);
    } else if (key) {
        sig = key->sign(input);
    }
    return input + "." + b64url(sig);
}

inline long long inSeconds(long long s) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count() + s;
}

// Claims an NRF would issue to an AMF for this NWDAF's AnalyticsInfo.
inline nlohmann::json claimsFor(const std::string& nwdaf_id,
                                const std::string& scope = "nnwdaf-analyticsinfo") {
    return {{"iss", "7d3f9e3a-0000-4000-8000-00000000abcd"},
            {"sub", "5a5a5a5a-1111-4222-8333-444455556666"},
            {"aud", nwdaf_id},
            {"scope", scope},
            {"exp", inSeconds(300)}};
}

}  // namespace jwt_test

#endif  // NWDAF_USE_TLS
