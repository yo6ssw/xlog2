// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

// Single source of truth for the displayed version is project(VERSION ...) in
// CMakeLists.txt, baked in here at build time via the XLOG_VERSION macro
// (target_compile_definitions on xlog_core, propagated to both frontends).

namespace xlog {
#ifdef XLOG_VERSION
inline constexpr const char* kVersion = XLOG_VERSION;
#else
inline constexpr const char* kVersion = "unknown";
#endif
}  // namespace xlog
