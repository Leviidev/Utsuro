#include "log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace utsuro {
namespace {

std::mutex g_mutex;

int InitialLevel() {
    const char* env = std::getenv("UTSURO_LOG");
    if (env == nullptr) return kLogInfo;
    if (!std::strcmp(env, "debug")) return kLogDebug;
    if (!std::strcmp(env, "warn")) return kLogWarn;
    if (!std::strcmp(env, "error")) return kLogError;
    return kLogInfo;
}

std::atomic<int> g_level {InitialLevel()};

} // namespace

bool LogEnabled(LogLevel level) {
    return static_cast<int>(level) >= g_level.load(std::memory_order_relaxed);
}

void Log(LogLevel level, const char* format, ...) {
    if (!LogEnabled(level)) {
        return;
    }
    char message[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    static const char* const names[] = {"D", "I", "W", "E"};
    std::scoped_lock lock {g_mutex};
    std::fprintf(stderr, "[ut:%s] %s\n", names[level], message);
}

} // namespace utsuro
