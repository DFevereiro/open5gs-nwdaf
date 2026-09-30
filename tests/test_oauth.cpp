// H1.10 — access-token validation (TS 33.501 V18.12.0 §13.4.1.1.2 step 2;
// TS 29.510 V18.11.0 AccessTokenClaims). Tokens are signed at runtime.
#include <catch2/catch_test_macros.hpp>
#include "official_schema.hpp"

#ifdef NWDAF_USE_TLS
#include "nwdaf_oauth.hpp"
#include "jwt_test_util.hpp"
#include <fstream>

using json = nlohmann::json;
using jwt_test::Key;
using jwt_test::token;
using jwt_test::claimsFor;

static const std::string NWDAF_ID = "198e9234-b849-42a2-9f70-d9a587616c2b";

static NwdafConfig oauthConfig() {
    NwdafConfig cfg;
    cfg.nf_instance_id = NWDAF_ID;
    cfg.served_snssai_sst = 1;
    cfg.served_snssai_sd = "000001";
    return cfg;
}

// Validator trusting `key` (an RSA or EC public key written to a PEM file).
static NwdafAccessTokenValidator validatorFor(const Key& key) {
    const std::string pem = "/tmp/nwdaf_oauth_test_pub.pem";
    key.writePublicPem(pem);
    return NwdafAccessTokenValidator(oauthConfig(), std::make_shared<NwdafPemKeyProvider>(pem), &officialSchemas());
}

static NwdafTokenCheck::Outcome check(const NwdafAccessTokenValidator& v, const std::string& tok,
                                      const std::string& cert_id = "") {
    return v.check("Bearer " + tok, "nnwdaf-analyticsinfo", cert_id).outcome;
}

TEST_CASE("H1.10: a valid RS256 token is accepted and its claims returned") {
    Key rsa("RSA");
    auto v = validatorFor(rsa);
    auto r = v.check("Bearer " + token("RS256", claimsFor(NWDAF_ID), &rsa), "nnwdaf-analyticsinfo", "");
    REQUIRE(r.outcome == NwdafTokenCheck::Ok);
    REQUIRE(r.claims["aud"] == NWDAF_ID);
}

TEST_CASE("H1.10: ES256 (raw R||S) and HS256 (shared secret) are supported") {
    Key ec("EC");
    REQUIRE(check(validatorFor(ec), token("ES256", claimsFor(NWDAF_ID), &ec)) == NwdafTokenCheck::Ok);

    const std::string secret_file = "/tmp/nwdaf_oauth_test_secret";
    std::ofstream(secret_file) << "shared-with-the-nrf\n";
    NwdafAccessTokenValidator hs(oauthConfig(), std::make_shared<NwdafSharedSecretProvider>(secret_file));
    REQUIRE(check(hs, token("HS256", claimsFor(NWDAF_ID), nullptr, "shared-with-the-nrf")) == NwdafTokenCheck::Ok);
    REQUIRE(check(hs, token("HS256", claimsFor(NWDAF_ID), nullptr, "wrong-secret")) == NwdafTokenCheck::Invalid);
}

TEST_CASE("H1.10: no token, a non-Bearer header, and malformed tokens") {
    Key rsa("RSA");
    auto v = validatorFor(rsa);
    REQUIRE(v.check("", "nnwdaf-analyticsinfo", "").outcome == NwdafTokenCheck::Missing);
    REQUIRE(v.check("Basic Zm9vOmJhcg==", "nnwdaf-analyticsinfo", "").outcome == NwdafTokenCheck::Invalid);
    REQUIRE(check(v, "not-a-jws") == NwdafTokenCheck::Invalid);
}

TEST_CASE("H1.10: integrity — tampered, unsigned, or signed by another key") {
    Key rsa("RSA"), other("RSA");
    auto v = validatorFor(rsa);
    std::string tok = token("RS256", claimsFor(NWDAF_ID), &rsa);
    // Swap the payload for one granting a wider scope, keeping the signature.
    const auto d1 = tok.find('.'), d2 = tok.rfind('.');
    const std::string forged = tok.substr(0, d1 + 1) +
        jwt_test::b64url(claimsFor(NWDAF_ID, "nnwdaf-analyticsinfo nnwdaf-eventssubscription").dump()) +
        tok.substr(d2);
    REQUIRE(check(v, forged) == NwdafTokenCheck::Invalid);
    REQUIRE(check(v, token("none", claimsFor(NWDAF_ID), nullptr)) == NwdafTokenCheck::Invalid);
    REQUIRE(check(v, token("RS256", claimsFor(NWDAF_ID), &other)) == NwdafTokenCheck::Invalid);
}

TEST_CASE("H1.10: expiry and audience") {
    Key rsa("RSA");
    auto v = validatorFor(rsa);
    json c = claimsFor(NWDAF_ID);
    c["exp"] = jwt_test::inSeconds(-1);
    REQUIRE(check(v, token("RS256", c, &rsa)) == NwdafTokenCheck::Invalid);

    c = claimsFor(NWDAF_ID);
    c["aud"] = "AMF";                               // another NF type
    REQUIRE(check(v, token("RS256", c, &rsa)) == NwdafTokenCheck::Invalid);
    c["aud"] = "NWDAF";                             // this NF type
    REQUIRE(check(v, token("RS256", c, &rsa)) == NwdafTokenCheck::Ok);
    c["aud"] = json::array({"11111111-1111-4111-8111-111111111111", NWDAF_ID});
    REQUIRE(check(v, token("RS256", c, &rsa)) == NwdafTokenCheck::Ok);
}

TEST_CASE("H1.10: missing mandatory claims are reported by name") {
    Key rsa("RSA");
    auto v = validatorFor(rsa);
    json c = claimsFor(NWDAF_ID);
    c.erase("scope");
    auto r = v.check("Bearer " + token("RS256", c, &rsa), "nnwdaf-analyticsinfo", "");
    REQUIRE(r.outcome == NwdafTokenCheck::Invalid);
    REQUIRE(r.missing_claims == std::vector<std::string>{"scope"});
}

TEST_CASE("H1.10: claims are checked against the official AccessTokenClaims schema") {
    Key rsa("RSA");
    auto v = validatorFor(rsa);
    json c = claimsFor(NWDAF_ID);
    c["producerPlmnId"] = {{"mcc", "not-a-mcc"}, {"mnc", "70"}};
    REQUIRE(check(v, token("RS256", c, &rsa)) == NwdafTokenCheck::Invalid);
}

TEST_CASE("H1.10: scope must include the requested service") {
    Key rsa("RSA");
    auto v = validatorFor(rsa);
    REQUIRE(check(v, token("RS256", claimsFor(NWDAF_ID, "nnwdaf-eventssubscription"), &rsa)) ==
            NwdafTokenCheck::InsufficientScope);
    REQUIRE(check(v, token("RS256", claimsFor(NWDAF_ID, "nnrf-disc nnwdaf-analyticsinfo"), &rsa)) ==
            NwdafTokenCheck::Ok);
}

TEST_CASE("H1.10: slice and set restrictions must match what this NWDAF serves") {
    Key rsa("RSA");
    auto v = validatorFor(rsa);
    json c = claimsFor(NWDAF_ID);
    c["producerSnssaiList"] = json::array({{{"sst", 2}}});
    REQUIRE(check(v, token("RS256", c, &rsa)) == NwdafTokenCheck::Invalid);
    c["producerSnssaiList"] = json::array({{{"sst", 1}, {"sd", "000001"}}});
    REQUIRE(check(v, token("RS256", c, &rsa)) == NwdafTokenCheck::Ok);

    c = claimsFor(NWDAF_ID);
    c["producerNfSetId"] = "set1.nwdafset.5gc.mnc070.mcc999";
    REQUIRE(check(v, token("RS256", c, &rsa)) == NwdafTokenCheck::Invalid);
}

TEST_CASE("H1.10: sub must match the NF Instance ID of the client certificate") {
    Key rsa("RSA");
    auto v = validatorFor(rsa);
    const std::string tok = token("RS256", claimsFor(NWDAF_ID), &rsa);
    REQUIRE(check(v, tok, "5a5a5a5a-1111-4222-8333-444455556666") == NwdafTokenCheck::Ok);
    REQUIRE(check(v, tok, "99999999-9999-4999-8999-999999999999") == NwdafTokenCheck::Invalid);
}

TEST_CASE("H1.10: OAuth without a verification key fails closed") {
    NwdafConfig cfg = oauthConfig();
    cfg.oauth_enabled = true;
    REQUIRE_THROWS(NwdafAccessTokenValidator::keysFromConfig(cfg));
    cfg.oauth_nrf_public_key_file = "/nonexistent/nrf.pem";
    REQUIRE_THROWS(NwdafAccessTokenValidator::keysFromConfig(cfg));
}

#endif  // NWDAF_USE_TLS
