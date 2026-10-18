// log.h -- one logging path for the whole runtime. UTSURO_LOG=debug|info|warn|error.
#pragma once

namespace utsuro {

enum LogLevel { kLogDebug = 0, kLogInfo = 1, kLogWarn = 2, kLogError = 3 };

void Log(LogLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));
bool LogEnabled(LogLevel level);

} // namespace utsuro

#define UT_DEBUG(...) ::utsuro::Log(::utsuro::kLogDebug, __VA_ARGS__)
#define UT_INFO(...) ::utsuro::Log(::utsuro::kLogInfo, __VA_ARGS__)
#define UT_WARN(...) ::utsuro::Log(::utsuro::kLogWarn, __VA_ARGS__)
#define UT_ERROR(...) ::utsuro::Log(::utsuro::kLogError, __VA_ARGS__)
