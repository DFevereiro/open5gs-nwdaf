#include "nwdaf_schema_validator.hpp"
#include <yaml-cpp/yaml.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

using json = nlohmann::json;

namespace {

// YAML → JSON. Quoted scalars (tag "!") stay strings; plain scalars are typed
// the way the OpenAPI documents rely on (booleans, integers, numbers, null).
json toJson(const YAML::Node& n) {
    switch (n.Type()) {
    case YAML::NodeType::Sequence: {
        json arr = json::array();
        for (const auto& item : n) arr.push_back(toJson(item));
        return arr;
    }
    case YAML::NodeType::Map: {
        json obj = json::object();
        for (auto it = n.begin(); it != n.end(); ++it)
            obj[it->first.as<std::string>()] = toJson(it->second);
        return obj;
    }
    case YAML::NodeType::Scalar: {
        const std::string& s = n.Scalar();
        if (n.Tag() == "!") return s;
        if (s == "true")  return true;
        if (s == "false") return false;
        if (s == "null" || s == "~") return nullptr;
        static const std::regex integer(R"(^[-+]?[0-9]+$)");
        static const std::regex number(R"(^[-+]?([0-9]+\.[0-9]*|\.[0-9]+|[0-9]+)([eE][-+]?[0-9]+)?$)");
        try {
            if (std::regex_match(s, integer)) return std::stoll(s);
            if (std::regex_match(s, number))  return std::stod(s);
        } catch (const std::out_of_range&) {}
        return s;
    }
    default:
        return nullptr;
    }
}

std::string typeName(const json& v) {
    if (v.is_object())  return "object";
    if (v.is_array())   return "array";
    if (v.is_string())  return "string";
    if (v.is_boolean()) return "boolean";
    if (v.is_number_integer() || v.is_number_unsigned()) return "integer";
    if (v.is_number())  return "number";
    return "null";
}

// JSON Pointer reference token escaping (RFC 6901).
std::string escapeToken(const std::string& key) {
    std::string out;
    for (char c : key) {
        if (c == '~')      out += "~0";
        else if (c == '/') out += "~1";
        else               out += c;
    }
    return out;
}

size_t codepoints(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

}  // namespace

NwdafSchemaValidator::NwdafSchemaValidator(std::string base_dir)
    : base_dir_(std::move(base_dir)) {}

const json& NwdafSchemaValidator::document(const std::string& file) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = docs_.find(file);
    if (it != docs_.end()) return *it->second;
    YAML::Node root;
    try {
        root = YAML::LoadFile(base_dir_ + "/" + file);
    } catch (const std::exception& e) {
        throw std::runtime_error("cannot load OpenAPI document " + base_dir_ + "/" +
                                 file + ": " + e.what());
    }
    auto doc = std::make_unique<json>(toJson(root));
    const json& ref = *doc;
    docs_.emplace(file, std::move(doc));
    return ref;
}

NwdafSchemaValidator::Located
NwdafSchemaValidator::resolve(const std::string& ref, const std::string& from_file) const {
    // "<file>#<pointer>"; an empty file part means the referring document.
    const auto hash = ref.find('#');
    std::string file = ref.substr(0, hash);
    if (file.empty()) file = from_file;
    const std::string pointer = hash == std::string::npos ? "" : ref.substr(hash + 1);
    const json& doc = document(file);
    try {
        return {&doc.at(json::json_pointer(pointer)), file};
    } catch (const json::exception&) {
        throw std::runtime_error("unresolvable $ref '" + ref + "' (from " + from_file + ")");
    }
}

NwdafSchemaValidator::Located
NwdafSchemaValidator::follow(const json& schema, const std::string& file) const {
    Located at{&schema, file};
    for (int hops = 0; at.node->is_object() && at.node->contains("$ref"); ++hops) {
        if (hops == 32) throw std::runtime_error("$ref chain too deep in " + file);
        at = resolve((*at.node)["$ref"].get<std::string>(), at.file);
    }
    return at;
}

const json& NwdafSchemaValidator::schema(const std::string& ref) const {
    const Located at = resolve(ref, "");
    return *follow(*at.node, at.file).node;
}

std::vector<SchemaViolation>
NwdafSchemaValidator::validate(const json& doc, const std::string& ref) const {
    const Located root = resolve(ref, "");
    std::vector<SchemaViolation> out;
    check(doc, *root.node, root.file, "", out);
    return out;
}

const std::regex* NwdafSchemaValidator::regexFor(const std::string& pattern) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = regexes_.find(pattern);
    if (it != regexes_.end()) return it->second.get();
    std::unique_ptr<std::regex> re;
    try {
        re = std::make_unique<std::regex>(pattern, std::regex::ECMAScript);
    } catch (const std::regex_error&) {
        // A pattern std::regex cannot compile is not asserted, rather than
        // rejecting input the specification allows.
        spdlog::debug("schema pattern not supported by std::regex, not asserted: {}", pattern);
    }
    const std::regex* p = re.get();
    regexes_.emplace(pattern, std::move(re));
    return p;
}

bool NwdafSchemaValidator::matches(const json& doc, const json& schema,
                                   const std::string& file, const std::string& ptr,
                                   std::vector<SchemaViolation>* first_errors) const {
    std::vector<SchemaViolation> errs;
    check(doc, schema, file, ptr, errs);
    if (!errs.empty() && first_errors && first_errors->empty()) *first_errors = errs;
    return errs.empty();
}

void NwdafSchemaValidator::check(const json& doc, const json& raw,
                                 const std::string& raw_file, const std::string& ptr,
                                 std::vector<SchemaViolation>& out) const {
    const Located at = follow(raw, raw_file);
    const json& s = *at.node;
    const std::string& file = at.file;
    if (!s.is_object()) return;

    if (doc.is_null() && s.value("nullable", false)) return;

    if (s.contains("type")) {
        const std::string want = s["type"].get<std::string>();
        const std::string got  = typeName(doc);
        if (want != got && !(want == "number" && got == "integer")) {
            out.push_back({ptr, "expected " + want + ", got " + got});
            return;   // further checks would be meaningless
        }
    }

    if (s.contains("enum") && s["enum"].is_array()) {
        bool found = false;
        for (const auto& e : s["enum"]) if (e == doc) { found = true; break; }
        if (!found) out.push_back({ptr, "value " + doc.dump() + " is not in the enumeration"});
    }

    if (doc.is_number()) {
        const double v = doc.get<double>();
        if (s.contains("minimum")) {
            const double m = s["minimum"].get<double>();
            if (s.value("exclusiveMinimum", false) ? v <= m : v < m)
                out.push_back({ptr, "below the minimum " + s["minimum"].dump()});
        }
        if (s.contains("maximum")) {
            const double m = s["maximum"].get<double>();
            if (s.value("exclusiveMaximum", false) ? v >= m : v > m)
                out.push_back({ptr, "above the maximum " + s["maximum"].dump()});
        }
    }

    if (doc.is_string()) {
        const std::string& v = doc.get_ref<const std::string&>();
        if (s.contains("minLength") && codepoints(v) < s["minLength"].get<size_t>())
            out.push_back({ptr, "shorter than minLength " + s["minLength"].dump()});
        if (s.contains("maxLength") && codepoints(v) > s["maxLength"].get<size_t>())
            out.push_back({ptr, "longer than maxLength " + s["maxLength"].dump()});
        if (s.contains("pattern")) {
            const std::regex* re = regexFor(s["pattern"].get<std::string>());
            if (re && !std::regex_search(v, *re))
                out.push_back({ptr, "does not match the pattern " + s["pattern"].get<std::string>()});
        }
    }

    if (doc.is_object()) {
        if (s.contains("required"))
            for (const auto& r : s["required"])
                if (!doc.contains(r.get<std::string>()))
                    out.push_back({ptr + "/" + escapeToken(r.get<std::string>()),
                                   "mandatory attribute is missing"});
        const json* props = s.contains("properties") ? &s["properties"] : nullptr;
        for (auto it = doc.begin(); it != doc.end(); ++it) {
            const std::string child = ptr + "/" + escapeToken(it.key());
            if (props && props->contains(it.key())) {
                check(it.value(), (*props)[it.key()], file, child, out);
            } else if (s.contains("additionalProperties")) {
                const json& ap = s["additionalProperties"];
                if (ap.is_boolean() && !ap.get<bool>())
                    out.push_back({child, "attribute is not allowed here"});
                else if (ap.is_object())
                    check(it.value(), ap, file, child, out);
            }
        }
        if (s.contains("minProperties") && doc.size() < s["minProperties"].get<size_t>())
            out.push_back({ptr, "fewer than minProperties " + s["minProperties"].dump()});
        if (s.contains("maxProperties") && doc.size() > s["maxProperties"].get<size_t>())
            out.push_back({ptr, "more than maxProperties " + s["maxProperties"].dump()});
    }

    if (doc.is_array()) {
        if (s.contains("items"))
            for (size_t i = 0; i < doc.size(); ++i)
                check(doc[i], s["items"], file, ptr + "/" + std::to_string(i), out);
        if (s.contains("minItems") && doc.size() < s["minItems"].get<size_t>())
            out.push_back({ptr, "fewer than minItems " + s["minItems"].dump()});
        if (s.contains("maxItems") && doc.size() > s["maxItems"].get<size_t>())
            out.push_back({ptr, "more than maxItems " + s["maxItems"].dump()});
    }

    if (s.contains("allOf"))
        for (const auto& branch : s["allOf"]) check(doc, branch, file, ptr, out);

    if (s.contains("anyOf")) {
        std::vector<SchemaViolation> first;
        bool any = false;
        for (const auto& branch : s["anyOf"])
            if (matches(doc, branch, file, ptr, &first)) { any = true; break; }
        if (!any)
            out.push_back({ptr, "matches none of the anyOf alternatives" +
                                (first.empty() ? std::string() :
                                 " (first: " + first.front().pointer + " " + first.front().reason + ")")});
    }

    if (s.contains("oneOf")) {
        std::vector<SchemaViolation> first;
        int hits = 0;
        for (const auto& branch : s["oneOf"])
            if (matches(doc, branch, file, ptr, &first)) ++hits;
        if (hits == 0)
            out.push_back({ptr, "matches none of the oneOf alternatives" +
                                (first.empty() ? std::string() :
                                 " (first: " + first.front().pointer + " " + first.front().reason + ")")});
        else if (hits > 1)
            out.push_back({ptr, "matches more than one oneOf alternative"});
    }

    if (s.contains("not") && matches(doc, s["not"], file, ptr))
        out.push_back({ptr, "matches a schema it must not match"});
}
