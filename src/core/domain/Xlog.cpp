// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "Xlog.h"

#include "Bands.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <string>

namespace xlog {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// Trims leading/trailing ASCII whitespace (incl. a trailing CR from CRLF files).
std::string trim(const std::string& s) {
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    size_t b = 0, e = s.size();
    while (b < e && isSpace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isSpace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// "05 Jun 2026" / "5 jun 2026" -> "2026-06-05". Passes the value through
// unchanged if it is not in the expected "DD Mon YYYY" shape.
std::string dateFromXlog(const std::string& v) {
    static constexpr std::array<const char*, 12> kMonths = {
        "jan", "feb", "mar", "apr", "may", "jun",
        "jul", "aug", "sep", "oct", "nov", "dec"};

    std::string day, mon, year;
    {
        std::string tok;
        std::array<std::string*, 3> slots = {&day, &mon, &year};
        size_t slot = 0;
        for (char c : v) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!tok.empty() && slot < slots.size()) *slots[slot++] = tok;
                tok.clear();
            } else {
                tok += c;
            }
        }
        if (!tok.empty() && slot < slots.size()) *slots[slot] = tok;
    }
    if (day.empty() || mon.empty() || year.size() != 4)
        return v;

    int month = 0;
    const std::string m = toLower(mon).substr(0, 3);
    for (size_t i = 0; i < kMonths.size(); ++i)
        if (m == kMonths[i]) { month = static_cast<int>(i) + 1; break; }
    if (month == 0)
        return v;

    if (day.size() == 1)
        day = "0" + day;

    char buf[3] = {0};
    buf[0] = static_cast<char>('0' + month / 10);
    buf[1] = static_cast<char>('0' + month % 10);
    return year + "-" + std::string(buf) + "-" + day;
}

// "1432" -> "14:32"; passes shorter values through.
std::string timeFromXlog(const std::string& v) {
    if (v.size() < 4)
        return v;
    return v.substr(0, 2) + ":" + v.substr(2, 2);
}

// Maps an xlog column name to the matching Qso field. xlog's file header uses
// "GMT"/"GMTEND"; accept the GUI's "UTC"/"UTCEND" spellings too, just in case.
void setField(Qso& q, const std::string& field, const std::string& value) {
    if (value.empty())
        return;
    if (field == "date")              q.date     = dateFromXlog(value);
    else if (field == "gmt" || field == "utc")        q.time_on  = timeFromXlog(value);
    else if (field == "gmtend" || field == "utcend")  q.time_off = timeFromXlog(value);
    else if (field == "call")         q.call     = toUpper(value);
    else if (field == "band") {
        // xlog stores the frequency in MHz here; derive the band name from it.
        q.freq = value;
        q.band = bands::forFrequencyMHz(std::atof(value.c_str()));
    }
    else if (field == "mode")         q.mode     = toUpper(value);
    else if (field == "rst")          q.rst_sent = value;
    else if (field == "myrst")        q.rst_rcvd = value;
    else if (field == "qslout")       q.qsl_sent = toUpper(value);
    else if (field == "qslin")        q.qsl_rcvd = toUpper(value);
    else if (field == "power")        q.power    = value;
    else if (field == "name")         q.name     = value;
    else if (field == "qth")          q.qth      = value;
    else if (field == "locator")      q.locator  = toUpper(value);
    else if (field == "remarks")      q.comment  = value;
    // NR, AWARDS, U1, U2 have no Qso equivalent and are ignored.
}

// One header column: its normalised field name and its byte offset on the line.
struct Column {
    std::string field;
    size_t      start;
};

// Parses the header line into columns. Returns empty if it does not look like
// an xlog header (no DATE and CALL columns).
std::vector<Column> parseHeader(const std::string& line) {
    std::vector<Column> cols;
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i >= n)
            break;
        const size_t start = i;
        while (i < n && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        cols.push_back({toLower(line.substr(start, i - start)), start});
    }

    const bool hasDate = std::any_of(cols.begin(), cols.end(),
                                     [](const Column& c) { return c.field == "date"; });
    const bool hasCall = std::any_of(cols.begin(), cols.end(),
                                     [](const Column& c) { return c.field == "call"; });
    if (!hasDate || !hasCall)
        return {};
    return cols;
}

} // namespace

std::vector<Qso> parse(const std::string& text) {
    std::vector<Qso> out;

    size_t pos = 0;
    const size_t n = text.size();
    const auto nextLine = [&](std::string& line) -> bool {
        if (pos >= n)
            return false;
        const size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) {
            line = text.substr(pos);
            pos = n;
        } else {
            line = text.substr(pos, eol - pos);
            pos = eol + 1;
        }
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        return true;
    };

    // The first non-empty line is the header.
    std::vector<Column> cols;
    std::string line;
    while (nextLine(line)) {
        if (trim(line).empty())
            continue;
        cols = parseHeader(line);
        break;
    }
    if (cols.empty())
        return out;

    while (nextLine(line)) {
        if (trim(line).empty())
            continue;
        Qso q;
        for (size_t c = 0; c < cols.size(); ++c) {
            const size_t start = cols[c].start;
            if (start >= line.size())
                continue;
            const size_t end = (c + 1 < cols.size()) ? cols[c + 1].start : line.size();
            setField(q, cols[c].field, trim(line.substr(start, end - start)));
        }
        if (!q.call.empty())
            out.push_back(q);
    }

    return out;
}

} // namespace xlog
