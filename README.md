# xlog2

A desktop **amateur-radio logging program** — a clone of the classic
[xlog](http://www.nongnu.org/xlog/), rebuilt in **C++20** with **CMake**.

xlog2 ships **two interchangeable frontends** over one shared, toolkit-neutral
core library:

- **`xlog2-gtk`** — a **GTK 4** (gtkmm-4) UI.
- **`xlog2-qt`** — a **Qt 6 Widgets** UI.

Both read and write the same logbooks (`.xlog`) and the same settings file, so
you can switch between them freely.

## Features

- Tabular log view with date, time, call, band, mode, frequency, reports, name,
  QTH, locator, power, QSL status and comments. Columns can be reordered (drag a
  header or use its context menu) and resized; the layout is remembered and
  shared between both frontends.
- Entry form to add, edit and delete QSOs; click a row to load it for editing.
  RST sent/received default to `599`.
- **Multiple logbooks in tabs** — open several `.xlog` files at once; the
  previously open files (and the active tab) are reopened on the next launch.
- **Duplicate detection** — flags in red when the contact being entered was
  already worked on the same band and mode that UTC day.
- Frequency → band auto-detection as you type; a "Now" button stamps the
  current UTC date/time.
- SQLite-backed logbooks (`.xlog`), saved immediately as you log. The default
  logbook lives at `$XDG_DATA_HOME/xlog2/default.xlog`
  (`~/.local/share/xlog2/default.xlog`) and is reopened automatically. *New Tab*
  opens a transient in-memory log until *Save As*.
- ADIF import and export; per-band / per-mode statistics.
- **UDP network logging** — auto-logs QSOs pushed by WSJT-X ("Logged ADIF") or
  any program sending raw ADIF datagrams.
- **Hamlib rig control** — polls the radio's frequency/mode and auto-fills the
  entry form. Connecting is non-blocking, so an unreachable rig never freezes
  the UI.
- **DXCC lookup** — entity / CQ-ITU zones / continent from `cty.dat` (drop one
  at `$XDG_DATA_HOME/xlog2/cty.dat`).
- **QRZ.com** callsign lookup (name/QTH/grid prefill).
- **Network CW keyer** (cwdaemon) with F1–F9 message macros (the function keys
  work from anywhere in the window, including the DX-cluster panel).
- **DX-cluster** (telnet) band map: spots aggregated by frequency with a
  spotter count, band-filter chips, and double-click to tune.
- **Rig audio** — plays a `cwsd` `audio_stream_server` Opus-over-UDP stream of
  the receiver audio through a local ALSA device.
- **CW Skimmer** — a dockable multi-channel CW decoder: it decodes every CW
  signal in the rig-audio passband at once, showing a waterfall with callsign
  labels plus a per-signal decode table (frequency / WPM / text / call), with
  Gate and Min-SNR controls. Decoded callsigns are validated/corrected against an
  optional Super-Check-Partial list (drop a `MASTER.SCP` at
  `$XDG_DATA_HOME/xlog2/master.scp`), with a "calls in database only" mode. See
  [docs/cw-skimmer-decoder.md](docs/cw-skimmer-decoder.md) for how it works.
- **LoTW** — *Upload* signs/submits new QSOs via ARRL's `tqsl`; *Download
  confirmations* fetches confirmed QSOs and marks matches (call+band+mode+date).
  A `LoTW` column shows `✓` confirmed / `↑` uploaded.
- Remembers column layout, window size and open tabs in
  `~/.config/xlog2/layout.ini`. (Window *position* isn't restored — GTK4/Wayland
  has no API for it.) The LoTW download password is stored there in **plain
  text** (file mode `0600`); LoTW upload uses `tqsl`/your certificate instead.

## Building

A C++20 compiler, CMake ≥ 3.16, and the dev packages for SQLite, Hamlib,
libcurl, Opus and ALSA, plus gtkmm-4 (for the GTK frontend) and/or Qt 6 (for the
Qt frontend).

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config \
    libgtkmm-4.0-dev qt6-base-dev \
    libsqlite3-dev libhamlib-dev libcurl4-openssl-dev \
    libopus-dev libasound2-dev
# Optional, runtime only — for LoTW upload:
sudo apt install tqsl
```

Then:

```sh
cmake -S . -B build && cmake --build build -j
```

This builds both frontends — `build/xlog2-gtk` and `build/xlog2-qt`. Build just
one with the options:

```sh
cmake -S . -B build -DXLOG_BUILD_GTK=ON -DXLOG_BUILD_QT=OFF   # GTK only
cmake -S . -B build -DXLOG_BUILD_GTK=OFF -DXLOG_BUILD_QT=ON   # Qt only
```

Run:

```sh
GDK_BACKEND=wayland ./build/xlog2-gtk
./build/xlog2-qt
```

> After changing headers or adding/removing source files, prefer a **clean
> rebuild** (`rm -rf build && cmake -S . -B build && cmake --build build -j`).

## Layout

```
src/core/        toolkit-neutral library `xlog_core` (no gtkmm/Qt):
  domain/          Qso, LogBook (SQLite), Adif, Bands, Dxcc, DxSpot
  logic/           QsoMapper, DxccDeriver, CwExpander, Statistics, DupeMessage,
                   StrUtil, TimeUtil
  settings/        Settings + IniFile (the layout.ini codec)
  platform/        IUiDispatcher (UI-thread marshalling), ProcessRunner
  services/        Rig (Hamlib), Udp, Lotw, Qrz, CwKeyer, DxCluster, Audio
  presenter/       LogPagePresenter, MainPresenter — all business logic
  view/            ILogPageView, IMainView, FormData (interfaces + DTOs)

src/             gtkmm frontend: MainWindow, LogPage, DxClusterPanel,
                 XlogApplication, GlibDispatcher, UiUtil, main.cpp
src/qt/          Qt frontend: QtMainWindow, QtLogPage, QtDxClusterPanel,
                 QsoTableModel, FlowLayout, QtDispatcher, main.cpp

debian/          Debian packaging (one source → xlog2-gtk + xlog2-qt binaries)
packaging/       .desktop files, release.sh, publish-ppa.sh
```

The core links only SQLite/Hamlib/libcurl/Opus/ALSA/threads — never a GUI
toolkit — so the UI/business-logic boundary is enforced by the build. See
[`CLAUDE.md`](CLAUDE.md) for the detailed design notes.

## Packaging & releasing

- **Versioning:** Semantic Versioning; see [`VERSIONING.md`](VERSIONING.md). The
  version's single source of truth is `project(VERSION ...)` in `CMakeLists.txt`;
  releases are annotated `vX.Y.Z` git tags.
- **Cut a release:** `packaging/release.sh X.Y.Z` (bumps version + changelog,
  commits, tags), then `git push origin master --follow-tags`.
- **Publish to the PPA:** `packaging/publish-ppa.sh` builds signed Debian source
  packages and uploads them to `ppa:benishor/hamtools`, which builds the
  binaries. Needs `devscripts`, `debhelper`, `dput` and a GPG key registered on
  Launchpad; pass `--no-upload` to build without uploading.

## Tests

No test framework; tests are throwaway `main()` programs linked against
`xlog_core`. Pure logic needs no GUI:

```sh
g++ -std=c++20 -Isrc/core/domain -Isrc/core/logic \
    /tmp/foo_test.cpp build/libxlog_core.a \
    $(pkg-config --cflags --libs sqlite3 hamlib libcurl) -o /tmp/foo_test
```

## License

GPL-3.0, like the original xlog. See [`LICENSE`](LICENSE).
