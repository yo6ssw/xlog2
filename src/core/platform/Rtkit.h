// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#pragma once

namespace platform {

// Request real-time (SCHED_FIFO) scheduling for the *calling thread* from the
// org.freedesktop.RealtimeKit1 system D-Bus service — the same privileged
// broker PipeWire / PulseAudio use. This works on an ordinary desktop with no
// rtprio limit in limits.conf, because rtkit (running with privilege) grants RT
// per-thread on request and bounds it with RLIMIT_RTTIME.
//
// `priority` is the SCHED_FIFO priority to ask for; it is clamped to rtkit's
// advertised MaxRealtimePriority. Returns true if RT was granted. Safe to call
// when no system bus / no rtkit daemon is present (returns false). POSIX/Linux
// only, like the rest of src/core/platform.
bool makeThreadRealtime(int priority);

}  // namespace platform
