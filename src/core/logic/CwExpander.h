// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <string>

// Expands a cwdaemon message template, substituting %TOKEN% placeholders from
// the current entry form. Tokens are matched case-insensitively; an unknown or
// unterminated %…% is copied through literally.
namespace cw {

// The substitution values pulled from the entry form for a keyer message.
struct Substitutions {
    std::string call;  // %CALL%
    std::string name;  // %NAME%
    std::string qth;   // %QTH%
    std::string rst;   // %RST%  (the RST-received field)
};

std::string expand(const std::string& tmpl, const Substitutions& subs);

}  // namespace cw
