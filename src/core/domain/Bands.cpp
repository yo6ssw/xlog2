// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "Bands.h"

namespace bands {

namespace {

struct BandRange {
    const char* name;
    double      low;   // MHz, inclusive
    double      high;  // MHz, inclusive
};

// IARU amateur allocations, low to high. Used both for the drop-down order
// and for frequency -> band lookup.
const std::vector<BandRange> kBands = {
    {"2190m",   0.1357,   0.1378},
    {"630m",    0.472,    0.479},
    {"160m",    1.8,      2.0},
    {"80m",     3.5,      4.0},
    {"60m",     5.06,     5.45},
    {"40m",     7.0,      7.3},
    {"30m",    10.1,     10.15},
    {"20m",    14.0,     14.35},
    {"17m",    18.068,   18.168},
    {"15m",    21.0,     21.45},
    {"12m",    24.89,    24.99},
    {"10m",    28.0,     29.7},
    {"6m",     50.0,     54.0},
    {"4m",     70.0,     70.5},
    {"2m",    144.0,    148.0},
    {"1.25m", 222.0,    225.0},
    {"70cm",  420.0,    450.0},
    {"33cm",  902.0,    928.0},
    {"23cm", 1240.0,   1300.0},
};

} // namespace

const std::vector<std::string>& names() {
    static const std::vector<std::string> n = [] {
        std::vector<std::string> v;
        v.reserve(kBands.size());
        for (const auto& b : kBands)
            v.emplace_back(b.name);
        return v;
    }();
    return n;
}

const std::vector<std::string>& modes() {
    static const std::vector<std::string> m = {
        "SSB", "USB", "LSB", "CW", "AM", "FM",
        "RTTY", "PSK31", "FT8", "FT4", "JT65", "JS8", "MFSK", "DIGITAL",
    };
    return m;
}

std::string forFrequencyMHz(double mhz) {
    for (const auto& b : kBands) {
        if (mhz >= b.low && mhz <= b.high)
            return b.name;
    }
    return {};
}

} // namespace bands
