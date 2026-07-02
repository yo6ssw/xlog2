// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "IUiDispatcher.h"
#include "DxSpot.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Connects to a DX cluster (telnet) and streams DX spots.
//
// A worker thread owns a blocking POSIX socket: it resolves + connects, reads
// lines, and writes the login callsign / user commands. Decoded spots, raw
// lines and status changes are marshalled to the UI thread via the injected
// dispatcher, so every callback fires on the UI thread. A self-pipe wakes the
// worker for outbound commands and for disconnect.
class DxCluster {
public:
    std::function<void(const DxSpot&)>      onSpot;    // a parsed DX spot
    std::function<void(const std::string&)> onLine;    // every raw line (console)
    std::function<void(const std::string&)> onStatus;  // connection state/errors

    explicit DxCluster(IUiDispatcher& ui) : ui_(ui) {}
    ~DxCluster();
    DxCluster(const DxCluster&)            = delete;
    DxCluster& operator=(const DxCluster&) = delete;

    void connectTo(const std::string& host, int port, const std::string& loginCall);
    void disconnect();
    bool isConnected() const { return connected_.load(); }
    void sendCommand(const std::string& line);  // CRLF appended

private:
    void worker(std::string host, int port, std::string loginCall);
    void handleLine(std::string line, bool& loggedIn, const std::string& loginCall);
    void wake();  // poke the self-pipe so the worker re-checks its queue/stop flag

    void postStatus(const std::string& s);
    void postLine(const std::string& s);
    void postSpot(const DxSpot& s);

    IUiDispatcher&    ui_;
    int               wake_[2] = {-1, -1};
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread       thread_;

    std::mutex               outMutex_;
    std::vector<std::string> outbound_;  // commands queued from the UI thread

    // Liveness token for posted closures (see RigController). Recreated on each
    // connect so late callbacks from a previous session are dropped.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

namespace dxcluster {
// Parse a single cluster line into a spot, or nullopt if it isn't a "DX de"
// line. Exposed (not anonymous) so it can be unit-tested without giomm.
std::optional<DxSpot> parseSpot(const std::string& line);
}  // namespace dxcluster
