#pragma once

#include "DxSpot.h"

#include <giomm/cancellable.h>
#include <giomm/datainputstream.h>
#include <giomm/socketclient.h>
#include <giomm/socketconnection.h>

#include <sigc++/connection.h>

#include <functional>
#include <optional>
#include <string>

// Connects to a DX cluster (telnet) and streams DX spots.
//
// Built on giomm's async TCP: Gio::SocketClient connects, a Gio::DataInputStream
// reads lines, the output stream writes the login callsign and commands. It all
// runs on the GLib main loop (async DNS + I/O) — no worker thread — so every
// callback fires on the UI thread.
class DxCluster {
public:
    std::function<void(const DxSpot&)>      onSpot;    // a parsed DX spot
    std::function<void(const std::string&)> onLine;    // every raw line (console)
    std::function<void(const std::string&)> onStatus;  // connection state/errors

    DxCluster() = default;
    ~DxCluster();
    DxCluster(const DxCluster&)            = delete;
    DxCluster& operator=(const DxCluster&) = delete;

    void connectTo(const std::string& host, int port, const std::string& loginCall);
    void disconnect();
    bool isConnected() const { return connected_; }
    void sendCommand(const std::string& line);  // CRLF appended

private:
    void onConnected(const Glib::RefPtr<Gio::AsyncResult>& result);
    void queueRead();
    void onLineRead(const Glib::RefPtr<Gio::AsyncResult>& result);
    void handleLine(std::string line);
    void sendLogin();
    void writeRaw(const std::string& bytes);
    void teardown(const std::string& statusMsg);

    Glib::RefPtr<Gio::SocketClient>     client_;
    Glib::RefPtr<Gio::SocketConnection> conn_;
    Glib::RefPtr<Gio::DataInputStream>  in_;
    Glib::RefPtr<Gio::Cancellable>      cancellable_;
    sigc::connection loginTimer_;  // fallback auto-login if no prompt is seen
    std::string loginCall_;
    bool        connected_ = false;
    bool        loggedIn_  = false;
};

namespace dxcluster {
// Parse a single cluster line into a spot, or nullopt if it isn't a "DX de"
// line. Exposed (not anonymous) so it can be unit-tested without giomm.
std::optional<DxSpot> parseSpot(const std::string& line);
}  // namespace dxcluster
