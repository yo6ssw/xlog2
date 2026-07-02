// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "Rtkit.h"

#include <dbus/dbus.h>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>

// Minimal client for org.freedesktop.RealtimeKit1, modelled on the well-known
// reference protocol (PulseAudio/PipeWire's rtkit helper). Two D-Bus calls:
//   1. read RTTimeUSecMax / MaxRealtimePriority (properties), and
//   2. MakeThreadRealtime(thread_tid, priority).
// rtkit refuses a thread that has no finite RLIMIT_RTTIME (so a runaway RT thread
// can't lock the machine), so we set that from RTTimeUSecMax before asking.
namespace platform {
namespace {

constexpr char kService[] = "org.freedesktop.RealtimeKit1";
constexpr char kPath[]    = "/org/freedesktop/RealtimeKit1";
constexpr char kIface[]   = "org.freedesktop.RealtimeKit1";

pid_t threadId() { return static_cast<pid_t>(::syscall(SYS_gettid)); }

// Read an rtkit integer property; the value is wrapped in a variant that may hold
// an INT32 (MaxRealtimePriority, MinNiceLevel) or INT64 (RTTimeUSecMax).
bool getIntProperty(DBusConnection* conn, const char* prop, long long& out) {
    DBusMessage* msg = dbus_message_new_method_call(
        kService, kPath, "org.freedesktop.DBus.Properties", "Get");
    if (msg == nullptr)
        return false;
    const char* iface = kIface;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface,
                             DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    dbus_message_unref(msg);
    if (reply == nullptr) {
        dbus_error_free(&err);
        return false;
    }

    DBusMessageIter it, var;
    bool ok = false;
    if (dbus_message_iter_init(reply, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&it, &var);
        switch (dbus_message_iter_get_arg_type(&var)) {
            case DBUS_TYPE_INT32: {
                dbus_int32_t v = 0;
                dbus_message_iter_get_basic(&var, &v);
                out = v;
                ok = true;
                break;
            }
            case DBUS_TYPE_INT64: {
                dbus_int64_t v = 0;
                dbus_message_iter_get_basic(&var, &v);
                out = v;
                ok = true;
                break;
            }
            default:
                break;
        }
    }
    dbus_message_unref(reply);
    return ok;
}

bool callMakeRealtime(DBusConnection* conn, pid_t tid, std::uint32_t prio) {
    DBusMessage* msg =
        dbus_message_new_method_call(kService, kPath, kIface, "MakeThreadRealtime");
    if (msg == nullptr)
        return false;
    dbus_uint64_t thread = static_cast<dbus_uint64_t>(tid);
    dbus_uint32_t p = prio;
    dbus_message_append_args(msg, DBUS_TYPE_UINT64, &thread,
                             DBUS_TYPE_UINT32, &p, DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
    dbus_message_unref(msg);
    if (reply == nullptr) {
        dbus_error_free(&err);
        return false;
    }
    dbus_message_unref(reply);
    return true;
}

}  // namespace

bool makeThreadRealtime(int priority) {
    DBusError err;
    dbus_error_init(&err);
    // A private connection so we own its lifetime and don't disturb any shared bus.
    DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
    if (conn == nullptr) {
        dbus_error_free(&err);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    // rtkit requires a finite RLIMIT_RTTIME on the thread; mirror its advertised cap.
    long long rttimeMax = 0;
    if (getIntProperty(conn, "RTTimeUSecMax", rttimeMax) && rttimeMax > 0) {
        struct rlimit rl{};
        if (::getrlimit(RLIMIT_RTTIME, &rl) == 0) {
            rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(rttimeMax);
            ::setrlimit(RLIMIT_RTTIME, &rl);  // best-effort
        }
    }

    long long maxPrio = priority;
    if (getIntProperty(conn, "MaxRealtimePriority", maxPrio) && priority > maxPrio)
        priority = static_cast<int>(maxPrio);
    if (priority < 1)
        priority = 1;

    const bool ok = callMakeRealtime(conn, threadId(), static_cast<std::uint32_t>(priority));

    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    return ok;
}

}  // namespace platform
