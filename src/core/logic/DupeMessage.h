// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "Qso.h"

#include <string>

// Formats the duplicate-warning banner from the QSO that a new entry duplicates.
// (Duplicate detection itself lives in LogBook::findDuplicate.)
namespace dupe {

// e.g. "⚠ Dupe — already worked DL1ABC on 20m SSB at 14:30 (2026-06-02)".
std::string format(const Qso& existing);

}  // namespace dupe
