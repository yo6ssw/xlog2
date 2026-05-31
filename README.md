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
- Frequency → band auto-detection as you type.
- "Now" button to stamp the current UTC date/time.
- SQLite-backed logbooks (`.xlog`), saved immediately as you log.
- ADIF import and export.
- Per-band / per-mode statistics.
- UDP network logging: auto-logs QSOs pushed by WSJT-X ("Logged ADIF")
  or any program sending raw ADIF datagrams (*Network* menu).
- Remembers your column layout (order, width, visibility) between runs,
  stored in `~/.config/xlog2/layout.ini`.

## Building

You need a C++20 compiler, CMake ≥ 3.16, and the development packages for
gtkmm-4 and SQLite.

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake libgtkmm-4.0-dev libsqlite3-dev
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
| `src/LogBook.*`      | SQLite-backed storage and CRUD.                 |
| `src/Udp.*`          | UDP listener + WSJT-X/ADIF datagram decoding.   |
| `src/QsoItem.h`      | `Glib::Object` wrapper for the `ColumnView`.    |
| `src/MainWindow.*`   | The main window: menu, log view, entry form.    |
| `src/XlogApplication.*` | The `Gtk::Application`.                       |
| `src/main.cpp`       | Entry point.                                    |

## License

GPL-3.0, like the original xlog.
