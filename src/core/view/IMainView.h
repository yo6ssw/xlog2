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
};
