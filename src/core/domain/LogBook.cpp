// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "LogBook.h"

#include "Adif.h"
#include "TimeUtil.h"
#include "Uuid.h"
#include "Xlog.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace {

// Column order shared by INSERT, UPDATE and SELECT. id is handled separately.
constexpr std::array<const char*, 24> kColumns = {
    "date", "time_on", "time_off", "call", "band", "mode", "freq",
    "rst_sent", "rst_rcvd", "name", "qth", "locator", "power",
    "qsl_sent", "qsl_rcvd", "comment",
    "lotw_sent", "lotw_sent_date", "lotw_rcvd", "lotw_rcvd_date",
    "country", "cq_zone", "itu_zone", "continent",
};

std::array<std::string, kColumns.size()> fieldsOf(const Qso& q) {
    return {q.date, q.time_on, q.time_off, q.call, q.band, q.mode, q.freq,
            q.rst_sent, q.rst_rcvd, q.name, q.qth, q.locator, q.power,
            q.qsl_sent, q.qsl_rcvd, q.comment,
            q.lotw_sent, q.lotw_sent_date, q.lotw_rcvd, q.lotw_rcvd_date,
            q.country, q.cq_zone, q.itu_zone, q.continent};
}

void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(),
                      static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

// "INSERT INTO qsos (c1,c2,...,uuid,updated_at) VALUES (?,?,...);" built from
// kColumns so the three insert sites cannot drift out of sync. The two sync
// columns are appended explicitly (they are not in kColumns / fieldsOf because
// they carry server-authoritative identity/timestamps, never caller values).
std::string insertSql() {
    std::string cols, marks;
    for (size_t i = 0; i < kColumns.size(); ++i) {
        if (i) { cols += ','; marks += ','; }
        cols += kColumns[i];
        marks += '?';
    }
    cols += ",uuid,updated_at";
    marks += ",?,?";
    return "INSERT INTO qsos (" + cols + ") VALUES (" + marks + ");";
}

// Binds the 24 user columns (params 1..24) followed by uuid (25) and
// updated_at (26) for an insertSql() statement.
void bindInsertRow(sqlite3_stmt* stmt, const Qso& q, const std::string& uuid,
                   const std::string& updatedAt) {
    const auto fields = fieldsOf(q);
    int i = 0;
    for (; i < static_cast<int>(fields.size()); ++i)
        bindText(stmt, i + 1, fields[i]);
    bindText(stmt, i + 1, uuid);
    bindText(stmt, i + 2, updatedAt);
}

// "SELECT id,c1,c2,...,uuid,updated_at FROM qsos ORDER BY date,time_on,id;"
std::string selectSql() {
    std::string cols = "id";
    for (const char* c : kColumns) {
        cols += ',';
        cols += c;
    }
    cols += ",uuid,updated_at";
    return "SELECT " + cols + " FROM qsos ORDER BY date, time_on, id;";
}

// Deterministic content key for backfilling a uuid on a pre-sync row. Built
// only from QSO content (never the local rowid) so two machines that hold the
// same QSO derive the same uuid. Includes enough fields that distinct contacts
// don't collide.
std::string contentKey(const Qso& q) {
    const char sep = '\x1f';
    return q.call + sep + q.band + sep + q.mode + sep + q.date + sep +
           q.time_on + sep + q.time_off + sep + q.freq + sep + q.rst_sent +
           sep + q.rst_rcvd + sep + q.comment;
}

// Deterministic backfill timestamp from the QSO's own date/time, so the same
// pre-sync row gets the same updated_at on every machine (a no-op first sync).
std::string backfillStamp(const Qso& q) {
    const std::string date = q.date.empty() ? "1970-01-01" : q.date;
    const std::string time = q.time_on.empty() ? "00:00" : q.time_on;
    return date + "T" + time + ":00.000Z";
}

std::string columnText(sqlite3_stmt* stmt, int col) {
    const unsigned char* t = sqlite3_column_text(stmt, col);
    return t ? reinterpret_cast<const char*>(t) : std::string{};
}

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

} // namespace

LogBook::LogBook() {
    newInMemory();
}

LogBook::~LogBook() {
    close();
}

void LogBook::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    cache_.clear();
    path_.clear();
}

bool LogBook::createSchema(sqlite3* db) {
    const char* sql =
        "CREATE TABLE IF NOT EXISTS qsos ("
        "  id       INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  date     TEXT, time_on  TEXT, time_off TEXT, call     TEXT,"
        "  band     TEXT, mode     TEXT, freq     TEXT, rst_sent TEXT,"
        "  rst_rcvd TEXT, name     TEXT, qth      TEXT, locator  TEXT,"
        "  power    TEXT, qsl_sent TEXT, qsl_rcvd TEXT, comment  TEXT,"
        "  lotw_sent TEXT, lotw_sent_date TEXT,"
        "  lotw_rcvd TEXT, lotw_rcvd_date TEXT,"
        "  country TEXT, cq_zone TEXT, itu_zone TEXT, continent TEXT,"
        "  uuid TEXT, updated_at TEXT);";
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        sqlite3_free(err);
        return false;
    }

    // Migrate logbooks created before a column existed: add any missing
    // column. ALTER TABLE fails harmlessly ("duplicate column name") when the
    // column is already present, so the errors are ignored. The two sync
    // columns migrate the same way.
    for (const char* col : kColumns) {
        const std::string alter =
            std::string("ALTER TABLE qsos ADD COLUMN ") + col + " TEXT;";
        char* e = nullptr;
        sqlite3_exec(db, alter.c_str(), nullptr, nullptr, &e);
        sqlite3_free(e);
    }
    for (const char* col : {"uuid", "updated_at"}) {
        const std::string alter =
            std::string("ALTER TABLE qsos ADD COLUMN ") + col + " TEXT;";
        char* e = nullptr;
        sqlite3_exec(db, alter.c_str(), nullptr, nullptr, &e);
        sqlite3_free(e);
    }

    // Sync support: an index for uuid lookups (non-unique — deterministic
    // backfill of two identical QSOs could collide, and a duplicate uuid is
    // harmless to the in-memory merge), a deletion-tombstone table, and a
    // key/value meta table holding this logbook's sync_id.
    const char* aux =
        "CREATE INDEX IF NOT EXISTS idx_qsos_uuid ON qsos(uuid);"
        "CREATE TABLE IF NOT EXISTS tombstones ("
        "  uuid TEXT PRIMARY KEY, deleted_at TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);";
    char* e2 = nullptr;
    sqlite3_exec(db, aux, nullptr, nullptr, &e2);
    sqlite3_free(e2);
    return true;
}

void LogBook::newInMemory() {
    close();
    if (sqlite3_open(":memory:", &db_) != SQLITE_OK) {
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    createSchema(db_);
    reload();
}

bool LogBook::open(const std::string& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    if (!createSchema(db)) {
        sqlite3_close(db);
        return false;
    }
    close();
    db_ = db;
    path_ = path;
    migrateUuids();
    reload();
    return true;
}

bool LogBook::saveAs(const std::string& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    if (!createSchema(db)) {
        sqlite3_close(db);
        return false;
    }

    // Copy every cached QSO into the new database in one transaction,
    // preserving each row's sync identity (uuid/updated_at) so a Save As does
    // not orphan the logbook from its peers.
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    const std::string sql = insertSql();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    for (const auto& q : cache_) {
        const std::string uuid = q.uuid.empty() ? uuidutil::newUuid() : q.uuid;
        const std::string stamp =
            q.updated_at.empty() ? timeutil::utcNowMillisIso() : q.updated_at;
        bindInsertRow(stmt, q, uuid, stamp);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);

    // Carry the sync_id across so the new file is recognised as the same
    // logbook by peers.
    {
        const std::string sid = metaGet("sync_id");
        if (!sid.empty()) {
            sqlite3_stmt* m = nullptr;
            if (sqlite3_prepare_v2(
                    db, "INSERT OR REPLACE INTO meta (key, value) VALUES ('sync_id', ?);",
                    -1, &m, nullptr) == SQLITE_OK) {
                bindText(m, 1, sid);
                sqlite3_step(m);
                sqlite3_finalize(m);
            }
        }
    }

    close();
    db_ = db;
    path_ = path;
    reload();
    return true;
}

void LogBook::reload() {
    cache_.clear();
    if (!db_)
        return;

    const std::string sql = selectSql();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Qso q;
        q.id             = sqlite3_column_int64(stmt, 0);
        q.date           = columnText(stmt, 1);
        q.time_on        = columnText(stmt, 2);
        q.time_off       = columnText(stmt, 3);
        q.call           = columnText(stmt, 4);
        q.band           = columnText(stmt, 5);
        q.mode           = columnText(stmt, 6);
        q.freq           = columnText(stmt, 7);
        q.rst_sent       = columnText(stmt, 8);
        q.rst_rcvd       = columnText(stmt, 9);
        q.name           = columnText(stmt, 10);
        q.qth            = columnText(stmt, 11);
        q.locator        = columnText(stmt, 12);
        q.power          = columnText(stmt, 13);
        q.qsl_sent       = columnText(stmt, 14);
        q.qsl_rcvd       = columnText(stmt, 15);
        q.comment        = columnText(stmt, 16);
        q.lotw_sent      = columnText(stmt, 17);
        q.lotw_sent_date = columnText(stmt, 18);
        q.lotw_rcvd      = columnText(stmt, 19);
        q.lotw_rcvd_date = columnText(stmt, 20);
        q.country        = columnText(stmt, 21);
        q.cq_zone        = columnText(stmt, 22);
        q.itu_zone       = columnText(stmt, 23);
        q.continent      = columnText(stmt, 24);
        q.uuid           = columnText(stmt, 25);
        q.updated_at     = columnText(stmt, 26);
        cache_.push_back(std::move(q));
    }
    sqlite3_finalize(stmt);
}

long LogBook::add(const Qso& q) {
    if (!db_)
        return 0;

    const std::string sql = insertSql();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return 0;

    // Honour a caller-supplied identity (the sync receive path) and otherwise
    // mint a fresh one and stamp the current time.
    const std::string uuid = q.uuid.empty() ? uuidutil::newUuid() : q.uuid;
    const std::string stamp =
        q.updated_at.empty() ? timeutil::utcNowMillisIso() : q.updated_at;
    bindInsertRow(stmt, q, uuid, stamp);

    long id = 0;
    if (sqlite3_step(stmt) == SQLITE_DONE)
        id = static_cast<long>(sqlite3_last_insert_rowid(db_));
    sqlite3_finalize(stmt);

    reload();
    return id;
}

bool LogBook::update(const Qso& q) {
    if (!db_ || q.id == 0)
        return false;

    std::string sql = "UPDATE qsos SET ";
    for (size_t i = 0; i < kColumns.size(); ++i) {
        sql += kColumns[i];
        sql += "=?,";
    }
    sql += "updated_at=? WHERE id=?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    // A local edit always bumps updated_at to now (the caller's stale
    // form-loaded value is ignored) and never touches uuid.
    const auto fields = fieldsOf(q);
    int i = 0;
    for (; i < static_cast<int>(fields.size()); ++i)
        bindText(stmt, i + 1, fields[i]);
    bindText(stmt, i + 1, timeutil::utcNowMillisIso());
    sqlite3_bind_int64(stmt, i + 2, q.id);

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);

    reload();
    return ok;
}

int LogBook::updateBatch(const std::vector<Qso>& qsos) {
    if (!db_ || qsos.empty())
        return 0;

    std::string sql = "UPDATE qsos SET ";
    for (size_t i = 0; i < kColumns.size(); ++i) {
        sql += kColumns[i];
        sql += "=?,";
    }
    sql += "updated_at=? WHERE id=?;";

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return 0;
    }

    const std::string now = timeutil::utcNowMillisIso();
    int count = 0;
    for (const auto& q : qsos) {
        if (q.id == 0)
            continue;
        const auto fields = fieldsOf(q);
        int i = 0;
        for (; i < static_cast<int>(fields.size()); ++i)
            bindText(stmt, i + 1, fields[i]);
        bindText(stmt, i + 1, now);
        sqlite3_bind_int64(stmt, i + 2, q.id);
        if (sqlite3_step(stmt) == SQLITE_DONE)
            ++count;
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);

    reload();
    return count;
}

bool LogBook::remove(long id) {
    return removeTracked(id).ok;
}

RemoveResult LogBook::removeTracked(long id) {
    RemoveResult r;
    if (!db_ || id == 0)
        return r;

    // Find the row's uuid (for the tombstone) before deleting it.
    std::string uuid;
    for (const auto& q : cache_) {
        if (q.id == id) { uuid = q.uuid; break; }
    }

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);

    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM qsos WHERE id=?;", -1, &del,
                           nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return r;
    }
    sqlite3_bind_int64(del, 1, id);
    const bool ok = sqlite3_step(del) == SQLITE_DONE;
    sqlite3_finalize(del);

    const std::string deletedAt = timeutil::utcNowMillisIso();
    if (ok && !uuid.empty()) {
        sqlite3_stmt* tomb = nullptr;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT INTO tombstones (uuid, deleted_at) VALUES (?, ?) "
                "ON CONFLICT(uuid) DO UPDATE SET deleted_at=excluded.deleted_at "
                "WHERE excluded.deleted_at > tombstones.deleted_at;",
                -1, &tomb, nullptr) == SQLITE_OK) {
            bindText(tomb, 1, uuid);
            bindText(tomb, 2, deletedAt);
            sqlite3_step(tomb);
            sqlite3_finalize(tomb);
        }
    }

    sqlite3_exec(db_, ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);

    reload();
    r.ok = ok;
    r.uuid = uuid;
    r.deleted_at = deletedAt;
    return r;
}

int LogBook::insertAll(const std::vector<Qso>& parsed) {
    if (parsed.empty() || !db_)
        return 0;

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    const std::string sql = insertSql();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    const std::string now = timeutil::utcNowMillisIso();
    int count = 0;
    for (const auto& q : parsed) {
        // Imported records get a fresh identity unless one was supplied.
        const std::string uuid = q.uuid.empty() ? uuidutil::newUuid() : q.uuid;
        const std::string stamp = q.updated_at.empty() ? now : q.updated_at;
        bindInsertRow(stmt, q, uuid, stamp);
        if (sqlite3_step(stmt) == SQLITE_DONE)
            ++count;
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);

    reload();
    return count;
}

int LogBook::importAdif(const std::string& adifText) {
    return insertAll(adif::parse(adifText));
}

int LogBook::importXlog(const std::string& xlogText) {
    return insertAll(xlog::parse(xlogText));
}

std::string LogBook::exportAdif() const {
    return adif::write(cache_);
}

std::optional<Qso> LogBook::findDuplicate(const Qso& q, long excludeId,
                                          DupeRule rule) const {
    if (q.call.empty())
        return std::nullopt;

    // cache_ is ordered by date then time; scan from newest so the first hit
    // is the most recent duplicate.
    for (auto it = cache_.rbegin(); it != cache_.rend(); ++it) {
        const Qso& e = *it;
        if (e.id == excludeId)
            continue;
        if (!iequals(e.call, q.call))
            continue;
        switch (rule) {
            case DupeRule::CallBandModeDay:
                if (e.band == q.band && iequals(e.mode, q.mode) && e.date == q.date)
                    return e;
                break;
            case DupeRule::CallBandMode:
                if (e.band == q.band && iequals(e.mode, q.mode))
                    return e;
                break;
            case DupeRule::Call:
                return e;
        }
    }
    return std::nullopt;
}

std::vector<Qso> LogBook::qsosNotLotwSent() const {
    std::vector<Qso> out;
    for (const auto& q : cache_)
        if (q.lotw_sent != "Y")
            out.push_back(q);
    return out;
}

void LogBook::markLotwSent(const std::vector<long>& ids, const std::string& date) {
    // Collect copies first: update() reloads cache_, which would otherwise
    // invalidate the iteration below.
    std::vector<Qso> updates;
    for (long id : ids) {
        for (const auto& q : cache_) {
            if (q.id == id && q.lotw_sent != "Y") {
                Qso u = q;
                u.lotw_sent = "Y";
                u.lotw_sent_date = date;
                updates.push_back(std::move(u));
                break;
            }
        }
    }
    for (const auto& u : updates)
        update(u);
}

namespace {

// Minutes since midnight for "HH:MM" (or "HHMM"); -1 if not parseable.
int minutesOfDay(const std::string& t) {
    int h = 0, m = 0;
    if (std::sscanf(t.c_str(), "%d:%d", &h, &m) == 2 ||
        (t.size() >= 4 && std::sscanf(t.c_str(), "%2d%2d", &h, &m) == 2))
        return h * 60 + m;
    return -1;
}

} // namespace

int LogBook::applyLotwConfirmations(const std::vector<Qso>& confirmed) {
    constexpr int kTimeToleranceMin = 30;

    // Collect the local QSOs to confirm first (update() reloads cache_), and
    // never match the same local QSO against two confirmations.
    std::vector<Qso> updates;
    std::set<long> used;

    for (const auto& c : confirmed) {
        for (const auto& e : cache_) {
            if (e.lotw_rcvd == "Y" || used.count(e.id))
                continue;
            if (!iequals(e.call, c.call) || e.band != c.band ||
                !iequals(e.mode, c.mode) || e.date != c.date)
                continue;

            // If both sides carry a time, require it within tolerance.
            const int et = minutesOfDay(e.time_on);
            const int ct = minutesOfDay(c.time_on);
            if (et >= 0 && ct >= 0 && std::abs(et - ct) > kTimeToleranceMin)
                continue;

            Qso u = e;
            u.lotw_rcvd = "Y";
            u.lotw_rcvd_date = !c.lotw_rcvd_date.empty() ? c.lotw_rcvd_date : c.date;
            updates.push_back(std::move(u));
            used.insert(e.id);
            break;  // one local QSO per confirmation
        }
    }
    for (const auto& u : updates)
        update(u);
    return static_cast<int>(updates.size());
}

// --- Logbook sync ---

void LogBook::migrateUuids() {
    if (!db_)
        return;

    // Collect rows lacking a uuid (added by the column migration as NULL on
    // pre-sync logbooks).
    std::vector<Qso> rows;
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(
                db_,
                "SELECT id,date,time_on,time_off,call,band,mode,freq,rst_sent,"
                "rst_rcvd,comment FROM qsos WHERE uuid IS NULL OR uuid='';",
                -1, &stmt, nullptr) != SQLITE_OK)
            return;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Qso q;
            q.id       = sqlite3_column_int64(stmt, 0);
            q.date     = columnText(stmt, 1);
            q.time_on  = columnText(stmt, 2);
            q.time_off = columnText(stmt, 3);
            q.call     = columnText(stmt, 4);
            q.band     = columnText(stmt, 5);
            q.mode     = columnText(stmt, 6);
            q.freq     = columnText(stmt, 7);
            q.rst_sent = columnText(stmt, 8);
            q.rst_rcvd = columnText(stmt, 9);
            q.comment  = columnText(stmt, 10);
            rows.push_back(std::move(q));
        }
        sqlite3_finalize(stmt);
    }
    if (rows.empty())
        return;

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_stmt* up = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "UPDATE qsos SET uuid=?, updated_at=? WHERE id=?;",
                           -1, &up, nullptr) == SQLITE_OK) {
        for (const auto& q : rows) {
            bindText(up, 1, uuidutil::uuidV5(contentKey(q)));
            bindText(up, 2, backfillStamp(q));
            sqlite3_bind_int64(up, 3, q.id);
            sqlite3_step(up);
            sqlite3_reset(up);
        }
        sqlite3_finalize(up);
    }
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
}

std::string LogBook::metaGet(const std::string& key) const {
    if (!db_)
        return {};
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT value FROM meta WHERE key=?;", -1, &stmt,
                           nullptr) != SQLITE_OK)
        return {};
    bindText(stmt, 1, key);
    std::string out;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        out = columnText(stmt, 0);
    sqlite3_finalize(stmt);
    return out;
}

void LogBook::metaSet(const std::string& key, const std::string& value) {
    if (!db_)
        return;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_, "INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?);", -1,
            &stmt, nullptr) != SQLITE_OK)
        return;
    bindText(stmt, 1, key);
    bindText(stmt, 2, value);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::string LogBook::ensureSyncId() {
    std::string id = metaGet("sync_id");
    if (id.empty()) {
        id = uuidutil::newUuid();
        metaSet("sync_id", id);
    }
    return id;
}

void LogBook::setSyncId(const std::string& id) {
    metaSet("sync_id", id);
}

SyncManifest LogBook::syncManifest() const {
    SyncManifest m;
    m.records.reserve(cache_.size());
    for (const auto& q : cache_)
        if (!q.uuid.empty())
            m.records.push_back({q.uuid, q.updated_at});

    if (db_) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT uuid, deleted_at FROM tombstones;",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW)
                m.tombstones.push_back({columnText(stmt, 0), columnText(stmt, 1)});
            sqlite3_finalize(stmt);
        }
    }
    return m;
}

std::vector<Qso> LogBook::recordsByUuids(
    const std::vector<std::string>& uuids) const {
    std::unordered_set<std::string> want(uuids.begin(), uuids.end());
    std::vector<Qso> out;
    for (const auto& q : cache_)
        if (!q.uuid.empty() && want.count(q.uuid))
            out.push_back(q);
    return out;
}

MergeResult LogBook::applyRemote(const std::vector<Qso>& records,
                                 const std::vector<SyncEntry>& tombstones,
                                 const std::string& localNodeId,
                                 const std::string& peerNodeId) {
    MergeResult res;
    if (!db_)
        return res;

    // Local indexes (built from the in-memory cache + tombstone table).
    std::unordered_map<std::string, std::pair<long, std::string>> live;  // uuid -> (id, updated_at)
    for (const auto& q : cache_)
        if (!q.uuid.empty())
            live[q.uuid] = {q.id, q.updated_at};

    std::unordered_map<std::string, std::string> tomb;  // uuid -> deleted_at
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT uuid, deleted_at FROM tombstones;",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW)
                tomb[columnText(stmt, 0)] = columnText(stmt, 1);
            sqlite3_finalize(stmt);
        }
    }

    // Tie-break for equal timestamps: the higher node id wins, deterministically
    // on both peers.
    const bool peerWinsTies = peerNodeId > localNodeId;

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);

    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(db_, insertSql().c_str(), -1, &ins, nullptr);

    std::string updSql = "UPDATE qsos SET ";
    for (size_t i = 0; i < kColumns.size(); ++i) {
        updSql += kColumns[i];
        updSql += "=?,";
    }
    updSql += "updated_at=? WHERE uuid=?;";
    sqlite3_stmt* upd = nullptr;
    sqlite3_prepare_v2(db_, updSql.c_str(), -1, &upd, nullptr);

    sqlite3_stmt* del = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM qsos WHERE uuid=?;", -1, &del, nullptr);

    sqlite3_stmt* tup = nullptr;
    sqlite3_prepare_v2(
        db_,
        "INSERT INTO tombstones (uuid, deleted_at) VALUES (?, ?) "
        "ON CONFLICT(uuid) DO UPDATE SET deleted_at=excluded.deleted_at "
        "WHERE excluded.deleted_at > tombstones.deleted_at;",
        -1, &tup, nullptr);

    sqlite3_stmt* tdel = nullptr;
    sqlite3_prepare_v2(db_, "DELETE FROM tombstones WHERE uuid=?;", -1, &tdel,
                       nullptr);

    // Clears a tombstone once a strictly-newer record has resurrected its uuid,
    // so the row isn't advertised as both live and deleted.
    auto clearTomb = [&](const std::string& uuid) {
        bindText(tdel, 1, uuid);
        sqlite3_step(tdel);
        sqlite3_reset(tdel);
        tomb.erase(uuid);
    };

    // Incoming live records. Ties (equal timestamp) are resolved as "delete
    // wins" against a tombstone, and by node id against a live row.
    for (const auto& r : records) {
        if (r.uuid.empty()) { ++res.skipped; continue; }

        const auto t = tomb.find(r.uuid);
        if (t != tomb.end() && t->second >= r.updated_at) {
            ++res.skipped;  // a known deletion is newer or equal — delete wins
            continue;
        }

        const auto l = live.find(r.uuid);
        if (l == live.end()) {
            bindInsertRow(ins, r, r.uuid, r.updated_at);
            sqlite3_step(ins);
            sqlite3_reset(ins);
            if (t != tomb.end()) clearTomb(r.uuid);  // resurrected
            ++res.inserted;
        } else {
            const std::string& localStamp = l->second.second;
            const bool remoteWins =
                r.updated_at > localStamp ||
                (r.updated_at == localStamp && peerWinsTies);
            if (remoteWins) {
                const auto fields = fieldsOf(r);
                int i = 0;
                for (; i < static_cast<int>(fields.size()); ++i)
                    bindText(upd, i + 1, fields[i]);
                bindText(upd, i + 1, r.updated_at);
                bindText(upd, i + 2, r.uuid);
                sqlite3_step(upd);
                sqlite3_reset(upd);
                ++res.updated;
            } else {
                ++res.skipped;
            }
        }
    }

    // Incoming tombstones.
    for (const auto& ts : tombstones) {
        if (ts.uuid.empty())
            continue;
        bindText(tup, 1, ts.uuid);
        bindText(tup, 2, ts.stamp);
        sqlite3_step(tup);
        sqlite3_reset(tup);

        const auto l = live.find(ts.uuid);
        if (l != live.end() && ts.stamp >= l->second.second) {
            bindText(del, 1, ts.uuid);
            sqlite3_step(del);
            sqlite3_reset(del);
            ++res.deleted;
        }
        // else: a strictly-newer local edit survives (resurrect); it will
        // re-propagate to the peer on the next exchange.
    }

    sqlite3_finalize(ins);
    sqlite3_finalize(upd);
    sqlite3_finalize(del);
    sqlite3_finalize(tup);
    sqlite3_finalize(tdel);
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);

    reload();
    return res;
}
