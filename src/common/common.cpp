#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "msdisp/common.h"
#include "msdisp/log.h"

namespace msdisp {

int64_t nowUs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(c.QuadPart * 1000000 / freq.QuadPart);
}

namespace {
std::mutex g_logMutex;
FILE* g_logFile = nullptr;
LogLevel g_level = LogLevel::Info;
const char* levelName(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "D";
        case LogLevel::Info: return "I";
        case LogLevel::Warn: return "W";
        default: return "E";
    }
}
}  // namespace

void logSetFile(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) fclose(g_logFile);
    g_logFile = path.empty() ? nullptr : _wfopen(path.c_str(), L"a");
}

void logSetLevel(LogLevel level) { g_level = level; }

void logf(LogLevel level, const char* fmt, ...) {
    if (level < g_level) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::lock_guard<std::mutex> lock(g_logMutex);
    fprintf(stderr, "%02d:%02d:%02d.%03d %s %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            levelName(level), msg);
    if (g_logFile) {
        fprintf(g_logFile, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s %s\n", st.wYear, st.wMonth, st.wDay, st.wHour,
                st.wMinute, st.wSecond, st.wMilliseconds, levelName(level), msg);
        fflush(g_logFile);
    }
}

}  // namespace msdisp
