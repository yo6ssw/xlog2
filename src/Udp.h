#pragma once

#include "Qso.h"

#include <glibmm/main.h>
#include <sigc++/connection.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Decodes a single UDP datagram into QSO records. Recognises WSJT-X's binary
// "Logged ADIF" packet (message type 12) and falls back to treating the
// datagram as raw ADIF text. On return, `source` names the origin (used for
// status messages); it is left untouched when nothing decodes.
std::vector<Qso> decodeDatagram(const std::uint8_t* data, std::size_t len,
                                std::string& source);

// Listens on a UDP port, integrated with the GLib main loop, and reports any
// decoded QSOs through a callback. This is xlog's network-logging idea: other
// programs (WSJT-X, fldigi, JTAlert, …) send logged contacts and they are
// added to the logbook automatically.
class UdpListener {
public:
    using Callback =
        std::function<void(const std::vector<Qso>&, const std::string& source)>;

    UdpListener() = default;
    ~UdpListener();
    UdpListener(const UdpListener&)            = delete;
    UdpListener& operator=(const UdpListener&) = delete;

    void setCallback(Callback cb) { callback_ = std::move(cb); }

    // Binds and starts listening. Returns false (with `error` set) on failure.
    bool start(int port, std::string& error);
    void stop();

    bool isListening() const { return fd_ >= 0; }
    int  port() const { return port_; }

private:
    bool onReadable(Glib::IOCondition condition);

    int              fd_   = -1;
    int              port_ = 0;
    sigc::connection ioConn_;
    Callback         callback_;
};
