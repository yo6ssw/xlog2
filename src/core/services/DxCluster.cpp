// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "DxCluster.h"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Bands.h"

namespace {

std::string toLower(std::string s) {
  for (auto& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// A trailing time stamp like "1432Z": digits followed by 'Z'.
bool isTimeToken(const std::string& t) {
  if (t.size() < 3 || t.size() > 5) return false;
  if (std::toupper(static_cast<unsigned char>(t.back())) != 'Z') return false;
  for (size_t i = 0; i + 1 < t.size(); ++i)
    if (!std::isdigit(static_cast<unsigned char>(t[i]))) return false;
  return true;
}

bool looksLikeLoginPrompt(const std::string& lower) {
  return lower.find("login:") != std::string::npos ||
         lower.find("your call") != std::string::npos ||
         lower.find("enter your call") != std::string::npos ||
         lower.find("call:") != std::string::npos ||
         lower.find("callsign:") != std::string::npos;
}

}  // namespace

namespace dxcluster {

std::optional<DxSpot> parseSpot(const std::string& line) {
  const std::string lower = toLower(line);
  const std::string pfx = "dx de ";
  if (lower.compare(0, pfx.size(), pfx) != 0) return std::nullopt;

  const size_t colon = line.find(':', pfx.size());
  if (colon == std::string::npos) return std::nullopt;

  DxSpot s;
  s.spotter = trim(line.substr(pfx.size(), colon - pfx.size()));

  const std::vector<std::string> tok = splitWs(line.substr(colon + 1));
  if (tok.size() < 2) return std::nullopt;

  char* endp = nullptr;
  s.freqKHz = std::strtod(tok[0].c_str(), &endp);
  if (endp == tok[0].c_str() || s.freqKHz <= 0.0)
    return std::nullopt;  // first token wasn't a frequency
  s.dxCall = tok[1];

  size_t commentEnd = tok.size();
  if (tok.size() >= 3 && isTimeToken(tok.back())) {
    s.timeUtc = tok.back();
    commentEnd = tok.size() - 1;
  }
  for (size_t i = 2; i < commentEnd; ++i) {
    if (!s.comment.empty()) s.comment += ' ';
    s.comment += tok[i];
  }

  s.band = bands::forFrequencyMHz(s.freqKHz / 1000.0);
  return s;
}

}  // namespace dxcluster

namespace {

// Resolve host:port and open a blocking TCP connection. Returns the fd, or -1
// (with `err` set) on failure.
int connectTcp(const std::string& host, int port, std::string& err) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string portStr = std::to_string(port);
  const int rc = ::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
  if (rc != 0) {
    err = ::gai_strerror(rc);
    return -1;
  }
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

}  // namespace

DxCluster::~DxCluster() {
  // Drop callbacks before tearing down: at shutdown the owning panel may
  // already be destroyed, and a posted callback would call into a freed
  // widget. Nulling here plus the liveness token closes both windows.
  onSpot = nullptr;
  onLine = nullptr;
  onStatus = nullptr;
  disconnect();
}

void DxCluster::wake() {
  if (wake_[1] >= 0) {
    const char b = 1;
    [[maybe_unused]] ssize_t w = ::write(wake_[1], &b, 1);
  }
}

void DxCluster::connectTo(const std::string& host, int port,
                          const std::string& loginCall) {
  disconnect();
  if (host.empty() || port <= 0 || port > 65535) {
    postStatus("DX cluster: invalid host/port.");
    return;
  }
  // Fresh liveness token so any straggling callback from a prior session is
  // ignored.
  alive_ = std::make_shared<bool>(true);
  if (::pipe(wake_) != 0) {
    postStatus("DX cluster: " + std::string(std::strerror(errno)));
    return;
  }
  running_.store(true);
  thread_ = std::thread(&DxCluster::worker, this, host, port, loginCall);
}

void DxCluster::worker(std::string host, int port, std::string loginCall) {
  postStatus("DX cluster: connecting to " + host + ":" + std::to_string(port) +
             "…");

  std::string err;
  const int fd = connectTcp(host, port, err);
  if (fd < 0) {
    postStatus("DX cluster: connect failed — " + err);
    running_.store(false);
    return;
  }
  connected_.store(true);
  postStatus("DX cluster: connected.");

  using clock = std::chrono::steady_clock;
  const auto loginDeadline = clock::now() + std::chrono::milliseconds(2000);
  bool loggedIn = false;

  auto writeAll = [fd](const std::string& bytes) {
    size_t off = 0;
    while (off < bytes.size()) {
      const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, 0);
      if (n <= 0) break;
      off += static_cast<size_t>(n);
    }
  };

  std::string buffer;  // accumulates partial lines
  char rx[8192];

  while (running_.load()) {
    // Fallback auto-login: prompts vary between cluster software, so if none
    // was recognised, send the call a short time after connecting.
    int timeoutMs = -1;
    if (!loggedIn && !loginCall.empty()) {
      const auto now = clock::now();
      if (now >= loginDeadline) {
        writeAll(loginCall + "\r\n");
        loggedIn = true;
        postStatus("DX cluster: sent login " + loginCall + ".");
      } else {
        timeoutMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                loginDeadline - now)
                .count());
      }
    }

    pollfd fds[2] = {{fd, POLLIN, 0}, {wake_[0], POLLIN, 0}};
    const int rc = ::poll(fds, 2, timeoutMs);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (fds[1].revents & POLLIN) {
      char drain[64];
      [[maybe_unused]] ssize_t d = ::read(wake_[0], drain, sizeof(drain));
      // Flush any queued outbound commands.
      std::vector<std::string> pending;
      {
        std::lock_guard<std::mutex> lock(outMutex_);
        pending.swap(outbound_);
      }
      for (const auto& cmd : pending) writeAll(cmd + "\r\n");
      if (!running_.load()) break;
    }
    if (fds[0].revents & (POLLIN | POLLHUP)) {
      const ssize_t n = ::recv(fd, rx, sizeof(rx), 0);
      if (n <= 0) {
        postStatus("DX cluster: connection closed by server.");
        break;
      }
      buffer.append(rx, static_cast<size_t>(n));
      size_t nl;
      while ((nl = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, nl);
        buffer.erase(0, nl + 1);
        handleLine(std::move(line), loggedIn, loginCall);
      }
    }
  }

  ::close(fd);
  connected_.store(false);
}

void DxCluster::handleLine(std::string line, bool& loggedIn,
                           const std::string& loginCall) {
  if (!line.empty() && line.back() == '\r')  // telnet CRLF
    line.pop_back();

  postLine(line);

  if (!loggedIn && !loginCall.empty() && looksLikeLoginPrompt(toLower(line))) {
    // Queue the login; the worker's poll loop flushes it on the next wake.
    {
      std::lock_guard<std::mutex> lock(outMutex_);
      outbound_.push_back(loginCall);
    }
    wake();
    loggedIn = true;
    postStatus("DX cluster: sent login " + loginCall + ".");
  }

  if (auto spot = dxcluster::parseSpot(line)) postSpot(*spot);
}

void DxCluster::sendCommand(const std::string& line) {
  if (!connected_.load()) {
    postStatus("DX cluster: not connected.");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(outMutex_);
    outbound_.push_back(line);
  }
  wake();
}

void DxCluster::disconnect() {
  const bool wasRunning = running_.exchange(false);
  if (thread_.joinable()) {
    wake();
    thread_.join();
  }
  for (int* p : {&wake_[0], &wake_[1]}) {
    if (*p >= 0) {
      ::close(*p);
      *p = -1;
    }
  }
  if (wasRunning) postStatus("DX cluster: disconnected.");
}

void DxCluster::postStatus(const std::string& s) {
  ui_.post([this, w = std::weak_ptr<bool>(alive_), s]() {
    if (!w.expired() && onStatus) onStatus(s);
  });
}

void DxCluster::postLine(const std::string& s) {
  ui_.post([this, w = std::weak_ptr<bool>(alive_), s]() {
    if (!w.expired() && onLine) onLine(s);
  });
}

void DxCluster::postSpot(const DxSpot& s) {
  ui_.post([this, w = std::weak_ptr<bool>(alive_), s]() {
    if (!w.expired() && onSpot) onSpot(s);
  });
}
