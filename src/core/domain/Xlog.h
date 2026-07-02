// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "Qso.h"

#include <string>
#include <vector>

// Reader for the native log format of the original `xlog` program (its "Flog"
// format, files with a ".xlog" extension).
//
// An xlog file is plain text: a fixed-width header line naming each column
// (e.g. "DATE", "GMT", "GMTEND", "CALL", "BAND", ...) followed by one
// space-padded, fixed-width record per QSO; the final column (REMARKS) is
// free-form to end of line. Column widths are derived from the header so the
// reader adapts to whatever column set a given file declares. Dates are stored
// as "DD Mon YYYY" and times as "HHMM"; xlog keeps the frequency in MHz in its
// "BAND" column. Columns without a Qso equivalent (NR, AWARDS, U1, U2) are
// ignored, mirroring the ADIF reader's handling of unknown fields.
namespace xlog {

// Parses native xlog text into QSO records. Returns an empty vector if the
// text does not look like an xlog file (no recognisable header).
std::vector<Qso> parse(const std::string& text);

} // namespace xlog
