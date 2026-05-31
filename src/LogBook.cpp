#include "LogBook.h"

#include "Adif.h"

#include <sqlite3.h>

#include <array>

namespace {

// Column order shared by INSERT, UPDATE and SELECT. id is handled separately.
constexpr std::array<const char*, 16> kColumns = {
    "date", "time_on", "time_off", "call", "band", "mode", "freq",
    "rst_sent", "rst_rcvd", "name", "qth", "locator", "power",
    "qsl_sent", "qsl_rcvd", "comment",
};

std::array<std::string, 16> fieldsOf(const Qso& q) {
    return {q.date, q.time_on, q.time_off, q.call, q.band, q.mode, q.freq,
            q.rst_sent, q.rst_rcvd, q.name, q.qth, q.locator, q.power,
            q.qsl_sent, q.qsl_rcvd, q.comment};
}

void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(),
                      static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string columnText(sqlite3_stmt* stmt, int col) {
    const unsigned char* t = sqlite3_column_text(stmt, col);
    return t ? reinterpret_cast<const char*>(t) : std::string{};
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
        "  power    TEXT, qsl_sent TEXT, qsl_rcvd TEXT, comment  TEXT);";
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        sqlite3_free(err);
        return false;
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
    const char* sql =
        "INSERT INTO qsos (date,time_on,time_off,call,band,mode,freq,"
        "rst_sent,rst_rcvd,name,qth,locator,power,qsl_sent,qsl_rcvd,comment) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
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

    const char* sql =
        "SELECT id,date,time_on,time_off,call,band,mode,freq,rst_sent,"
        "rst_rcvd,name,qth,locator,power,qsl_sent,qsl_rcvd,comment "
        "FROM qsos ORDER BY date, time_on, id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
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
        q.name     = columnText(stmt, 10);
        q.qth      = columnText(stmt, 11);
        q.locator  = columnText(stmt, 12);
        q.power    = columnText(stmt, 13);
        q.qsl_sent = columnText(stmt, 14);
        q.qsl_rcvd = columnText(stmt, 15);
        q.comment  = columnText(stmt, 16);
        cache_.push_back(std::move(q));
    }
    sqlite3_finalize(stmt);
}

long LogBook::add(const Qso& q) {
    if (!db_)
        return 0;

    const char* sql =
        "INSERT INTO qsos (date,time_on,time_off,call,band,mode,freq,"
        "rst_sent,rst_rcvd,name,qth,locator,power,qsl_sent,qsl_rcvd,comment) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
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

int LogBook::importAdif(const std::string& adifText) {
    const std::vector<Qso> parsed = adif::parse(adifText);
    if (parsed.empty() || !db_)
        return 0;

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    const char* sql =
        "INSERT INTO qsos (date,time_on,time_off,call,band,mode,freq,"
        "rst_sent,rst_rcvd,name,qth,locator,power,qsl_sent,qsl_rcvd,comment) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
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

std::string LogBook::exportAdif() const {
    return adif::write(cache_);
}
