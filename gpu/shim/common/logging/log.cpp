#include "../../bbport_diagnostics.h"
// bbport: see log.h.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>
#include "common/logging/log.h"

namespace Common::Log {
static Level threshold() {
    static const Level level = [] {
        const char* v = BB_DIAGNOSTIC_ENV("BB_GPU_LOG");
        if (!v) return Level::warn;
        static const char* names[] = {"trace", "debug", "info", "warning", "error", "critical"};
        for (int i = 0; i < 6; ++i)
            if (!std::strcmp(v, names[i])) return Level(i);
        return Level::warn;
    }();
    return level;
}
// BB_GPU_LOG_ONLY=Lib.Np,Lib.Net: debug and trace lines of just these classes (diagnostics builds).
static bool classEnabled(std::string_view log_class) {
    static const std::string list = [] {
        const char* v = BB_DIAGNOSTIC_ENV("BB_GPU_LOG_ONLY");
        return std::string(v ? v : "");
    }();
    size_t pos = 0;
    while (pos < list.size()) {
        size_t end = list.find(',', pos);
        if (end == std::string::npos) end = list.size();
        const std::string_view prefix(list.data() + pos, end - pos);
        if (!prefix.empty() && log_class.substr(0, prefix.size()) == prefix) return true;
        pos = end + 1;
    }
    return false;
}
bool ShouldLog(std::string_view log_class, Level level) {
    return level >= threshold() || (level <= Level::debug && classEnabled(log_class));
}

void Write(std::string_view log_class, Level level, const char* file, int line, const char* function,
           std::string&& message) {
    static std::mutex mutex;
    static const char* names[] = {"Trace", "Debug", "Info", "Warning", "Error", "Critical"};
    const char* base = std::strrchr(file, '/');
    std::scoped_lock lock{mutex};
    char clock[16] = "";
    if constexpr (BB_DIAGNOSTICS) { // wall clock, to line the log up with another process
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_local{};
        localtime_s(&tm_local, &t);
        const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
        std::snprintf(clock, sizeof(clock), "%02d:%02d:%02d.%03d ", tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec, ms);
    }
    std::fprintf(stderr, "%sGPU [%.*s] <%s> %s:%d %s: %s\n", clock, int(log_class.size()), log_class.data(),
                 names[int(level)], base ? base + 1 : file, line, function, message.c_str());
}
} // namespace Common::Log
