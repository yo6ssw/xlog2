// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

#include "IUiDispatcher.h"

#include <functional>
#include <string>
#include <thread>
#include <vector>

// Runs an external command asynchronously and reports its result on the UI
// thread, replacing the toolkit-specific Gio::Subprocess used for the LoTW
// upload. A worker thread spawns the child (posix_spawn) and waits for it;
// stdout+stderr are captured and delivered via the injected dispatcher.
//
// Linux/POSIX only, matching the application's target platform.
class ProcessRunner {
public:
    struct Result {
        bool        ok = false;     // true iff the child exited 0
        int         exitCode = -1;
        std::string output;         // merged stdout + stderr
        std::string error;          // set when the child could not be spawned
    };

    explicit ProcessRunner(IUiDispatcher& ui) : ui_(ui) {}
    ~ProcessRunner();
    ProcessRunner(const ProcessRunner&)            = delete;
    ProcessRunner& operator=(const ProcessRunner&) = delete;

    // Spawn argv (argv[0] is the program, resolved via PATH). `onDone` fires on
    // the UI thread when the child exits or fails to start.
    void run(const std::vector<std::string>& argv,
             std::function<void(const Result&)> onDone);

private:
    IUiDispatcher& ui_;
    std::thread    thread_;
};
