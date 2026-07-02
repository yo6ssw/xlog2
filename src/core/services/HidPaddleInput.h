// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "IUiDispatcher.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

// Reads a vendor-defined HID Morse paddle (the companion ../paddles firmware)
// and reports dit/dash contact changes. The device is a RAW HID device — a
// custom report on a vendor usage page that no OS input subsystem interprets,
// so it never disturbs text input (unlike a keyboard-class paddle). It is
// identified by USB vendor 0x1EAF and the vendor HID usage page 0xFFC0 in its
// report descriptor, and sends a 2-byte report {state, seq} on every edge,
// where state bit0 = dit and bit1 = dash.
//
// A worker thread auto-discovers the /dev/hidraw* node, opens it, and blocks in
// poll() until a report arrives, so a paddle edge surfaces within ~1 ms. If the
// device is absent or unplugged it keeps retrying discovery in the background.
//
// LATENCY NOTE: onDit/onDah fire on the WORKER thread, not the UI thread. They
// are meant to drive RemotePaddleKeyer::setDit/setDah, which are lock-free
// atomics safe to set from any thread — calling them directly skips the
// UI-thread hop and keeps the keying path minimal. onStatus is marshalled to
// the UI thread via the dispatcher.
class HidPaddleInput {
public:
    std::function<void(bool pressed)>       onDit;     // WORKER thread
    std::function<void(bool pressed)>       onDah;     // WORKER thread
    std::function<void(const std::string&)> onStatus;  // UI thread

    explicit HidPaddleInput(IUiDispatcher& ui) : ui_(ui) {}
    ~HidPaddleInput();
    HidPaddleInput(const HidPaddleInput&)            = delete;
    HidPaddleInput& operator=(const HidPaddleInput&) = delete;

    void start();
    void stop();
    bool isActive() const { return running_.load(); }

private:
    void worker();
    int  openPaddle(std::string& nameOut, bool& permissionDenied);  // fd, or -1
    void wake();
    void postStatus(const std::string& s);

    IUiDispatcher&        ui_;
    int                   wake_[2] = {-1, -1};
    std::atomic<bool>     running_{false};
    std::thread           thread_;

    // Liveness token for posted closures (mirrors DxCluster/RemotePaddleKeyer):
    // recreated on each start so a late status callback is dropped.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
