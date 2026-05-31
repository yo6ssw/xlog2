#pragma once

#include <glibmm/dispatcher.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The interesting fields of a QRZ.com callsign record, mapped to the parts of
// a Qso we can prefill.
struct QrzResult {
    std::string call;
    std::string name;     // "First Last"
    std::string qth;      // "City, State"
    std::string locator;  // Maidenhead grid
    std::string country;

    // Every element QRZ returned for the callsign, in document order, for
    // display in the lookup popup (the named fields above are derived from it).
    std::vector<std::pair<std::string, std::string>> fields;
};

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
    std::function<void(const QrzResult& result, const std::string& error)> onResult;

    QrzClient();
    ~QrzClient();
    QrzClient(const QrzClient&)            = delete;
    QrzClient& operator=(const QrzClient&) = delete;

    bool isBusy() const { return busy_.load(); }

    // Starts an async lookup. Returns false if a lookup is already running.
    bool lookup(const std::string& user, const std::string& password,
                const std::string& callsign);

private:
    void worker(std::string user, std::string password, std::string callsign);
    void onDispatch();  // UI thread

    std::string sessionKey_;  // touched only on the worker thread

    std::thread       thread_;
    std::atomic<bool> busy_{false};

    std::mutex  mutex_;
    QrzResult   result_;
    std::string error_;
    bool        hasResult_ = false;

    Glib::Dispatcher dispatcher_;
};
