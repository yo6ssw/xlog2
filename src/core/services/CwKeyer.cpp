// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "CwKeyer.h"

#include <netdb.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace {
constexpr char kEsc = '\x1b';
}  // namespace

CwKeyer::~CwKeyer() {
    if (fd_ >= 0)
        ::close(fd_);
}

bool CwKeyer::setEndpoint(const std::string& host, int port) {
    lastError_.clear();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    addrLen_ = 0;

    if (host.empty() || port <= 0 || port > 65535) {
        lastError_ = "invalid keyer host/port";
        return false;
    }

    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(),
                                 &hints, &res);
    if (rc != 0 || !res) {
        lastError_ = std::string("cannot resolve ") + host + ": " +
                     gai_strerror(rc);
        return false;
    }

    bool ok = false;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        fd_ = fd;
        std::memcpy(&addr_, ai->ai_addr, ai->ai_addrlen);
        addrLen_ = ai->ai_addrlen;
        ok = true;
        break;
    }
    ::freeaddrinfo(res);

    if (!ok)
        lastError_ = std::string("cannot open keyer socket: ") + std::strerror(errno);
    return ok;
}

bool CwKeyer::sendDatagram(const std::string& bytes) {
    if (fd_ < 0) {
        lastError_ = "keyer not configured";
        return false;
    }
    const ssize_t n = ::sendto(fd_, bytes.data(), bytes.size(), 0,
                               reinterpret_cast<const struct sockaddr*>(&addr_),
                               addrLen_);
    if (n < 0) {
        lastError_ = std::string("keyer send failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool CwKeyer::send(const std::string& text) {
    if (text.empty())
        return true;
    // Set the speed first (its own datagram) so each message keys at the
    // configured wpm even if cwdaemon was restarted.
    if (speed_ > 0 && !sendDatagram(std::string(1, kEsc) + "2" + std::to_string(speed_)))
        return false;
    return sendDatagram(text);
}

bool CwKeyer::abort() {
    return sendDatagram(std::string(1, kEsc) + "4");
}
