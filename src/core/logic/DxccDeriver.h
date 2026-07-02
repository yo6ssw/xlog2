// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <string>

// Derives the DXCC entity/zone fields shown next to the callsign, and formats
// them for display. Wraps dxcc::lookup (cty.dat) with the UI's fallback rule:
// when no country file matches, keep whatever the loaded record carried so
// editing a QSO never silently drops imported DXCC data.
namespace dxccderive {

// DXCC fields as text, matching the Qso member representation.
struct Fields {
  std::string country;    // entity name, e.g. "Germany"
  std::string cq_zone;    // CQ zone number as text
  std::string itu_zone;   // ITU zone number as text
  std::string continent;  // "EU", "NA", …
};

// Resolve `call` against cty.dat. On a match, return the looked-up fields; on
// no match (or no country file loaded), return `fallback`. An empty call yields
// the fallback too.
Fields derive(const std::string& call, const Fields& fallback);

// One-line summary, e.g. "Germany  ·  CQ 14  ·  ITU 28  ·  EU". Empty when the
// country is unknown.
std::string format(const Fields& f);

}  // namespace dxccderive
