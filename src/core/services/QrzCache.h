// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <mutex>
#include <optional>
#include <string>

#include "QrzResult.h"

struct sqlite3;

// A persistent on-disk cache of QRZ.com callsign records, so a repeated lookup
// of the same callsign needn't hit the network. Backed by a tiny SQLite file
// (one row per callsign, stamped with the fetch time); entries older than the
// configured lifetime are treated as misses. Toolkit-neutral; all access is
// mutex-guarded so the QrzClient worker thread and the UI thread
// (configuration) can touch it safely.
class QrzCache {
 public:
  QrzCache() = default;
  ~QrzCache();
  QrzCache(const QrzCache&) = delete;
  QrzCache& operator=(const QrzCache&) = delete;

  // Open (or reopen) the cache database at `path`, creating the schema. A no-op
  // if already open at the same path. Returns false if the file can't be opened
  // (the cache then silently behaves as empty).
  bool open(const std::string& path);

  // The cached record for `call` if present and younger than `maxAgeDays`.
  // maxAgeDays <= 0 disables the cache (always returns nullopt).
  std::optional<QrzResult> get(const std::string& call, int maxAgeDays);

  // Insert or replace the record for its callsign, stamped with the current
  // time.
  void put(const QrzResult& result);

 private:
  std::mutex mutex_;
  sqlite3* db_ = nullptr;
  std::string path_;
};
