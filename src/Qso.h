#pragma once

#include <string>

// A single logged contact (QSO).
//
// Dates are stored as "YYYY-MM-DD" and times as "HH:MM" (UTC), matching the
// way they are presented in the UI. Conversion to/from the ADIF wire format
// (YYYYMMDD / HHMM) happens in Adif.cpp.
struct Qso {
    long        id = 0;          // SQLite rowid; 0 means "not yet stored"
    std::string date;           // QSO date, UTC,  YYYY-MM-DD
    std::string time_on;        // time on,  UTC,  HH:MM
    std::string time_off;       // time off, UTC,  HH:MM
    std::string call;           // worked station callsign
    std::string band;           // e.g. "20m"
    std::string mode;           // e.g. "SSB", "CW", "FT8"
    std::string freq;           // frequency in MHz, e.g. "14.250"
    std::string rst_sent;       // report sent
    std::string rst_rcvd;       // report received
    std::string name;           // operator name
    std::string qth;            // location / town
    std::string locator;        // Maidenhead grid square
    std::string power;          // TX power in watts
    std::string qsl_sent;       // "Y" / "N" / ""
    std::string qsl_rcvd;       // "Y" / "N" / ""
    std::string comment;        // free-form remarks
};
