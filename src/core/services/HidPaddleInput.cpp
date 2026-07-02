// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "HidPaddleInput.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint16_t kVendor =
    0x1EAF;  // LeafLabs Maple (the Blue Pill's USB VID)

// The bytes HID_RAW_REPORT_DESCRIPTOR emits for "Usage Page (vendor 0xFFC0)":
// item 0x06 (Usage Page, 2-byte) followed by 0xC0, 0xFF (little-endian 0xFFC0).
// Matching this in the report descriptor distinguishes our paddle from any
// other 0x1EAF device that might be plugged in.
const uint8_t kUsagePageSig[3] = {0x06, 0xC0, 0xFF};

bool descriptorHasVendorUsagePage(int fd) {
  int descSize = 0;
  if (::ioctl(fd, HIDIOCGRDESCSIZE, &descSize) < 0 || descSize <= 0)
    return false;
  struct hidraw_report_descriptor desc;
  std::memset(&desc, 0, sizeof(desc));
  desc.size = static_cast<__u32>(descSize);
  if (::ioctl(fd, HIDIOCGRDESC, &desc) < 0) return false;
  for (int i = 0; i + 3 <= descSize; ++i) {
    if (desc.value[i] == kUsagePageSig[0] &&
        desc.value[i + 1] == kUsagePageSig[1] &&
        desc.value[i + 2] == kUsagePageSig[2])
      return true;
  }
  return false;
}

}  // namespace

HidPaddleInput::~HidPaddleInput() { stop(); }

void HidPaddleInput::start() {
  if (running_.exchange(true)) return;
  alive_ = std::make_shared<bool>(true);
  if (::pipe(wake_) != 0) {
    wake_[0] = wake_[1] = -1;
  }
  thread_ = std::thread(&HidPaddleInput::worker, this);
}

void HidPaddleInput::stop() {
  running_.store(false);
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
  // Drop any status closure still queued for the UI thread.
  alive_ = std::make_shared<bool>(true);
}

void HidPaddleInput::wake() {
  if (wake_[1] >= 0) {
    const char b = 1;
    [[maybe_unused]] ssize_t n = ::write(wake_[1], &b, 1);
  }
}

void HidPaddleInput::postStatus(const std::string& s) {
  ui_.post([this, w = std::weak_ptr<bool>(alive_), s]() {
    if (!w.expired() && onStatus) onStatus(s);
  });
}

int HidPaddleInput::openPaddle(std::string& nameOut, bool& permissionDenied) {
  permissionDenied = false;
  DIR* dir = ::opendir("/dev");
  if (!dir) return -1;

  int found = -1;
  struct dirent* e;
  while ((e = ::readdir(dir)) != nullptr) {
    if (std::strncmp(e->d_name, "hidraw", 6) != 0) continue;
    const std::string path = std::string("/dev/") + e->d_name;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      if (errno == EACCES) permissionDenied = true;
      continue;
    }
    struct hidraw_devinfo info;
    std::memset(&info, 0, sizeof(info));
    if (::ioctl(fd, HIDIOCGRAWINFO, &info) == 0 &&
        static_cast<uint16_t>(info.vendor) == kVendor &&
        descriptorHasVendorUsagePage(fd)) {
      char name[256] = {0};
      if (::ioctl(fd, HIDIOCGRAWNAME(sizeof(name)), name) < 0) name[0] = '\0';
      nameOut = path;
      if (name[0]) nameOut += std::string(" (") + name + ")";
      found = fd;
      break;
    }
    ::close(fd);
  }
  ::closedir(dir);
  return found;
}

void HidPaddleInput::worker() {
  postStatus("HID paddle: searching…");

  int fd = -1;
  bool lastDit = false, lastDah = false;
  bool reportedPerm = false;

  while (running_.load()) {
    if (fd < 0) {
      std::string name;
      bool permDenied = false;
      fd = openPaddle(name, permDenied);
      if (fd >= 0) {
        postStatus("HID paddle: connected — " + name);
        lastDit = lastDah = false;
        reportedPerm = false;
      } else {
        if (permDenied && !reportedPerm) {
          postStatus(
              "HID paddle: found but permission denied — install the "
              "udev rule and replug.");
          reportedPerm = true;
        }
        // Retry discovery in ~1 s, or sooner if asked to stop.
        pollfd w{wake_[0], POLLIN, 0};
        ::poll(&w, 1, 1000);
        if (w.revents & POLLIN) {
          char d[64];
          [[maybe_unused]] ssize_t n = ::read(wake_[0], d, sizeof(d));
        }
        continue;
      }
    }

    pollfd fds[2] = {{fd, POLLIN, 0}, {wake_[0], POLLIN, 0}};
    const int rc = ::poll(fds, 2, -1);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (fds[1].revents & POLLIN) {
      char d[64];
      [[maybe_unused]] ssize_t n = ::read(wake_[0], d, sizeof(d));
      if (!running_.load()) break;
    }
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      uint8_t buf[16];
      const ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        // Device went away: release any held contact, then re-discover.
        ::close(fd);
        fd = -1;
        if (lastDit && onDit) onDit(false);
        if (lastDah && onDah) onDah(false);
        lastDit = lastDah = false;
        postStatus("HID paddle: disconnected, searching…");
        continue;
      }
      const bool dit = (buf[0] & 0x01) != 0;
      const bool dah = (buf[0] & 0x02) != 0;
      if (dit != lastDit) {
        lastDit = dit;
        if (onDit) onDit(dit);
      }
      if (dah != lastDah) {
        lastDah = dah;
        if (onDah) onDah(dah);
      }
    }
  }

  if (fd >= 0) ::close(fd);
  // Don't leave a contact stuck closed if we stop mid-press.
  if (lastDit && onDit) onDit(false);
  if (lastDah && onDah) onDah(false);
}
