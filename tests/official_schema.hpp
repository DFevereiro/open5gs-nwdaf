#pragma once
// The official Rel-18 OpenAPI artifacts (docs/frozen-standards.md), loaded
// once per test binary, and a check that a JSON value matches one of their
// schemas. `ref` is "<file>#/components/schemas/<Name>".
#include "nwdaf_schema_validator.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>

inline NwdafSchemaValidator& officialSchemas() {
    static NwdafSchemaValidator v(NWDAF_3GPP_OPENAPI_DIR);
    return v;
}

inline void requireOfficialSchema(const nlohmann::json& value, const std::string& ref) {
    const auto v = officialSchemas().validate(value, ref);
    INFO(ref << ": " << value.dump(2));
    INFO((v.empty() ? std::string() : v.front().pointer + " " + v.front().reason));
    REQUIRE(v.empty());
}
