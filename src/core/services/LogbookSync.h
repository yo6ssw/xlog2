#pragma once

#include "IUiDispatcher.h"
#include "SyncProtocol.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Peer-to-peer TCP transport for logbook sync. One worker thread owns the
// socket and does *only* I/O + frame (de)serialization; every decoded message
// and state change is marshalled to the UI thread via the injected dispatcher.
// All logbook access lives in SyncCoordinator on the UI thread.
//
// Two roles form a pair: one peer Listens (accepts a single connection), the
// other Connects (and auto-reconnects). Built on the same blocking-POSIX +
// self-pipe pattern as DxCluster. Ping/Pong keepalives are handled internally
// and not surfaced to the coordinator.
class LogbookSync {
public:
    enum class Role { Listen, Connect };

    std::function<void()>                            onConnected;     // a peer is live
    std::function<void()>                            onDisconnected;  // peer dropped
    std::function<void(const syncproto::Message&)>   onMessage;       // decoded frame
    std::function<void(const std::string&)>          onStatus;        // human status

    explicit LogbookSync(IUiDispatcher& ui) : ui_(ui) {}
    ~LogbookSync();
    LogbookSync(const LogbookSync&)            = delete;
    LogbookSync& operator=(const LogbookSync&) = delete;

    // For Connect, host is the peer to dial; for Listen, host is ignored.
    void start(Role role, const std::string& host, int port, int reconnectMs);
    void stop();

    bool isConnected() const { return connected_.load(); }

    // Serialize + queue a message; sent on the worker. No-op if not connected.
    void sendMessage(const syncproto::Message& m);

private:
    void worker(Role role, std::string host, int port, int reconnectMs);
    void runConnection(int fd);  // pump one live connection until it drops
    void wake();

    void postStatus(const std::string& s);

    IUiDispatcher&    ui_;
    int               wake_[2] = {-1, -1};
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread       thread_;

    std::mutex               outMutex_;
    std::vector<std::string> outbound_;  // serialized frames queued from UI thread

    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
