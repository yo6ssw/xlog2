#pragma once

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

}  // namespace timeutil
