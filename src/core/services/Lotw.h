// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "IUiDispatcher.h"
#include "ProcessRunner.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Talks to ARRL's Logbook of The World:
//  - downloads the confirmation report over HTTPS with libcurl on a worker
//    thread, delivering the ADIF body to the UI thread via Glib::Dispatcher
//    (same shape as RigController);
//  - uploads a signed ADIF by spawning ARRL's `tqsl` tool asynchronously
//    (tqsl manages the operator's certificate; we never see it).
class LotwClient {
public:
    // Called on the UI thread when a download finishes. On success `error` is
    // empty and `adif` holds the report body; otherwise `error` is set.
    std::function<void(const std::string& adif, const std::string& error)> onDownloadDone;
    // Called on the UI thread when a tqsl upload finishes.
    std::function<void(bool ok, const std::string& message)> onUploadDone;

    explicit LotwClient(IUiDispatcher& ui);
    ~LotwClient();
    LotwClient(const LotwClient&)            = delete;
    LotwClient& operator=(const LotwClient&) = delete;

    bool isBusy() const { return busy_.load(); }

    // Starts an async download of confirmations. `since` (YYYY-MM-DD) may be
    // empty for "everything". Returns false if a download is already running.
    bool downloadConfirmations(const std::string& user, const std::string& password,
                               const std::string& since);

    // Signs and uploads an ADIF file via tqsl (async). `stationLocation` may be
    // empty to let tqsl use its default/configured location.
    void uploadAdifFile(const std::string& tqslPath,
                        const std::string& stationLocation,
                        const std::string& adifPath);

private:
    void worker(std::string url);
    void deliverDownload();  // UI thread

    IUiDispatcher&    ui_;
    ProcessRunner     uploader_;     // tqsl spawn (async, result on the UI thread)
    std::thread       thread_;
    std::atomic<bool> busy_{false};

    std::mutex  mutex_;
    std::string body_;
    std::string error_;
    bool        hasResult_ = false;

    // Liveness token for posted download closures (see RigController).
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
