// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <cctype>
#include <string>
#include <vector>

// Small toolkit-neutral string helpers shared across the core.
namespace strutil {

// Uppercase an ASCII string in place-style (returns a copy). Used for callsigns
// and Maidenhead locators, which are conventionally upper-case.
inline std::string toUpper(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Split a ';'-separated list, dropping empty fields. Used for the saved session
// path list.
inline std::vector<std::string> splitSemicolons(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ';') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

}  // namespace strutil
