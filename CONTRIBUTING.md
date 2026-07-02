# Contributing to xlog2

Thanks for your interest! xlog2 is a logging program for radio amateurs on
Linux, and contributions — bug reports, fixes, features, docs — are welcome.

## Reporting bugs

Open a GitHub issue and include:

- your distribution and `cmake --version`,
- which frontend (`xlog2-gtk` or `xlog2-qt`) and your gtkmm-4 / Qt 6 versions,
- what you did and what happened, with any terminal output,
- for rig/UDP/cluster/LoTW issues, the relevant service and settings.

## Building

xlog2 uses a **git submodule** (`third_party/multimaster`), so clone with
`--recurse-submodules` (or run `git submodule update --init --recursive`).
See the [README](README.md) for the full dependency list. In short:

```sh
sudo apt install build-essential cmake pkg-config \
    libgtkmm-4.0-dev qt6-base-dev libsqlite3-dev libhamlib-dev \
    libcurl4-openssl-dev libopus-dev libasound2-dev \
    libpipewire-0.3-dev libdbus-1-dev libsodium-dev
cmake -S . -B build && cmake --build build -j
```

Requires a C++20 compiler. Build just one frontend with `-DXLOG_BUILD_GTK=OFF`
or `-DXLOG_BUILD_QT=OFF`. After changing headers or the file list, prefer a
**clean rebuild** (`rm -rf build && …`) — incremental builds have produced stale
binaries here.

## Architecture

xlog2 is a **toolkit-neutral core (`xlog_core`) with two thin frontends** (GTK 4
and Qt 6). All business logic lives in the core's presenters; the frontends only
implement view interfaces. New per-logbook logic belongs in `LogPagePresenter`,
shell logic in `MainPresenter` — **not** in the widgets. Read
[`CLAUDE.md`](CLAUDE.md) before making non-trivial changes.

## Coding style

- **clang-format, Google style** — the `.clang-format` at the repo root is
  authoritative. Run `clang-format -i` on the C/C++ files you touch.
- **Do not** run clang-format on the `third_party/multimaster` submodule (it has
  its own style) or on the vendored native libraries.
- Kotlin (Android) is not covered by clang-format; follow the surrounding style.
- Keep pure-reformatting changes in their **own commit** (see
  `.git-blame-ignore-revs`).

## Licensing of contributions

xlog2 is **GPL-3.0-or-later**. By contributing you agree to license your work
under those terms. Add an SPDX header to new source files:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: <year> <your name / callsign>
```

Please sign off your commits (`git commit -s`) per the
[Developer Certificate of Origin](https://developercertificate.org/).

## Tests

There is no GUI test framework; because the core is toolkit-free, logic and
presenters are testable with a stub view (see `CLAUDE.md` for the pattern).
Describe how you tested in your PR.

## Pull requests

- Branch off `master`; keep PRs focused; make sure CI is green.
