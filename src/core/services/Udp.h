#pragma once

#include "IUiDispatcher.h"
#include "Qso.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Decodes a single UDP datagram into QSO records. Recognises WSJT-X's binary
// "Logged ADIF" packet (message type 12) and falls back to treating the
// datagram as raw ADIF text. On return, `source` names the origin (used for
// status messages); it is left untouched when nothing decodes.
std::vector<Qso> decodeDatagram(const std::uint8_t* data, std::size_t len,
                                std::string& source);

// Listens on a UDP port and reports any decoded QSOs through a callback. This is
// xlog's network-logging idea: other programs (WSJT-X, fldigi, JTAlert, …) send
// logged contacts and they are added to the logbook automatically.
//
// A worker thread blocks in recv(); decoded QSOs are marshalled to the UI thread
// via the injected dispatcher, so the callback always fires on the UI thread.
// A self-pipe unblocks the worker promptly on stop().
class UdpListener {
public:
    using Callback =
        std::function<void(const std::vector<Qso>&, const std::string& source)>;

    explicit UdpListener(IUiDispatcher& ui) : ui_(ui) {}
    ~UdpListener();
    UdpListener(const UdpListener&)            = delete;
    UdpListener& operator=(const UdpListener&) = delete;

    void setCallback(Callback cb) { callback_ = std::move(cb); }

    // Binds and starts listening. Returns false (with `error` set) on failure.
    bool start(int port, std::string& error);
    void stop();

    bool isListening() const { return running_.load(); }
    int  port() const { return port_; }

private:
    void worker();

    IUiDispatcher&    ui_;
    int               fd_   = -1;
    int               wake_[2] = {-1, -1};  // self-pipe: write to wake the worker
    int               port_ = 0;
    std::atomic<bool> running_{false};
    std::thread       thread_;
    Callback          callback_;

    // Liveness token for posted closures (see RigController).
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
