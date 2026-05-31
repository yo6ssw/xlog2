# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

xlog2 is a GTK4 amateur-radio logging program (a clone of `xlog`) written in C++20 with CMake. It logs QSOs to SQLite, exchanges ADIF, listens for QSOs over UDP, controls a radio via Hamlib, and syncs with ARRL's LoTW.

## Commands

```sh
# Configure + build
cmake -S . -B build && cmake --build build -j

# Run (a display is required; this box uses Wayland)
GDK_BACKEND=wayland ./build/xlog2

# Dependencies (Debian/Ubuntu)
sudo apt install build-essential cmake libgtkmm-4.0-dev libsqlite3-dev \
                 libhamlib-dev libcurl4-openssl-dev
# tqsl (LoTW upload, runtime only): sudo apt install tqsl
```

After changing headers or adding/removing source files, do a **clean rebuild**
(`rm -rf build && cmake -S . -B build && cmake --build build -j`). Incremental
builds here have produced stale binaries that fail at startup; a clean rebuild
resolves it.

### Tests

There is no test framework. Tests are written as throwaway `main()` programs
(historically under `/tmp`) compiled against the specific sources under test:

```sh
# Pure logic (no GUI): link only what you need
g++ -std=c++20 -Isrc /tmp/foo_test.cpp src/LogBook.cpp src/Adif.cpp \
    $(pkg-config --cflags --libs sqlite3) -o /tmp/foo_test && /tmp/foo_test
```

Anything touching gtkmm/giomm objects (incl. `Glib::Dispatcher`, `ColumnView`,
`Gio::Subprocess`) **must run inside a real `Gtk::Application`** — calling the C
`gtk_init()` skips gtkmm's wrapper-table registration and breaks
`get_object()`/casts. Pattern: create the app, do the work in a
`signal_activate()` handler, drive async work with
`Glib::MainContext::get_default()->iteration(false)` in a bounded loop, then
`app->quit()`. Run with `GDK_BACKEND=wayland`.

## Architecture

**Shell vs. page.** `MainWindow` (`src/MainWindow.*`) is a thin shell:
menu bar, a `Gtk::Notebook` of tabs, the status line, and the long-lived
service objects (`UdpListener`, `RigController`, `LotwClient`). Each tab is a
`LogPage` (`src/LogPage.*`) that owns one logbook's entire UI + state: a
`LogBook`, the `ColumnView`, the entry form, and live dupe detection. Menu
actions resolve `currentPage()` and delegate. A `LogPage` reports back to the
shell via two signals — `signalChanged()` (content/path changed → update tab
label/title/session) and `signalStatus()` (status-line text). When adding a
per-logbook feature, it almost always belongs in `LogPage`, not `MainWindow`.

**Data + storage.** `Qso` (`src/Qso.h`) is the flat record. `LogBook`
(`src/LogBook.*`) wraps a SQLite DB and keeps an in-memory `cache_` (the source
the UI reads); every `add/update/remove` commits immediately and reloads the
cache. The column set is defined **once** in the `kColumns` array — INSERT and
SELECT SQL are generated from it (`insertSql()`/`selectSql()`), so adding a
field means: add it to `Qso`, to `kColumns`, to `fieldsOf()`, to the CREATE in
`createSchema()`, and to the `reload()` column reads. `createSchema()` also runs
`ALTER TABLE ADD COLUMN` for every column (ignoring "duplicate" errors) to
migrate older `.xlog` files. A `LogBook` starts in-memory; `MainWindow` opens a
persistent default at `$XDG_DATA_HOME/xlog2/default.xlog` so logged QSOs survive
restarts. *New Tab* is a transient in-memory log until *Save As*.

**ADIF** (`src/Adif.*`) is the interchange format: `parse()`/`write()` map
between the ADIF wire form (`YYYYMMDD`/`HHMM`) and `Qso` (`YYYY-MM-DD`/`HH:MM`).
It is reused everywhere QSOs cross a boundary (import/export, UDP, LoTW).

**Concurrency model — never block the GTK main loop.** Three patterns coexist:
- **Non-blocking socket on the main loop:** `UdpListener` (`src/Udp.*`) watches
  a UDP fd with `Glib::signal_io` — no thread. Decodes WSJT-X "Logged ADIF"
  packets and raw ADIF datagrams.
- **Worker thread → `Glib::Dispatcher`:** `RigController` (`src/Rig.*`, Hamlib
  polling) and `LotwClient` download (`src/Lotw.*`, libcurl) run blocking work
  on a `std::thread` and marshal results to the UI thread via a
  `Glib::Dispatcher`. The library handle is only touched on the worker thread;
  callbacks (`onUpdate`, `onDownloadDone`, …) fire on the UI thread.
- **`Gio::Subprocess` async:** `LotwClient` upload spawns ARRL's `tqsl`
  (`wait_check_async`) — no thread, exit status delivered on the main loop.

**ColumnView.** Rows are `QsoItem` (`src/QsoItem.h`), a `Glib::Object` wrapper
required to put `Qso`s in a `Gio::ListStore`. Columns are built with
per-column getter lambdas via `LogPage::makeColumn`. Each column has a stable
string id (decoupled from its display title) used for layout persistence.

**Settings.** One `Glib::KeyFile` at `$XDG_CONFIG_HOME/xlog2/layout.ini` holds
everything: `[window]` geometry (size + maximized; **position is unsupported on
GTK4/Wayland**), shared column `[columns]`/`[width]`/`[visible]` layout,
`[session]` open files + active tab, `[udp]`, `[rig]`, and `[lotw]`. Saved from
`MainWindow::onCloseRequest` (a `signal_close_request` handler, so widgets are
still alive) and applied in `loadSettings`. The file is `chmod 0600` because
`[lotw]` stores the download password in plain text.

**Widget lifetime.** Child widgets use `Gtk::make_managed<>` (parent-owned).
Top-level windows that are heap-allocated (the main window, About/Stats/settings
dialogs) set `set_hide_on_close(true)` and `delete` themselves from a
`signal_hide` handler — this is the idiom used throughout; follow it for new
dialogs. The app id is `ro.scripca.xlog2`.

**Deprecation stance.** Code deliberately uses non-deprecated GTK4 APIs
(`ColumnView` not `TreeView`, `DropDown` not `ComboBoxText`, `FileDialog` not
`FileChooserDialog`, `CssProvider::load_from_string`). Prefer the same when
extending.
