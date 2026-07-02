# Third-party components

xlog2 itself is licensed under GPL-3.0-or-later (see [`LICENSE`](LICENSE)). It
uses the following third-party components:

## Bundled / vendored

| Component | Location | License | Upstream |
|-----------|----------|---------|----------|
| multimaster | `third_party/multimaster` (git submodule) | LGPL-3.0-or-later | https://github.com/benishor/multimaster |
| libopus | `android/third_party/…` (Android build only) | BSD-3-Clause | https://opus-codec.org |
| libsodium | `android/third_party/…` (Android build only) | ISC | https://libsodium.org |
| Natural Earth coastline (1:110m) | `data/coastline.txt` | Public domain | https://www.naturalearthdata.com |

The `multimaster` submodule is fetched from its public repository; run
`git clone --recurse-submodules` (or `git submodule update --init --recursive`).

## Linked system libraries (provided by your distribution)

SQLite (public domain), **hamlib** (LGPL/GPL), **libcurl** (curl license),
**Opus** (BSD), **ALSA** `libasound` (LGPL), **gtkmm-4** (LGPL) and/or **Qt 6**
(LGPL), PipeWire (MIT), D-Bus (GPL/AFL), libsodium (ISC). LoTW upload shells out
to ARRL's `tqsl` at runtime; it is not bundled.
