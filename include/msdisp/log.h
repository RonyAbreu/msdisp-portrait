// msdisp-portrait — logging. FROZEN CONTRACT.
#pragma once
#include <string>

namespace msdisp {

enum class LogLevel { Debug, Info, Warn, Error };

// Thread-safe. Writes to stderr and, if set, to the log file.
void logSetFile(const std::wstring& path);
void logSetLevel(LogLevel level);
void logf(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define MSD_DEBUG(...) ::msdisp::logf(::msdisp::LogLevel::Debug, __VA_ARGS__)
#define MSD_INFO(...) ::msdisp::logf(::msdisp::LogLevel::Info, __VA_ARGS__)
#define MSD_WARN(...) ::msdisp::logf(::msdisp::LogLevel::Warn, __VA_ARGS__)
#define MSD_ERROR(...) ::msdisp::logf(::msdisp::LogLevel::Error, __VA_ARGS__)

}  // namespace msdisp
