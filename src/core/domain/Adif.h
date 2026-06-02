#pragma once

#include "Qso.h"

#include <string>
#include <vector>

// Minimal ADIF (Amateur Data Interchange Format) reader/writer.
//
// ADIF encodes each field as "<NAME:LENGTH>value" with records terminated by
// "<EOR>" and an optional header terminated by "<EOH>". This implementation
// covers the field set used by Qso; unknown fields are ignored on import.
namespace adif {

// Parses ADIF text into QSO records. Robust to missing headers and extra
// whitespace; field names are matched case-insensitively.
std::vector<Qso> parse(const std::string& text);

// Serialises QSOs to a complete ADIF document, including a short header.
std::string write(const std::vector<Qso>& qsos);

} // namespace adif
