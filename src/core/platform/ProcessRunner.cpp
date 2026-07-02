// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "ProcessRunner.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

extern char** environ;

void ProcessRunner::run(const std::vector<std::string>& argv,
                        std::function<void(const Result&)> onDone) {
    if (thread_.joinable())
        thread_.join();

    thread_ = std::thread([this, argv, onDone = std::move(onDone)]() mutable {
        Result r;

        // Pipe to capture the child's stdout+stderr.
        int pipefd[2];
        if (::pipe(pipefd) != 0) {
            r.error = std::string("pipe failed: ") + std::strerror(errno);
            ui_.post([onDone, r]() { onDone(r); });
            return;
        }

        // Redirect the child's stdout (1) and stderr (2) to the pipe write end.
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], 1);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], 2);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (auto& a : argv)
            cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);

        pid_t pid = 0;
        const int rc = ::posix_spawnp(&pid, cargv[0], &actions, nullptr,
                                      cargv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(pipefd[1]);  // parent only reads

        if (rc != 0) {
            ::close(pipefd[0]);
            r.error = std::string("could not run ") + argv[0] + ": " + std::strerror(rc);
            ui_.post([onDone, r]() { onDone(r); });
            return;
        }

        std::array<char, 4096> buf;
        ssize_t n;
        while ((n = ::read(pipefd[0], buf.data(), buf.size())) > 0)
            r.output.append(buf.data(), static_cast<size_t>(n));
        ::close(pipefd[0]);

        int status = 0;
        ::waitpid(pid, &status, 0);
        r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        r.ok = (r.exitCode == 0);

        ui_.post([onDone, r]() { onDone(r); });
    });
}

ProcessRunner::~ProcessRunner() {
    if (thread_.joinable())
        thread_.join();
}
