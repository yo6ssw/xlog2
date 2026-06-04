#include "LogBook.h"

#include "Adif.h"
#include "Xlog.h"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>

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

// "INSERT INTO qsos (c1,c2,...) VALUES (?,?,...);" built from kColumns so the
// three insert sites cannot drift out of sync.
std::string insertSql() {
    std::string cols, marks;
    for (size_t i = 0; i < kColumns.size(); ++i) {
        if (i) { cols += ','; marks += ','; }
        cols += kColumns[i];
        marks += '?';
    }
    return "INSERT INTO qsos (" + cols + ") VALUES (" + marks + ");";
}

// "SELECT id,c1,c2,... FROM qsos ORDER BY date,time_on,id;"
std::string selectSql() {
    std::string cols = "id";
    for (const char* c : kColumns) {
        cols += ',';
        cols += c;
    }
    return "SELECT " + cols + " FROM qsos ORDER BY date, time_on, id;";
}

void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(),
                      static_cast<int>(value.size()), SQLITE_TRANSIENT);
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
        "  country TEXT, cq_zone TEXT, itu_zone TEXT, continent TEXT);";
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        sqlite3_free(err);
        return false;
    }

    // Migrate logbooks created before a column existed: add any missing
    // column. ALTER TABLE fails harmlessly ("duplicate column name") when the
    // column is already present, so the errors are ignored.
    for (const char* col : kColumns) {
        const std::string alter =
            std::string("ALTER TABLE qsos ADD COLUMN ") + col + " TEXT;";
        char* e = nullptr;
        sqlite3_exec(db, alter.c_str(), nullptr, nullptr, &e);
        sqlite3_free(e);
    }
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

    // Copy every cached QSO into the new database in one transaction.
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    const std::string sql = insertSql();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    for (const auto& q : cache_) {
        const auto fields = fieldsOf(q);
        for (int i = 0; i < static_cast<int>(fields.size()); ++i)
            bindText(stmt, i + 1, fields[i]);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);

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

    const auto fields = fieldsOf(q);
    for (int i = 0; i < static_cast<int>(fields.size()); ++i)
        bindText(stmt, i + 1, fields[i]);

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
        sql += "=?";
        if (i + 1 < kColumns.size())
            sql += ",";
    }
    sql += " WHERE id=?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    const auto fields = fieldsOf(q);
    for (int i = 0; i < static_cast<int>(fields.size()); ++i)
        bindText(stmt, i + 1, fields[i]);
    sqlite3_bind_int64(stmt, static_cast<int>(fields.size()) + 1, q.id);

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
        sql += "=?";
        if (i + 1 < kColumns.size())
            sql += ",";
    }
    sql += " WHERE id=?;";

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return 0;
    }

    int count = 0;
    for (const auto& q : qsos) {
        if (q.id == 0)
            continue;
        const auto fields = fieldsOf(q);
        for (int i = 0; i < static_cast<int>(fields.size()); ++i)
            bindText(stmt, i + 1, fields[i]);
        sqlite3_bind_int64(stmt, static_cast<int>(fields.size()) + 1, q.id);
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
    if (!db_ || id == 0)
        return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM qsos WHERE id=?;", -1, &stmt,
                           nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(stmt, 1, id);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);

    reload();
    return ok;
}

int LogBook::insertAll(const std::vector<Qso>& parsed) {
    if (parsed.empty() || !db_)
        return 0;

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    const std::string sql = insertSql();
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    int count = 0;
    for (const auto& q : parsed) {
        const auto fields = fieldsOf(q);
        for (int i = 0; i < static_cast<int>(fields.size()); ++i)
            bindText(stmt, i + 1, fields[i]);
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
