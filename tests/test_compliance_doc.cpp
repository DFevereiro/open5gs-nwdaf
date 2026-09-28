// M1 — every "Compliant" row of docs/3gpp-rel18-compliance.md must be backed by
// a test that exists: it cites at least one tests/*.cpp file, and every test
// name it quotes is a TEST_CASE in the test sources (a trailing "…" = prefix).
// That the cited tests pass is proven by ctest in the same CI run.
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static const fs::path ROOT = NWDAF_SOURCE_DIR;

static std::string readAll(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::set<std::string> testCaseNames() {
    std::set<std::string> names;
    static const std::regex tc(R"re(TEST_CASE\("((?:[^"\\]|\\.)*)")re");
    for (const auto& e : fs::directory_iterator(ROOT / "tests")) {
        if (e.path().extension() != ".cpp") continue;
        const std::string src = readAll(e.path());
        for (std::sregex_iterator it(src.begin(), src.end(), tc), end; it != end; ++it)
            names.insert((*it)[1]);
    }
    return names;
}

static std::vector<std::string> cells(const std::string& row) {
    std::vector<std::string> out;
    std::string cur;
    bool in_code = false;
    for (size_t i = 1; i < row.size(); ++i) {   // skip the leading '|'
        const char c = row[i];
        if (c == '`') in_code = !in_code;
        if (c == '|' && !in_code) { out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    return out;
}

TEST_CASE("M1: every Compliant row of the compliance doc cites existing tests") {
    const auto names = testCaseNames();
    REQUIRE_FALSE(names.empty());
    std::istringstream doc(readAll(ROOT / "docs" / "3gpp-rel18-compliance.md"));
    static const std::regex file_ref(R"(tests/([A-Za-z0-9_]+\.cpp))");
    static const std::regex quoted("\"([^\"]+)\"");
    static const std::regex test_like(R"(^(H1\.\d+|M1|Integration|ARCH-\d+|COMP-\d+|BUG-\d+|PROD-\d+): )");
    const std::string ellipsis = "…";

    int compliant_rows = 0;
    for (std::string line; std::getline(doc, line);) {
        if (line.rfind("| ", 0) != 0) continue;
        const auto c = cells(line);
        // Capability | Code | 3GPP reference | Current status | …
        if (c.size() < 4 || c[3].find("Compliant") == std::string::npos ||
            c[3].find("Partially") != std::string::npos || c[3].find("Non-compliant") != std::string::npos)
            continue;
        ++compliant_rows;
        INFO("row: " << c[0]);
        const std::string& code = c[1];

        bool cites_file = false;
        for (std::sregex_iterator it(code.begin(), code.end(), file_ref), end; it != end; ++it) {
            INFO("cited file: " << (*it)[0]);
            CHECK(fs::exists(ROOT / "tests" / std::string((*it)[1])));
            cites_file = true;
        }
        CHECK(cites_file);

        for (std::sregex_iterator it(code.begin(), code.end(), quoted), end; it != end; ++it) {
            std::string name = (*it)[1];
            if (!std::regex_search(name, test_like)) continue;
            INFO("cited test: " << name);
            const bool prefix = name.size() >= ellipsis.size() &&
                                name.compare(name.size() - ellipsis.size(), ellipsis.size(), ellipsis) == 0;
            if (prefix) {
                name.resize(name.size() - ellipsis.size());
                while (!name.empty() && name.back() == ' ') name.pop_back();
                bool found = false;
                for (const auto& n : names) found = found || n.rfind(name, 0) == 0;
                CHECK(found);
            } else {
                CHECK(names.count(name) == 1);
            }
        }
    }
    REQUIRE(compliant_rows > 0);
}
