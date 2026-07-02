// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <string>

// Resolves a callsign to its DXCC entity, CQ/ITU zones and continent using the
// AD1C country file (cty.dat / "Big CTY"), the de-facto standard prefix table
// used by contest/logging software. The file is loaded lazily on first lookup
// from $XDG_DATA_HOME/xlog2/cty.dat (drop the file there); if it's absent every
// lookup simply returns nullptr and the feature stays dormant.
namespace dxcc {

struct Info {
  std::string entity;     // e.g. "Germany"
  std::string continent;  // "EU", "NA", …
  int cqZone = 0;
  int ituZone = 0;
};

// Parse cty.dat text into the in-memory tables (replaces anything prior). Used
// directly by tests; production code goes through lookup()'s lazy load.
void loadFromString(const std::string& ctyText);

// Load cty.dat from `path`; returns false if it can't be read.
bool loadFile(const std::string& path);

// True once a non-empty cty.dat has been loaded.
bool available();

// Resolve a callsign, applying exact-call exceptions, portable-indicator rules
// (/P, /MM, prefix overrides like DL/W1AW or W1AW/VE3) and longest-prefix
// matching. Returns nullptr if the call maps to no DXCC entity (e.g. /MM) or
// no country file is loaded. The pointer is valid until the next load.
const Info* lookup(const std::string& call);

}  // namespace dxcc
