#pragma once

#include "Qso.h"

#include <optional>
#include <string>
#include <vector>

struct sqlite3;

// How two QSOs are judged to be duplicates.
enum class DupeRule {
    CallBandModeDay,  // same call, band, mode and UTC day (default)
    CallBandMode,     // same call, band and mode (any day)
    Call,             // same call (any band/mode/day)
};

// A logbook backed by a SQLite database. Every add/update/remove is committed
// immediately, so an open file-backed logbook is always persisted. A freshly
// constructed LogBook lives in an in-memory database (an unsaved "New" log)
// until saveAs() migrates it to a file.
class LogBook {
public:
    LogBook();
    ~LogBook();

    LogBook(const LogBook&)            = delete;
    LogBook& operator=(const LogBook&) = delete;

    // Replaces the current logbook with a fresh empty in-memory one.
    void newInMemory();

    // Opens (or creates) a file-backed logbook, replacing the current one.
    bool open(const std::string& path);

    // Copies all current QSOs into a new file at path and switches to it.
    bool saveAs(const std::string& path);

    // The cached, ordered list of QSOs (by date then time on).
    const std::vector<Qso>& qsos() const { return cache_; }

    // CRUD. add() returns the new id and sets it on the cached copy.
    long add(const Qso& q);
    bool update(const Qso& q);
    bool remove(long id);

    // Imports ADIF text, inserting each record. Returns the number added.
    int importAdif(const std::string& adifText);

    // Serialises the whole logbook to ADIF text.
    std::string exportAdif() const;

    // Returns the most recent existing QSO that duplicates q under the given
    // rule, ignoring the row whose id == excludeId (the one being edited).
    // Empty if q is not a duplicate.
    std::optional<Qso> findDuplicate(const Qso& q, long excludeId = 0,
                                     DupeRule rule = DupeRule::CallBandModeDay) const;

    // --- LoTW (Logbook of The World) ---

    // QSOs not yet marked as uploaded to LoTW (lotw_sent != "Y").
    std::vector<Qso> qsosNotLotwSent() const;

    // Marks the given QSOs as uploaded to LoTW on the given date (YYYY-MM-DD).
    void markLotwSent(const std::vector<long>& ids, const std::string& date);

    // Given confirmation records downloaded from LoTW (each carrying call,
    // band, mode, date and optionally time_on / lotw_rcvd_date), marks the
    // matching local QSOs as LoTW-confirmed. Returns the number matched.
    int applyLotwConfirmations(const std::vector<Qso>& confirmed);

    const std::string& path() const { return path_; }
    bool isFileBacked() const { return !path_.empty(); }

private:
    bool createSchema(sqlite3* db);
    void reload();
    void close();

    sqlite3*         db_ = nullptr;
    std::string      path_;   // empty => in-memory
    std::vector<Qso> cache_;
};
