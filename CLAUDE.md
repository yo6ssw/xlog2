# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

xlog2 is a C++20/CMake amateur-radio logging program (a clone of `xlog`). It
logs QSOs to SQLite, exchanges ADIF, receives QSOs over UDP, controls a radio
via Hamlib, looks up calls on QRZ.com, drives a network CW keyer (text via
cwdaemon, plus real paddle keying via cwsd's remote_key), shows a DX-cluster
band map, and syncs with ARRL's LoTW.

It is built as a **toolkit-neutral core library (`xlog_core`) with two
interchangeable frontends**: `xlog2-gtk` (GTK 4 / gtkmm) and `xlog2-qt`
(Qt 6 Widgets). Both drive the same core and read/write the same files.

## Commands

```sh
# Configure + build (produces build/xlog2-gtk and build/xlog2-qt)
cmake -S . -B build && cmake --build build -j

# Build one frontend only
cmake -S . -B build -DXLOG_BUILD_GTK=ON -DXLOG_BUILD_QT=OFF
cmake -S . -B build -DXLOG_BUILD_GTK=OFF -DXLOG_BUILD_QT=ON

# Run (a display is required; this box uses Wayland)
GDK_BACKEND=wayland ./build/xlog2-gtk
./build/xlog2-qt

# Dependencies (Debian/Ubuntu)
sudo apt install build-essential cmake pkg-config \
                 libgtkmm-4.0-dev qt6-base-dev \
                 libsqlite3-dev libhamlib-dev libcurl4-openssl-dev \
                 libopus-dev libasound2-dev
# tqsl (LoTW upload, runtime only): sudo apt install tqsl
```

After changing headers or adding/removing source files, do a **clean rebuild**
(`rm -rf build && cmake -S . -B build && cmake --build build -j`). Incremental
builds here have produced stale binaries; a clean rebuild resolves it.

### Tests

There is no test framework. Tests are throwaway `main()` programs compiled
against `xlog_core` (build it first; it's `build/libxlog_core.a`):

```sh
g++ -std=c++20 -Isrc/core/domain -Isrc/core/logic -Isrc/core/settings \
    -Isrc/core/view -Isrc/core/platform -Isrc/core/services -Isrc/core/presenter \
    /tmp/foo_test.cpp build/libxlog_core.a \
    $(pkg-config --cflags --libs sqlite3 hamlib libcurl) -lpthread -o /tmp/foo_test
```

Because the core is toolkit-free, **presenters and logic are testable with no
GUI** — implement the view interface (`ILogPageView`/`IMainView`) as a stub that
records calls, drive the presenter, and assert. (See the RST-default and
dupe/DXCC checks done this way.) The Qt frontend can be exercised headlessly
with `QtTest` + `QT_QPA_PLATFORM=offscreen|wayland`; gtkmm widget code, if ever
tested, must run inside a real `Gtk::Application` (the C `gtk_init()` skips
gtkmm's wrapper-table registration and breaks casts).

## Architecture

**Core vs. frontends (Passive-View MVP).** `xlog_core` (`src/core/`) contains
*all* business logic and has **no gtkmm/Qt on its include path** — the boundary
is enforced by the build, so a stray toolkit include in core fails to compile.
Each frontend provides only thin *views* plus a couple of platform adapters.

- **Presenters** (`src/core/presenter/`) own the logic. `LogPagePresenter` holds
  one logbook's state (a `LogBook`, the entry-form mapping, live dupe + DXCC
  indicators, CW expansion, CRUD). `MainPresenter` holds shell-level config
  (`Settings`) and routes service results to the current tab.
- **View interfaces** (`src/core/view/`): `ILogPageView`, `IMainView`, and the
  `FormData` DTO. Presenters call these; the frontend implements them.
- **gtkmm views** (`src/`): `LogPage`/`MainWindow` implement the interfaces with
  gtkmm widgets; `XlogApplication` is the `Gtk::Application`; `GlibDispatcher` is
  the UI-thread adapter.
- **Qt views** (`src/qt/`): `QtLogPage`/`QtMainWindow` implement the same
  interfaces with Qt Widgets; `QtDispatcher` is the adapter; `QsoTableModel`,
  `QtDxClusterPanel` and `FlowLayout` are Qt-specific helpers.

When adding a per-logbook feature, it belongs in `LogPagePresenter`, not the
views. Shell-level routing belongs in `MainPresenter`. Only widget plumbing
goes in the frontends.

**Data + storage.** `Qso` (`src/core/domain/Qso.h`) is the flat record. `LogBook`
(`src/core/domain/LogBook.*`) wraps a SQLite DB and keeps an in-memory `cache_`
(ordered by `date, time_on, id`) that the UI reads; every `add/update/remove`
commits immediately and reloads the cache. The column set is defined **once** in
the `kColumns` array — INSERT/SELECT SQL is generated from it, and `createSchema`
runs `ALTER TABLE ADD COLUMN` per column (ignoring "duplicate" errors) to migrate
older `.xlog` files. Adding a field means touching: `Qso`, `kColumns`,
`fieldsOf()`, the CREATE in `createSchema()`, the `reload()` reads, `FormData` +
`QsoMapper` if it's on the entry form, and each frontend's column list
(gtkmm `LogPage::buildLogView`'s `add(...)`; Qt `QsoTableModel`). A `LogBook`
starts in-memory; the shell opens a persistent default at
`$XDG_DATA_HOME/xlog2/default.xlog`. *New Tab* is in-memory until *Save As*.

**ADIF** (`src/core/domain/Adif.*`) maps between the ADIF wire form
(`YYYYMMDD`/`HHMM`) and `Qso` (`YYYY-MM-DD`/`HH:MM`); reused everywhere QSOs
cross a boundary (import/export, UDP, LoTW).

**Original-xlog import** (`src/core/domain/Xlog.*`) reads the native fixed-width
text format of the original `xlog` program (`.xlog` "Flog" files): a header line
names the columns and their widths are derived from it. `LogBook::importXlog`
funnels through the same `insertAll()` path as `importAdif`. Note xlog keeps the
frequency in MHz in its `BAND` column (so it maps to `Qso::freq`, with the band
name derived via `bands::forFrequencyMHz`), dates are `DD Mon YYYY`, and
`RST`/`MYRST` are sent/received.

**Concurrency — never block the UI thread.** All services live in
`src/core/services/` and are toolkit-neutral: blocking work runs on a
`std::thread` and results are marshalled to the UI thread via an injected
`IUiDispatcher` (`GlibDispatcher` for gtkmm, `QtDispatcher` — a queued
`QMetaObject::invokeMethod` — for Qt).
- `RigController` (Hamlib) — `rig_open()` (which can block on an unreachable
  network rig) runs **on the worker**; `start()` returns immediately and reports
  via `onConnectResult`. `stop()` detaches a still-connecting worker (via a
  shared `Run` handshake) rather than hang at shutdown.
- `UdpListener` and `DxCluster` — worker thread doing blocking POSIX sockets
  (`recv`/`poll`/`getaddrinfo`), woken for stop/commands via a self-pipe. (These
  are the main **POSIX-specific** code, relevant to any future Windows port.)
- `LotwClient` — download via libcurl on a worker; upload spawns ARRL's `tqsl`
  via the neutral `ProcessRunner` (`posix_spawn` + `waitpid`).
- `AudioStreamClient` (`src/core/services/Audio.*`) — subscribes to a **cwsd**
  `audio_stream_server` Opus-over-UDP rig-audio stream and plays it back. A
  worker owns a connected UDP socket (woken for stop via a self-pipe), sends a
  small keepalive every ~2 s (cwsd drops silent subscribers), and for each
  datagram (4-byte big-endian sequence + Opus packet) Opus-decodes and writes to
  an **ALSA** playback device — opened lazily on the worker so a missing device
  never blocks. `sampleRate`/`channels` must match the server. It also posts a
  running decoded-frame count (`onStats`) ~once a second, shown as a live
  indicator in each shell's status bar. (Links `opus` + `asound`; the only audio
  code in xlog2.) It also exposes an `onPcm` tap (fired on the worker thread for
  each unmuted datagram) that feeds the CW skimmer.
- `CwSkimmer` (`src/core/services/CwSkimmer.*`) — a pragmatic **multi-channel CW
  decoder** in the spirit of VE3NEA's CW Skimmer, scaled to a single audio
  passband (the rig-audio stream) rather than wideband IQ. Fed mono PCM via
  `pushPcm` from `AudioStreamClient::onPcm`, its own worker decodes every carrier
  in the passband at once. Since CW lives only in the low audio (the band is
  capped at ~4 kHz), the worker first **decimates the stream to ~12 kHz** (an
  anti-alias FIR + downsample, e.g. ÷4 from 48 kHz) and runs the whole pipeline
  there — a quarter-size FFT and quarter-rate per-channel receivers for the same
  resolution. The decoder uses a **two-path** design that decouples frequency
  resolution from envelope timing (the single-FFT tradeoff otherwise forces a
  choice — coarse bins merge crowded signals into a few channels; a short window
  for sharp keying gives coarse bins): a **fine FFT** (~512 pts/23 Hz bins @
  12 kHz, dependency-free radix-2) drives the waterfall, the noise floor and
  channel detection (dominant-peak spawn + harmonic suppression, one channel per
  carrier); each channel then runs its **own narrowband receiver** — complex
  down-conversion to baseband at the carrier + a 2-stage low-pass — whose output
  power is the keying envelope, with time resolution set by the filter, not the
  FFT window. The decode then follows CW Skimmer's "soft, not hard" philosophy:
  envelope power vs a per-channel min-tracked noise floor → a continuous
  **tone-present probability** (logistic in SNR, never a per-frame threshold) →
  a fixed-lag **2-state Viterbi** picking the most-probable key down/up
  segmentation (the transition cost rejects noise blips and rides through fades,
  replacing any Schmitt/debounce); finalised runs are decoded with min-tracked
  dit-mark / element-gap references (kept *separate* because the finite window
  inflates marks and shrinks gaps), characters flushed as the inter-character
  silence elapses. Callsigns are pattern-matched out of the decoded text and then
  validated/edit-distance-corrected against an optional master-callsign list
  (`$XDG_DATA_HOME/xlog2/master.scp`, Super Check Partial) — CW Skimmer's biggest
  accuracy lever — with a Paranoid mode that surfaces only DB-confirmed calls.
  Emits `onWaterfall` (a max-pooled spectrum row),
  `onChannel` (id = FFT bin, pitch, wpm, rolling text, call) and
  `onChannelRemoved`, all marshalled to the UI thread. The dockable panels
  (`QtCwSkimmerPanel`, gtkmm `CwSkimmerPanel`) show a scrolling waterfall with
  callsign labels plus a decode table. Cold-start mangles the first character or
  two (the unit length isn't yet known) — inherent to streaming Morse decoders.
  **Full design reference: `docs/cw-skimmer-decoder.md`** (pipeline, every tuning
  constant, the operator controls, ghost/noise suppression, new-station
  adaptation, and known limitations) — update it when changing the decoder.
- `RemotePaddleKeyer` (`src/core/services/RemotePaddleKeyer.*`, wire format in
  `RemoteKeyProtocol.h`) — operator-side client for **cwsd's `remote_key`
  service: real paddle keying over the internet**. Unlike `CwKeyer` (which sends
  *text* for cwdaemon to key), the iambic keyer runs *here*, on jitter-free local
  paddle input, and streams finished key edges (key-down/key-up) timestamped on a
  per-session monotonic clock; cwsd replays them behind a fixed playout delay, so
  jitter never distorts the Morse. The worker polls the paddle atomics every
  ~250 us and ticks the element generator, timestamping each edge from the element
  *schedule* (not the poll clock) so dit/dah/gap durations stay exact regardless of
  poll granularity or scheduler stalls — the poll interval bounds only how fast a
  fresh paddle close is reacted to. Each edge ships as a UDP datagram carrying
  recent edges as loss-recovery history (cwsd dedups by timestamp), plus a
  keepalive (<cwsd's silence timeout) while idle. Paddle contacts arrive from the UI thread via
  lock-free `setDit`/`setDah`; **for testing, the `[` and `]` keys** simulate
  dit/dah (gtkmm: a CAPTURE-phase `EventControllerKey`; Qt: an app-wide
  `eventFilter`), intercepted only while the keyer is active so the brackets type
  normally otherwise. Config lives in the `[paddle]` settings block, is edited
  from the **Keyer ▸ Remote paddle keying / Paddle settings…** menu, and
  auto-resumes at startup. A second thread renders a **click-free local sidetone**
  (ramped-envelope sine) to ALSA, gated by the same key transitions, so the
  operator gets instant feel while the on-air signal lags by cwsd's playout delay;
  it is independent, so a missing audio device never stalls keying. (Reuses the
  `asound` link already pulled in for the audio stream.) It also reports a
  **transmit on/off** state via `onTransmit` (latched on the first key-down,
  released after a hang past the last key-up so it bridges character/word gaps);
  the shell wires that to `AudioStreamClient::setMuted` (gated by the
  `[paddle] mute_audio` setting) so the rig-audio stream is silenced while keying
  — semi-break-in, since otherwise you'd hear your own delayed signal fighting the
  local sidetone. `setMuted` keeps decoding/feeding ALSA with silence (no
  unsubscribe, no unmute glitch). **Autospace** (`[paddle] autospace`, on by
  default) holds a new character's first element to the 3-dit inter-character
  boundary when it's keyed too soon after the previous element, so quickly-tapped
  letters don't run together; it acts only across the idle gap between characters
  (a `Wait` phase before `Mark`), never on a continuing squeeze. *Scaffold note:*
  iambic memory gives iambic-A; full iambic-B is a TODO.
- Posted closures hold a `weak_ptr` liveness token so a result arriving after
  the controller/view is gone is dropped.

**Settings.** A neutral `IniFile` (`src/core/settings/`) reads/writes
`$XDG_CONFIG_HOME/xlog2/layout.ini`; the scalar config maps to the `Settings`
struct (`Settings::load/store`). Window geometry, the shared column layout
(`[columns]`/`[width]`/`[visible]`, stable per-column ids) and `[session]` tabs
live in the ini too. Both frontends `chmod 0600` it (the `[lotw]` download
password is plain text). gtkmm saves from `MainWindow::onCloseRequest`; Qt from
`QtMainWindow::closeEvent`.

**Frontend-specific notes.**
- *gtkmm:* rows are `QsoItem` (a `Glib::Object`) in a `Gio::ListStore` →
  `FilterListModel` → `ColumnView`; child widgets are `Gtk::make_managed<>`;
  heap top-levels set `set_hide_on_close(true)` and `delete` from `signal_hide`.
  Use non-deprecated GTK4 APIs (`ColumnView` not `TreeView`, `DropDown` not
  `ComboBoxText`, `FileDialog` not `FileChooserDialog`). App id `ro.scripca.xlog2`.
- *Qt:* `QsoTableModel` + `QSortFilterProxyModel`; the log shows source order
  (newest at bottom) via `sortByColumn(-1)`; the DX dock size is restored from
  the `position` setting via `resizeDocks` in show/resize events (it must run
  after the window has its laid-out, possibly maximized, size); F1–F9 are
  window-wide `QShortcut`s routed to the active tab.

## Versioning, packaging & releasing

SemVer; the version's single source of truth is `project(VERSION ...)` in
`CMakeLists.txt`. See **`VERSIONING.md`**. Cut a release with
`packaging/release.sh X.Y.Z` (bumps version + `debian/changelog`, commits, tags
`vX.Y.Z`). `debian/` builds one source package into the `xlog2-gtk` and
`xlog2-qt` binaries; `packaging/publish-ppa.sh` uploads signed source packages
to `ppa:benishor/hamtools`.
