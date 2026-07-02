// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include <sys/socket.h>

#include <string>

// Sends CW to a network keyer running ARRL/acerion's `cwdaemon` protocol.
//
// cwdaemon listens on UDP (default port 6789): a datagram whose first byte is
// not ESC (0x1B) is text to be keyed as Morse; escape requests configure the
// daemon. We use:
//   - plain text          -> key this message
//   - <ESC>2<wpm>         -> set Morse speed
//   - <ESC>4              -> abort the message currently being sent
//
// Sending is fire-and-forget UDP, so (unlike the curl-based services) there is
// no worker thread: the only potentially-blocking step, host resolution, runs
// once in setEndpoint().
class CwKeyer {
public:
    CwKeyer() = default;
    ~CwKeyer();
    CwKeyer(const CwKeyer&)            = delete;
    CwKeyer& operator=(const CwKeyer&) = delete;

    // Resolve host:port and (re)open the UDP socket. Returns false (and sets
    // lastError) on failure.
    bool setEndpoint(const std::string& host, int port);

    // Morse speed in wpm; 0 leaves cwdaemon's own default untouched.
    void setSpeed(int wpm) { speed_ = wpm; }

    bool isConfigured() const { return fd_ >= 0; }
    const std::string& lastError() const { return lastError_; }

    // Key `text` as Morse (preceded by the speed command when a speed is set).
    bool send(const std::string& text);
    // Abort the message currently being sent.
    bool abort();

private:
    bool sendDatagram(const std::string& bytes);

    int                     fd_     = -1;
    struct sockaddr_storage addr_{};
    socklen_t               addrLen_ = 0;
    int                     speed_   = 0;
    std::string             lastError_;
};
