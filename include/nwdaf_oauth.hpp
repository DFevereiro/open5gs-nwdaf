#pragma once
#ifdef NWDAF_USE_TLS

#include "nwdaf_config.hpp"
#include "nwdaf_schema_validator.hpp"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>

// H1.10: OAuth 2.0 access-token validation for the 3GPP interfaces.
//
// Standards logic (NwdafAccessTokenValidator) follows TS 33.501 V18.12.0
// §13.4.1.1.2 step 2 and the TS 29.510 V18.11.0 AccessTokenClaims. Where the
// verification key comes from is a deployment mechanism, not a 3GPP one
// (NwdafTokenKeyProvider): the initial providers read a configured NRF public
// key or a pre-shared secret.

// Verifies a JWS signature (RFC 7515) with a key obtained out of band.
class NwdafTokenKeyProvider {
public:
    virtual ~NwdafTokenKeyProvider() = default;
    // `signing_input` is "<b64url header>.<b64url payload>"; `signature` is
    // the decoded JWS signature.
    virtual bool verify(const std::string& alg, const std::string& signing_input,
                        const std::string& signature) const = 0;
};

// The NRF's public key from a PEM file: RS256 (RSA) or ES256 (P-256).
class NwdafPemKeyProvider : public NwdafTokenKeyProvider {
public:
    explicit NwdafPemKeyProvider(const std::string& pem_file);   // throws if unreadable
    ~NwdafPemKeyProvider() override;
    bool verify(const std::string& alg, const std::string& signing_input,
                const std::string& signature) const override;
private:
    void* key_ = nullptr;   // EVP_PKEY*
};

// A secret shared with the NRF: HS256 (TS 33.501 §13.4.1.0 NOTE 1a).
class NwdafSharedSecretProvider : public NwdafTokenKeyProvider {
public:
    explicit NwdafSharedSecretProvider(const std::string& secret_file);   // throws if unreadable
    bool verify(const std::string& alg, const std::string& signing_input,
                const std::string& signature) const override;
private:
    std::string secret_;
};

struct NwdafTokenCheck {
    enum Outcome { Ok, Missing, Invalid, InsufficientScope } outcome = Invalid;
    std::string reason;
    std::vector<std::string> missing_claims;   // for ACCESS_TOKEN_CLAIM_MISSING
    nlohmann::json claims;                     // when Ok
};

class NwdafAccessTokenValidator {
public:
    // `official` validates the claims against the TS 29.510 AccessTokenClaims
    // schema when it has the pinned artifacts; may be null.
    NwdafAccessTokenValidator(const NwdafConfig& config,
                              std::shared_ptr<const NwdafTokenKeyProvider> keys,
                              const NwdafSchemaValidator* official = nullptr);

    // `authorization`: the Authorization header ("" when absent).
    // `service`: the requested service's scope, e.g. "nnwdaf-analyticsinfo".
    // `client_nf_instance_id`: the NF Instance ID from the consumer's TLS
    // client certificate, "" when there is none.
    NwdafTokenCheck check(const std::string& authorization, const std::string& service,
                          const std::string& client_nf_instance_id) const;

    // The key provider the configuration selects; throws when OAuth is
    // enabled without one (fail closed).
    static std::shared_ptr<const NwdafTokenKeyProvider> keysFromConfig(const NwdafConfig& config);

private:
    NwdafConfig config_;
    std::shared_ptr<const NwdafTokenKeyProvider> keys_;
    const NwdafSchemaValidator* official_;
};

// NF Instance ID from an NF certificate's subjectAltName URI "urn:uuid:<id>";
// "" when absent. `x509` is an X509*. The URI form follows the TS 33.310 NF
// certificate profile, which is outside the frozen baseline — Requires
// verification (docs/3gpp-rel18-compliance.md).
std::string nwdafNfInstanceIdFromCert(void* x509);

#endif  // NWDAF_USE_TLS
