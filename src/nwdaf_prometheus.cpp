#include "nwdaf_prometheus.hpp"
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <optional>
#include <sstream>

namespace {

bool nameChar(char c, bool first) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == ':' ||
           (!first && std::isdigit(static_cast<unsigned char>(c)));
}

// One sample line: name[{label="value",...}] value [timestamp]
std::optional<NwdafPromSample> parseLine(const std::string& line) {
    size_t i = 0;
    const size_t n = line.size();
    auto skipSpace = [&] { while (i < n && (line[i] == ' ' || line[i] == '\t')) ++i; };

    NwdafPromSample s;
    skipSpace();
    if (i >= n || !nameChar(line[i], true)) return std::nullopt;
    while (i < n && nameChar(line[i], false)) s.name += line[i++];
    skipSpace();

    if (i < n && line[i] == '{') {
        ++i;
        for (;;) {
            skipSpace();
            if (i < n && line[i] == '}') { ++i; break; }
            std::string key;
            while (i < n && nameChar(line[i], key.empty())) key += line[i++];
            skipSpace();
            if (key.empty() || i + 1 >= n || line[i] != '=' || line[i + 1] != '"') return std::nullopt;
            i += 2;
            std::string value;
            for (;;) {
                if (i >= n) return std::nullopt;
                const char c = line[i++];
                if (c == '"') break;
                if (c != '\\') { value += c; continue; }
                if (i >= n) return std::nullopt;
                const char e = line[i++];
                value += e == 'n' ? '\n' : e;   // \\, \" and \n
            }
            s.labels[key] = value;
            skipSpace();
            if (i < n && line[i] == ',') { ++i; continue; }
            if (i < n && line[i] == '}') { ++i; break; }
            return std::nullopt;
        }
        skipSpace();
    }

    size_t end = i;
    while (end < n && line[end] != ' ' && line[end] != '\t') ++end;
    const std::string token = line.substr(i, end - i);
    if (token.empty()) return std::nullopt;
    errno = 0;
    char* stop = nullptr;
    s.value = std::strtod(token.c_str(), &stop);   // also +Inf, -Inf, NaN
    if (errno == ERANGE || stop != token.c_str() + token.size()) return std::nullopt;
    return s;
}

}  // namespace

std::vector<NwdafPromSample> NwdafPrometheusText::parse(const std::string& text) {
    std::vector<NwdafPromSample> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        if (auto s = parseLine(line)) out.push_back(std::move(*s));
    }
    return out;
}
