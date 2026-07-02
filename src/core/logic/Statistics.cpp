// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "Statistics.h"

#include <set>

namespace stats {

Statistics compute(const std::vector<Qso>& qsos) {
    Statistics s;
    s.total = qsos.size();
    std::set<std::string> calls;
    for (const auto& q : qsos) {
        if (!q.band.empty()) ++s.byBand[q.band];
        if (!q.mode.empty()) ++s.byMode[q.mode];
        if (!q.call.empty()) calls.insert(q.call);
    }
    s.uniqueCalls = calls.size();
    return s;
}

}  // namespace stats
