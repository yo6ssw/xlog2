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

// One entry in a sync manifest: a stable uuid plus its timestamp. For live
// records `stamp` is the QSO's updated_at; for tombstones it is the deleted_at.
struct SyncEntry {
    std::string uuid;
    std::string stamp;
};

// A compact summary of a logbook's syncable state: every live record's
// (uuid, updated_at) and every tombstone's (uuid, deleted_at). Cheap to build
// from the in-memory cache.
struct SyncManifest {
    std::vector<SyncEntry> records;
    std::vector<SyncEntry> tombstones;
};

// Outcome of merging a remote delta, for status reporting.
struct MergeResult {
    int inserted = 0;
    int updated  = 0;
    int deleted  = 0;
    int skipped  = 0;
};

// Outcome of a tracked delete: the deleted row's uuid + tombstone timestamp,
// so the caller can propagate the deletion to peers.
struct RemoveResult {
    bool        ok = false;
    std::string uuid;
    std::string deleted_at;
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

    // Like remove(), but also records a tombstone and reports the deleted row's
    // uuid + tombstone timestamp so the deletion can be propagated to peers.
    RemoveResult removeTracked(long id);

    // Updates many QSOs in a single transaction with one reload. Returns the
    // number written.
    int updateBatch(const std::vector<Qso>& qsos);

    // Imports ADIF text, inserting each record. Returns the number added.
    int importAdif(const std::string& adifText);

    // Imports native xlog ("Flog") text, inserting each record. Returns the
    // number added.
    int importXlog(const std::string& xlogText);

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

    // --- Logbook sync ---

    // A summary of this logbook's syncable state (live records + tombstones).
    SyncManifest syncManifest() const;

    // Full QSO records for the given uuids (those present in the cache).
    std::vector<Qso> recordsByUuids(const std::vector<std::string>& uuids) const;

    // Merge a remote delta into this logbook in a single transaction with one
    // reload. Per uuid the newer updated_at wins; a tombstone whose deleted_at
    // is newer deletes; equal timestamps are broken by node id (higher wins).
    // Identity is matched by uuid only (the duplicate UI is bypassed). The
    // incoming records carry their own uuid/updated_at, which are preserved.
    MergeResult applyRemote(const std::vector<Qso>& records,
                            const std::vector<SyncEntry>& tombstones,
                            const std::string& localNodeId,
                            const std::string& peerNodeId);

    // The per-logbook sync identity, minted+persisted on first use. Two peers
    // confirm they are syncing the same logbook by comparing this.
    std::string ensureSyncId();
    void        setSyncId(const std::string& id);

    // The stored sync_id without minting one (empty if never paired). Used in
    // the handshake so two fresh logbooks can agree on an id rather than each
    // minting a distinct one and mismatching.
    std::string syncId() const { return metaGet("sync_id"); }

private:
    bool createSchema(sqlite3* db);
    void reload();
    void close();

    // Backfills uuid/updated_at on rows created before sync existed, using a
    // deterministic content hash so independent machines agree. Run on open().
    void migrateUuids();

    std::string metaGet(const std::string& key) const;
    void        metaSet(const std::string& key, const std::string& value);

    // Inserts every QSO in one transaction, then reloads. Returns the count.
    int insertAll(const std::vector<Qso>& qsos);

    sqlite3*         db_ = nullptr;
    std::string      path_;   // empty => in-memory
    std::vector<Qso> cache_;
};
