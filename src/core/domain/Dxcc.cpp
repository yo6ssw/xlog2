// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "Dxcc.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace dxcc {
namespace {

std::unordered_map<std::string, Info> g_prefixes;  // prefix       -> info
std::unordered_map<std::string, Info> g_exact;     // exact call   -> info
bool g_loaded   = false;
bool g_attempted = false;  // tried the default path already

std::string upper(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

// Parse one prefix/callsign token (with its (cq)/[itu]/{cont}/<latlon>/~utc~
// overrides) and add it to the right table.
void addToken(std::string token, const Info& entityDefaults) {
    token = trim(token);
    if (token.empty())
        return;

    Info info = entityDefaults;
    bool exact = false;
    size_t i = 0;
    if (token[0] == '=') { exact = true; i = 1; }

    std::string base;
    for (; i < token.size(); ++i) {
        const char c = token[i];
        if (c == '(' || c == '[' || c == '<' || c == '{' || c == '~')
            break;
        base += c;
    }
    for (; i < token.size();) {
        const char open = token[i];
        const char close = open == '(' ? ')' : open == '[' ? ']'
                         : open == '<' ? '>' : open == '{' ? '}'
                         : open == '~' ? '~' : '\0';
        if (close == '\0') { ++i; continue; }
        const size_t j = token.find(close, i + 1);
        if (j == std::string::npos)
            break;
        const std::string inner = token.substr(i + 1, j - i - 1);
        if (open == '(')      info.cqZone   = std::atoi(inner.c_str());
        else if (open == '[') info.ituZone  = std::atoi(inner.c_str());
        else if (open == '{') info.continent = inner;
        // <latlon> and ~utc~ are ignored.
        i = j + 1;
    }

    if (base.empty())
        return;
    (exact ? g_exact : g_prefixes)[base] = info;
}

// Letters trailing the last digit ("W1AW"->2, "VE3"->0, "DL"->0 since no digit).
int suffixLen(const std::string& t) {
    int lastDigit = -1;
    for (int i = 0; i < static_cast<int>(t.size()); ++i)
        if (std::isdigit(static_cast<unsigned char>(t[i])))
            lastDigit = i;
    if (lastDigit < 0)
        return 0;  // a bare prefix such as "DL" or "F"
    int n = 0;
    for (int i = lastDigit + 1; i < static_cast<int>(t.size()); ++i)
        if (std::isalpha(static_cast<unsigned char>(t[i])))
            ++n;
    return n;
}

bool isModifier(const std::string& t) {
    if (t.empty())
        return false;
    bool allDigits = true;
    for (char c : t)
        if (!std::isdigit(static_cast<unsigned char>(c))) { allDigits = false; break; }
    if (allDigits)
        return true;  // e.g. "/7" US call-area change — same entity
    return t == "P" || t == "M" || t == "QRP" || t == "A" || t == "LH" ||
           t == "B" || t == "BCN" || t == "R";
}

// Reduce a (possibly portable) call to the single token that determines its
// DXCC entity. Returns "" for maritime/aeronautical mobile (no entity).
std::string resolvePortable(const std::string& call) {
    const std::vector<std::string> tokens = split(call, '/');
    if (tokens.size() == 1)
        return tokens[0];

    std::vector<std::string> kept;
    for (const auto& t : tokens) {
        if (t == "MM" || t == "AM")
            return {};  // maritime / aeronautical mobile -> no DXCC
        if (isModifier(t))
            continue;
        kept.push_back(t);
    }
    if (kept.empty())
        return tokens[0];
    if (kept.size() == 1)
        return kept[0];

    // The location prefix is the part with the fewest letters after its last
    // digit (DL, VE3, VP2E …), beating the operator's home call (W1AW). Ties
    // break toward the shorter, then the earlier token.
    const std::string* best = &kept[0];
    for (const auto& t : kept) {
        const int s = suffixLen(t), bs = suffixLen(*best);
        if (s < bs || (s == bs && t.size() < best->size()))
            best = &t;
    }
    return *best;
}

// Candidate cty.dat locations, most-specific first: the user's own copy under
// XDG_DATA_HOME wins, then common system-wide installs from other ham packages.
std::vector<std::string> candidateCtyPaths() {
    std::vector<std::string> paths;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x)
        paths.push_back(std::string(x) + "/xlog2/cty.dat");
    if (const char* h = std::getenv("HOME"); h && *h)
        paths.push_back(std::string(h) + "/.local/share/xlog2/cty.dat");
    paths.push_back("/usr/share/hamradio-files/cty.dat");
    paths.push_back("/usr/share/xlog/dxcc/cty.dat");
    paths.push_back("/usr/share/tlf/cty.dat");
    return paths;
}

void ensureLoaded() {
    if (g_loaded || g_attempted)
        return;
    g_attempted = true;
    for (const auto& path : candidateCtyPaths())
        if (loadFile(path))
            return;
}

}  // namespace

void loadFromString(const std::string& ctyText) {
    g_prefixes.clear();
    g_exact.clear();

    Info cur;
    bool haveEntity = false;
    std::istringstream in(ctyText);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        if (!std::isspace(static_cast<unsigned char>(line[0]))) {
            // Entity header: Name:CQ:ITU:Cont:Lat:Lon:UTC:PrimaryPrefix:
            const std::vector<std::string> f = split(line, ':');
            if (f.size() >= 4) {
                cur = Info{};
                cur.entity    = trim(f[0]);
                cur.cqZone    = std::atoi(trim(f[1]).c_str());
                cur.ituZone   = std::atoi(trim(f[2]).c_str());
                cur.continent = trim(f[3]);
                haveEntity    = true;
            }
        } else if (haveEntity) {
            // Continuation: comma-separated prefixes, list ends at ';'.
            for (const auto& tok : split(line, ','))
                addToken(split(tok, ';')[0], cur);
        }
    }
    g_loaded = !g_prefixes.empty() || !g_exact.empty();
}

bool loadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    loadFromString(ss.str());
    return g_loaded;
}

bool available() {
    ensureLoaded();
    return g_loaded;
}

const Info* lookup(const std::string& call) {
    ensureLoaded();
    if (!g_loaded)
        return nullptr;

    const std::string c = upper(trim(call));
    if (c.empty())
        return nullptr;

    if (auto it = g_exact.find(c); it != g_exact.end())
        return &it->second;

    const std::string key = resolvePortable(c);
    if (key.empty())
        return nullptr;

    for (size_t len = key.size(); len > 0; --len) {
        auto it = g_prefixes.find(key.substr(0, len));
        if (it != g_prefixes.end())
            return &it->second;
    }
    return nullptr;
}

}  // namespace dxcc
