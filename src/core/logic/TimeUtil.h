#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

// Toolkit-neutral time formatting.
namespace timeutil {

// Format the current UTC time with a strftime pattern, e.g. "%Y-%m-%d" or
// "%H:%M". Used to stamp QSO date/time-on fields.
inline std::string utcNow(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

// Milliseconds since the Unix epoch (UTC). Used as the comparison base for
// logbook-sync last-write-wins.
inline long long utcNowMillis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
}

// Current UTC time as fixed-width ISO-8601 with milliseconds, e.g.
// "2026-06-26T10:11:12.345Z". The fixed width makes lexicographic string
// comparison equivalent to chronological order, which the sync merge relies on
// for `updated_at` / `deleted_at`.
inline std::string utcNowMillisIso() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const long long ms =
        duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    gmtime_r(&t, &tm);
    char date[24];
    std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[40];
    std::snprintf(out, sizeof(out), "%s.%03lldZ", date, ms);
    return out;
}

}  // namespace timeutil
