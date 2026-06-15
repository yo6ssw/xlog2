#include "QrzCache.h"

#include "StrUtil.h"

#include <sqlite3.h>

#include <ctime>

namespace {

// Serialise the (key,value) field list into one self-delimiting string:
// each pair is "<klen>:<key><vlen>:<value>". Length prefixes make it safe for
// any content (newlines, colons) in the values.
std::string serializeFields(
    const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (const auto& [k, v] : fields) {
        out += std::to_string(k.size()) + ':' + k;
        out += std::to_string(v.size()) + ':' + v;
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> deserializeFields(const std::string& s) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = 0;
    auto readChunk = [&](std::string& dst) -> bool {
        const std::size_t colon = s.find(':', i);
        if (colon == std::string::npos)
            return false;
        std::size_t len = 0;
        try {
            len = static_cast<std::size_t>(std::stoul(s.substr(i, colon - i)));
        } catch (const std::exception&) {
            return false;
        }
        const std::size_t start = colon + 1;
        if (start + len > s.size())
            return false;
        dst = s.substr(start, len);
        i = start + len;
        return true;
    };
    while (i < s.size()) {
        std::string k, v;
        if (!readChunk(k) || !readChunk(v))
            break;
        out.emplace_back(std::move(k), std::move(v));
    }
    return out;
}

std::string columnText(sqlite3_stmt* stmt, int col) {
    const unsigned char* t = sqlite3_column_text(stmt, col);
    return t ? reinterpret_cast<const char*>(t) : std::string{};
}

void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(),
                      static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

}  // namespace

QrzCache::~QrzCache() {
    if (db_)
        sqlite3_close(db_);
}

bool QrzCache::open(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_ && path == path_)
        return true;  // already open at this path
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    path_.clear();
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return false;
    }
    const char* schema =
        "CREATE TABLE IF NOT EXISTS qrz_cache("
        "call TEXT PRIMARY KEY,"
        "fetched_at INTEGER NOT NULL,"
        "name TEXT, qth TEXT, locator TEXT, country TEXT,"
        "fields TEXT)";
    char* err = nullptr;
    if (sqlite3_exec(db_, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        sqlite3_free(err);
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    path_ = path;
    return true;
}

std::optional<QrzResult> QrzCache::get(const std::string& call, int maxAgeDays) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || maxAgeDays <= 0)
        return std::nullopt;
    const std::string key = strutil::toUpper(call);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT fetched_at,name,qth,locator,country,fields FROM qrz_cache WHERE call=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;
    bindText(stmt, 1, key);
    std::optional<QrzResult> out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const long long fetchedAt = sqlite3_column_int64(stmt, 0);
        const long long now = static_cast<long long>(std::time(nullptr));
        if (fetchedAt + static_cast<long long>(maxAgeDays) * 86400 >= now) {
            QrzResult r;
            r.call    = key;
            r.name    = columnText(stmt, 1);
            r.qth     = columnText(stmt, 2);
            r.locator = columnText(stmt, 3);
            r.country = columnText(stmt, 4);
            r.fields  = deserializeFields(columnText(stmt, 5));
            out = std::move(r);
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

void QrzCache::put(const QrzResult& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || r.call.empty())
        return;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT OR REPLACE INTO qrz_cache"
        "(call,fetched_at,name,qth,locator,country,fields) VALUES(?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    bindText(stmt, 1, strutil::toUpper(r.call));
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(std::time(nullptr)));
    bindText(stmt, 3, r.name);
    bindText(stmt, 4, r.qth);
    bindText(stmt, 5, r.locator);
    bindText(stmt, 6, r.country);
    bindText(stmt, 7, serializeFields(r.fields));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}
