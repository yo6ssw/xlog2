// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <string>

class LogPagePresenter;
struct QrzResult;

// What MainPresenter needs from the application shell. The shell (a backend's
// main window) implements this; the presenter routes service results through it
// without knowing about menus, tabs, dialogs or widgets.
class IMainView {
 public:
  virtual ~IMainView() = default;

  virtual void setStatus(const std::string&) = 0;

  // The current logbook tab's presenter, or null if there is none. Service
  // results (UDP QSOs, rig readings, QRZ/LoTW) are applied to it.
  virtual LogPagePresenter* currentLog() = 0;

  // True if `log` is still an open tab — guards results that arrive after the
  // initiating tab was closed.
  virtual bool isLogLive(LogPagePresenter* log) = 0;

  // Show the full QRZ.com record popup (a per-toolkit dialog).
  virtual void showQrzResult(const QrzResult&) = 0;

  // Kick off a background QRZ.com lookup for `callsign`; the result is routed
  // back through MainPresenter::routeQrzResult like any other lookup. Returns
  // true if a lookup was started, false if QRZ isn't configured or the client
  // is already busy (the presenter retries busy jobs when the client frees up).
  // Used to enrich QSOs received over UDP, not to prefill the entry form.
  virtual bool startQrzLookup(const std::string& callsign) = 0;
};
