# xlog2

A small **amateur-radio logging program** for the desktop — a clone of the
classic [xlog](http://www.nongnu.org/xlog/), rebuilt with **GTK 4** (via
gtkmm-4), **C++20** and **CMake**.

It keeps a logbook of QSOs in a SQLite database and can import/export the
standard **ADIF** interchange format.

## Features

- Tabular log view (GTK 4 `ColumnView`) with date, time, call, band, mode,
  frequency, reports, name, QTH, locator, power, QSL status and comments.
- Entry form to add, edit and delete QSOs; click a row to load it for editing.
- **Multiple logbooks in tabs** — open several `.xlog` files at once; the
  previously open files are reopened on the next launch.
- **Duplicate detection** — flags in red when the contact being entered was
  already worked on the same band and mode that UTC day.
- Frequency → band auto-detection as you type.
- "Now" button to stamp the current UTC date/time.
- SQLite-backed logbooks (`.xlog`), saved immediately as you log.
- ADIF import and export.
- Per-band / per-mode statistics.
- UDP network logging: auto-logs QSOs pushed by WSJT-X ("Logged ADIF")
  or any program sending raw ADIF datagrams (*Network* menu).
- **Hamlib rig control** — polls the radio's frequency/mode and auto-fills the
  entry form (*Rig ▸ Connect…*). Enter a Hamlib model id (e.g. `1` for the
  built-in dummy rig, useful for testing) and a serial device.
- Remembers your column layout (order, width, visibility), window size and
  open tabs between runs, stored in `~/.config/xlog2/layout.ini`. (Window
  *position* is not restored — GTK4/Wayland provides no API to query or set it.)

## Building

You need a C++20 compiler, CMake ≥ 3.16, and the development packages for
gtkmm-4, SQLite and Hamlib.

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake libgtkmm-4.0-dev libsqlite3-dev libhamlib-dev
```

Then:

```sh
cmake -S . -B build
cmake --build build -j
./build/xlog2
```

## Layout

| File                 | Responsibility                                  |
|----------------------|-------------------------------------------------|
| `src/Qso.h`          | The QSO data model.                             |
| `src/Bands.*`        | Band/mode tables and frequency→band lookup.     |
| `src/Adif.*`         | ADIF parsing and serialisation.                 |
| `src/LogBook.*`      | SQLite-backed storage, CRUD and dupe lookup.    |
| `src/Udp.*`          | UDP listener + WSJT-X/ADIF datagram decoding.   |
| `src/Rig.*`          | Hamlib rig polling on a worker thread.          |
| `src/QsoItem.h`      | `Glib::Object` wrapper for the `ColumnView`.    |
| `src/UiUtil.h`       | Small shared UI helpers.                         |
| `src/LogPage.*`      | One logbook tab: log view + entry form + dupes. |
| `src/MainWindow.*`   | Shell: menu, notebook of tabs, UDP, rig.        |
| `src/XlogApplication.*` | The `Gtk::Application`.                       |
| `src/main.cpp`       | Entry point.                                    |

## License

GPL-3.0, like the original xlog.
