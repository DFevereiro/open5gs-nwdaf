#pragma once
#include <nlohmann/json.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

// H1.7: OpenAPI 3.0 schema validation against a directory of YAML documents —
// the official 3GPP Rel-18 artifacts pinned in docs/frozen-standards.md, or
// this project's own operator-API document.
//
// Documents are loaded lazily on first reference and converted once to JSON,
// so validation walks immutable data and is safe to call from concurrent
// request handlers.
//
// Supported keywords: $ref (same-document and cross-document), type, nullable,
// enum, allOf, anyOf, oneOf (exactly one), not, required, properties,
// additionalProperties, minProperties, maxProperties, items, minItems,
// maxItems, minimum, maximum, exclusiveMinimum/Maximum (OAS 3.0 boolean form),
// minLength, maxLength, pattern. `format` is an annotation and is not asserted;
// where a format matters, the caller checks it as a prose rule.
struct SchemaViolation {
    std::string pointer;   // JSON Pointer into the validated document ("" = root)
    std::string reason;
};

class NwdafSchemaValidator {
public:
    // SEC-04: the longest string matched against a schema pattern; a longer
    // one is a violation (std::regex would recurse once per character).
    static constexpr size_t MAX_PATTERN_INPUT = 8192;

    // base_dir: directory holding the YAML documents that refs name.
    explicit NwdafSchemaValidator(std::string base_dir);

    // Validate `doc` against the schema named by `ref`, written as
    // "<file>#<json-pointer>", e.g.
    // "TS29520_Nnwdaf_EventsSubscription.yaml#/components/schemas/NnwdafEventsSubscription".
    // Throws std::runtime_error if the ref cannot be resolved.
    std::vector<SchemaViolation> validate(const nlohmann::json& doc,
                                          const std::string& ref) const;

    // The resolved schema a ref names (throws if it does not exist).
    const nlohmann::json& schema(const std::string& ref) const;

    // A whole document, converted to JSON (throws if it cannot be loaded).
    const nlohmann::json& document(const std::string& file) const;

    const std::string& baseDir() const { return base_dir_; }

private:
    struct Located {
        const nlohmann::json* node;
        std::string file;   // document the node lives in (resolves its own "#/..." refs)
    };

    Located resolve(const std::string& ref, const std::string& from_file) const;
    Located follow(const nlohmann::json& schema, const std::string& file) const;
    void check(const nlohmann::json& doc, const nlohmann::json& schema,
               const std::string& file, const std::string& ptr,
               std::vector<SchemaViolation>& out) const;
    bool matches(const nlohmann::json& doc, const nlohmann::json& schema,
                 const std::string& file, const std::string& ptr,
                 std::vector<SchemaViolation>* first_errors = nullptr) const;
    const std::regex* regexFor(const std::string& pattern) const;

    std::string base_dir_;
    mutable std::mutex mutex_;
    mutable std::map<std::string, std::unique_ptr<nlohmann::json>> docs_;
    mutable std::map<std::string, std::unique_ptr<std::regex>> regexes_;
};
