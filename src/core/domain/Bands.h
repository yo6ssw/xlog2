// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <string>
#include <vector>

// Static reference data for the entry form: the amateur bands and common
// operating modes, plus a helper to derive the band from a frequency.
namespace bands {

// Amateur band names, in the order shown in the band drop-down.
const std::vector<std::string>& names();

// Common operating modes, in the order shown in the mode drop-down.
const std::vector<std::string>& modes();

// Returns the band name for a frequency in MHz (e.g. 14.250 -> "20m"),
// or an empty string if it falls outside the known amateur allocations.
std::string forFrequencyMHz(double mhz);

}  // namespace bands
