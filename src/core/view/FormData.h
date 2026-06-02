#pragma once

#include <string>

// The QSO-entry form as plain data — the only structure that crosses the
// View boundary for the entry form. A backend's view reads its widgets into a
// FormData (and writes one back); the presenter converts to/from Qso. Keeping
// it toolkit-neutral lets the gtkmm and Qt views share one presenter.
//
// DXCC fields are intentionally absent: they are derived from the callsign by
// the presenter (see DxccDeriver), not typed into the form.
struct FormData {
    std::string date;       // YYYY-MM-DD (UTC)
    std::string time_on;    // HH:MM (UTC)
    std::string time_off;   // HH:MM (UTC)
    std::string call;
    std::string band;       // selected band name, empty if none
    std::string mode;       // selected mode name, empty if none
    std::string freq;       // MHz as text
    std::string rst_sent;
    std::string rst_rcvd;
    std::string name;
    std::string qth;
    std::string locator;
    std::string power;
    std::string comment;
    bool        qsl_sent = false;
    bool        qsl_rcvd = false;
};
