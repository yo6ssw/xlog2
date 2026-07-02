// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <map>
#include <string>
#include <vector>

#include "Qso.h"

// Aggregate logbook statistics, computed from a QSO list with no UI dependency.
namespace stats {

struct Statistics {
  std::size_t total = 0;
  std::size_t uniqueCalls = 0;
  std::map<std::string, int> byBand;  // band name -> count (ordered)
  std::map<std::string, int> byMode;  // mode name -> count (ordered)
};

Statistics compute(const std::vector<Qso>& qsos);

}  // namespace stats
