// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "IUiDispatcher.h"
#include "QrzCache.h"
#include "QrzResult.h"

// Looks up callsign details via QRZ.com's XML data service.
//
// QRZ requires a session: we log in with the user's username/password to get a
// session key, then query callsigns with it. The key is reused across lookups
// and silently refreshed if the server reports it expired. All blocking HTTP
// happens on a worker thread (libcurl) and the parsed result is marshalled back
// to the UI thread via Glib::Dispatcher — the same shape as LotwClient.
class QrzClient {
 public:
  // Called on the UI thread when a lookup finishes. On success `error` is
  // empty and `result` holds the record; otherwise `error` describes why.
  std::function<void(const QrzResult& result, const std::string& error)>
      onResult;

  // Bulk locator-fill outputs (UI thread). onFillProgress fires periodically;
  // onFillResult fires once at the end with every callsign for which a
  // non-empty locator was found (cache or network), plus a hit/fetch summary
  // and an error string set only if the whole operation failed (e.g. login
  // rejected).
  std::function<void(int done, int total)> onFillProgress;
  std::function<void(
      const std::vector<std::pair<std::string, std::string>>& callLocators,
      int fromCache, int fetched, const std::string& error)>
      onFillResult;

  explicit QrzClient(IUiDispatcher& ui);
  ~QrzClient();
  QrzClient(const QrzClient&) = delete;
  QrzClient& operator=(const QrzClient&) = delete;

  bool isBusy() const { return busy_.load(); }

  // Point the on-disk result cache at `path` and set its lifetime in days
  // (<= 0 disables caching). Call before lookups; safe to call again to change
  // the lifetime. Every lookup checks this cache before hitting the network.
  void setCache(const std::string& path, int lifetimeDays);

  // A cache-only lookup (no network): the fresh cached record for `callsign`,
  // or nullopt. Thread-safe (QrzCache is mutex-guarded). Used to answer a
  // peer's distributed-cache query.
  std::optional<QrzResult> cachedLookup(const std::string& callsign);

  // Insert a record into the local cache (e.g. one obtained from a peer).
  void cachePut(const QrzResult& result);

  // An async source consulted between the local cache and the network: given a
  // callsign, it eventually calls `reply` once on the UI thread with a record
  // (cache it + deliver) or nullopt (fall through to qrz.com). Wired by the
  // shell to the mesh peer query; unset => the network is used directly.
  using PeerResolver =
      std::function<void(const std::string& callsign,
                         std::function<void(std::optional<QrzResult>)> reply)>;
  void setPeerResolver(PeerResolver resolver) {
    peerResolver_ = std::move(resolver);
  }

  // Starts an async lookup: local cache, then peers (if a resolver is set),
  // then qrz.com. Returns false if a lookup is already running.
  bool lookup(const std::string& user, const std::string& password,
              const std::string& callsign);

  // Starts an async bulk locator fill over `callsigns` (cache-first, network on
  // miss). Returns false if an operation is already running.
  bool fillLocators(const std::string& user, const std::string& password,
                    const std::vector<std::string>& callsigns);

 private:
  void worker(std::string user, std::string password, std::string callsign);
  void fillWorker(std::string user, std::string password,
                  std::vector<std::string> callsigns);
  void deliverResult();  // UI thread
  void deliverFill();    // UI thread

  // Launch the network worker for the lookup currently in flight (params saved
  // in net_). The cache and peers have already been consulted.
  void startNetwork();
  // Deliver an already-resolved result (cache or peer hit) on the UI thread,
  // posted so onResult always fires after lookup() returns (the enrich queue
  // relies on this ordering).
  void deliverResolved(QrzResult result, std::string error);

  // Worker-thread HTTP helpers (use/refresh sessionKey_ via the curl handle).
  bool doLogin(void* curl, const std::string& user, const std::string& password,
               std::string& error);
  // Fetch + parse one callsign (lazy login, stale-key retry). On a network or
  // "no record" failure, returns an empty-call result and sets `error`.
  QrzResult fetchOne(void* curl, const std::string& user,
                     const std::string& password, const std::string& callsign,
                     std::string& error);

  IUiDispatcher& ui_;
  std::string sessionKey_;  // touched only on the worker thread

  std::thread thread_;
  std::atomic<bool> busy_{false};

  QrzCache cache_;
  std::atomic<int> cacheDays_{365};

  PeerResolver peerResolver_;
  // Params for the network fallback of the in-flight lookup (set on the UI
  // thread in lookup(), read when starting the worker).
  struct {
    std::string user, password, callsign;
  } net_;

  std::mutex mutex_;
  QrzResult result_;
  std::string error_;
  bool hasResult_ = false;

  // Bulk locator-fill delivery (guarded by mutex_).
  std::vector<std::pair<std::string, std::string>> fillResults_;
  int fillFromCache_ = 0;
  int fillFetched_ = 0;
  std::string fillError_;
  bool hasFill_ = false;

  // Liveness token for posted closures (see RigController).
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
