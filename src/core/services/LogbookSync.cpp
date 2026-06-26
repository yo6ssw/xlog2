#include "LogbookSync.h"

#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

namespace {

constexpr int kKeepaliveMs = 15000;  // idle ping interval / poll cadence

// Resolve host:port and open a blocking TCP connection. Returns the fd, or -1.
int connectTcp(const std::string& host, int port, std::string& err) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    const int rc = ::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0) { err = ::gai_strerror(rc); return -1; }
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) err = std::strerror(errno);
    return fd;
}

// Open a listening socket on the given port (all interfaces). Returns fd or -1.
int listenTcp(int port, std::string& err) {
    addrinfo hints{};
    hints.ai_family   = AF_INET;  // IPv4; simple and covers LAN + tunnels
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    const int rc = ::getaddrinfo(nullptr, portStr.c_str(), &hints, &res);
    if (rc != 0) { err = ::gai_strerror(rc); return -1; }
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd, 1) == 0)
            break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) err = std::strerror(errno);
    return fd;
}

void writeAll(int fd, const std::string& bytes) {
    size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, 0);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}

}  // namespace

LogbookSync::~LogbookSync() {
    onConnected    = nullptr;
    onDisconnected = nullptr;
    onMessage      = nullptr;
    onStatus       = nullptr;
    stop();
}

void LogbookSync::wake() {
    if (wake_[1] >= 0) {
        const char b = 1;
        [[maybe_unused]] ssize_t w = ::write(wake_[1], &b, 1);
    }
}

void LogbookSync::start(Role role, const std::string& host, int port,
                        int reconnectMs) {
    stop();
    if (port <= 0 || port > 65535) {
        postStatus("Sync: invalid port.");
        return;
    }
    if (role == Role::Connect && host.empty()) {
        postStatus("Sync: no peer host configured.");
        return;
    }
    alive_ = std::make_shared<bool>(true);
    if (::pipe(wake_) != 0) {
        postStatus("Sync: " + std::string(std::strerror(errno)));
        return;
    }
    running_.store(true);
    thread_ = std::thread(&LogbookSync::worker, this, role, host, port,
                          reconnectMs > 0 ? reconnectMs : 5000);
}

void LogbookSync::worker(Role role, std::string host, int port, int reconnectMs) {
    if (role == Role::Listen) {
        std::string err;
        const int lfd = listenTcp(port, err);
        if (lfd < 0) {
            postStatus("Sync: listen failed — " + err);
            running_.store(false);
            return;
        }
        postStatus("Sync: listening on port " + std::to_string(port) + "…");
        while (running_.load()) {
            pollfd fds[2] = {{lfd, POLLIN, 0}, {wake_[0], POLLIN, 0}};
            const int rc = ::poll(fds, 2, -1);
            if (rc < 0) { if (errno == EINTR) continue; break; }
            if (fds[1].revents & POLLIN) {
                char drain[64];
                [[maybe_unused]] ssize_t d = ::read(wake_[0], drain, sizeof(drain));
                if (!running_.load()) break;
            }
            if (fds[0].revents & POLLIN) {
                const int cfd = ::accept(lfd, nullptr, nullptr);
                if (cfd < 0) continue;
                postStatus("Sync: peer connected.");
                runConnection(cfd);
                ::close(cfd);
                if (running_.load())
                    postStatus("Sync: peer disconnected; waiting…");
            }
        }
        ::close(lfd);
    } else {
        while (running_.load()) {
            std::string err;
            postStatus("Sync: connecting to " + host + ":" + std::to_string(port) + "…");
            const int fd = connectTcp(host, port, err);
            if (fd >= 0) {
                postStatus("Sync: connected to peer.");
                runConnection(fd);
                ::close(fd);
            } else {
                postStatus("Sync: connect failed — " + err);
            }
            if (!running_.load()) break;
            // Interruptible backoff before retrying.
            pollfd pf{wake_[0], POLLIN, 0};
            if (::poll(&pf, 1, reconnectMs) > 0) {
                char drain[64];
                [[maybe_unused]] ssize_t d = ::read(wake_[0], drain, sizeof(drain));
            }
        }
    }
}

void LogbookSync::runConnection(int fd) {
    // A fresh connection starts with an empty outbound queue (drop anything
    // stale that was queued while disconnected).
    {
        std::lock_guard<std::mutex> lock(outMutex_);
        outbound_.clear();
    }
    connected_.store(true);
    ui_.post([this, w = std::weak_ptr<bool>(alive_)]() {
        if (!w.expired() && onConnected) onConnected();
    });

    syncproto::Decoder decoder;
    char rx[16384];
    using clock = std::chrono::steady_clock;
    auto lastTx = clock::now();

    while (running_.load()) {
        pollfd fds[2] = {{fd, POLLIN, 0}, {wake_[0], POLLIN, 0}};
        const int rc = ::poll(fds, 2, kKeepaliveMs);
        if (rc < 0) { if (errno == EINTR) continue; break; }

        if (rc == 0) {  // idle: send a keepalive ping
            writeAll(fd, syncproto::encodeFrame({syncproto::Type::Ping, ""}));
            lastTx = clock::now();
            continue;
        }

        if (fds[1].revents & POLLIN) {
            char drain[64];
            [[maybe_unused]] ssize_t d = ::read(wake_[0], drain, sizeof(drain));
            std::vector<std::string> pending;
            {
                std::lock_guard<std::mutex> lock(outMutex_);
                pending.swap(outbound_);
            }
            for (const auto& frame : pending)
                writeAll(fd, frame);
            if (!pending.empty()) lastTx = clock::now();
            if (!running_.load()) break;
        }

        if (fds[0].revents & (POLLIN | POLLHUP)) {
            const ssize_t n = ::recv(fd, rx, sizeof(rx), 0);
            if (n <= 0) break;  // peer closed
            decoder.feed(rx, static_cast<size_t>(n));
            syncproto::Message m;
            while (decoder.next(m)) {
                if (m.type == syncproto::Type::Ping) {
                    writeAll(fd, syncproto::encodeFrame({syncproto::Type::Pong, ""}));
                    lastTx = clock::now();
                    continue;
                }
                if (m.type == syncproto::Type::Pong)
                    continue;  // keepalive ack; nothing to do
                ui_.post([this, w = std::weak_ptr<bool>(alive_), m]() {
                    if (!w.expired() && onMessage) onMessage(m);
                });
            }
            if (decoder.failed()) {
                postStatus("Sync: protocol error; dropping connection.");
                break;
            }
        }
    }

    connected_.store(false);
    ui_.post([this, w = std::weak_ptr<bool>(alive_)]() {
        if (!w.expired() && onDisconnected) onDisconnected();
    });
}

void LogbookSync::sendMessage(const syncproto::Message& m) {
    if (!connected_.load())
        return;
    {
        std::lock_guard<std::mutex> lock(outMutex_);
        outbound_.push_back(syncproto::encodeFrame(m));
    }
    wake();
}

void LogbookSync::stop() {
    running_.store(false);
    if (thread_.joinable()) {
        wake();
        thread_.join();
    }
    for (int* p : {&wake_[0], &wake_[1]}) {
        if (*p >= 0) { ::close(*p); *p = -1; }
    }
    connected_.store(false);
}

void LogbookSync::postStatus(const std::string& s) {
    ui_.post([this, w = std::weak_ptr<bool>(alive_), s]() {
        if (!w.expired() && onStatus) onStatus(s);
    });
}
