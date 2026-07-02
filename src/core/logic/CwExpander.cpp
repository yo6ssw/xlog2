// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "CwExpander.h"

#include "StrUtil.h"

#include <array>
#include <utility>

namespace cw {

std::string expand(const std::string& tmpl, const Substitutions& subs) {
    const std::array<std::pair<std::string, std::string>, 4> table = {{
        {"CALL", subs.call},
        {"NAME", subs.name},
        {"QTH",  subs.qth},
        {"RST",  subs.rst},
    }};

    std::string out;
    const size_t n = tmpl.size();
    size_t i = 0;
    while (i < n) {
        if (tmpl[i] == '%') {
            const size_t end = tmpl.find('%', i + 1);
            if (end != std::string::npos) {
                const std::string name = strutil::toUpper(tmpl.substr(i + 1, end - i - 1));
                bool matched = false;
                for (const auto& [tok, val] : table)
                    if (name == tok) { out += val; matched = true; break; }
                if (matched) {
                    i = end + 1;
                    continue;
                }
            }
            // Unknown or unterminated token: copy the '%' through literally.
        }
        out += tmpl[i++];
    }
    return out;
}

}  // namespace cw
