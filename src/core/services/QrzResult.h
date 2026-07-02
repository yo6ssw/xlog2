// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <string>
#include <utility>
#include <vector>

// The interesting fields of a QRZ.com callsign record, mapped to the parts of
// a Qso we can prefill.
struct QrzResult {
    std::string call;
    std::string name;     // "First Last"
    std::string qth;      // "City, State"
    std::string locator;  // Maidenhead grid
    std::string country;

    // Every element QRZ returned for the callsign, in document order, for
    // display in the lookup popup (the named fields above are derived from it).
    std::vector<std::pair<std::string, std::string>> fields;
};
