// msdisp.exe — background tray app: virtual portrait monitor -> capture -> rotate/convert -> MS912x USB.
// Command line (all optional, not shown in the UI):
//   --console          attach to the parent console (or open one) and print the log there
//   --debug            log level Debug
//   --config <path>    use another INI instead of %APPDATA%\msdisp\msdisp.ini
//   --elevated-vendor-run ...   internal: elevated helper for the vendor's HKLM Run value (see core/vendor.h)
#include <windows.h>
#include <shellapi.h>

#include <cstdio>

#include "core/pipeline.h"
#include "core/util.h"
#include "core/vendor.h"
#include "msdisp/log.h"
#include "tray.h"

namespace {

constexpr wchar_t kMutexName[] = L"Local\\msdisp-portrait.single-instance";
constexpr LONGLONG kMaxLogBytes = 4LL << 20;

void attachConsole() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole()) return;
    SetConsoleOutputCP(CP_UTF8);
    FILE* f = nullptr;
    f = _wfreopen(L"CONOUT$", L"w", stderr);
    f = _wfreopen(L"CONOUT$", L"w", stdout);
    (void)f;
    setvbuf(stderr, nullptr, _IONBF, 0);
}

// Keeps one previous log (<name>.1) once the current one grows past kMaxLogBytes.
void rotateLog(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return;
    LONGLONG size = (LONGLONG(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    if (size > kMaxLogBytes) MoveFileExW(path.c_str(), (path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
}

void openLog(const std::wstring& path) {
    msdisp::ensureDirectory(msdisp::parentDirectory(path));
    rotateLog(path);
    msdisp::logSetFile(path);
}

std::wstring fullPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), DWORD(std::size(buf)), buf, nullptr);
    return n && n < std::size(buf) ? std::wstring(buf, n) : p;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool console = false, debug = false;
    std::wstring configPath;
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == msdisp::kElevatedFlag) {
            // Elevated helper: no UI, no single-instance check, no log file. Exit code = Win32 error.
            int rc = msdisp::runElevatedHelper(argc - i - 1, argv + i + 1);
            LocalFree(argv);
            return rc;
        }
        if (a == L"--console") {
            console = true;
        } else if (a == L"--debug") {
            debug = true;
        } else if (a == L"--config" && i + 1 < argc) {
            configPath = fullPath(argv[++i]);
        }
    }
    if (argv) LocalFree(argv);

    // Same as the manifest in res/msdisp.manifest; harmless (ERROR_ACCESS_DENIED) when the manifest already set it.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (console) attachConsole();
    if (debug) msdisp::logSetLevel(msdisp::LogLevel::Debug);

    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(msdisp::kTrayWindowClass, nullptr)) {
            PostMessageW(other, msdisp::kMsgSecondInstance, 0, 0);
        }
        fprintf(stderr, "msdisp is already running\n");
        CloseHandle(mutex);
        return 0;
    }

    // Default log first so warnings while parsing the INI are kept, then the configured one.
    std::wstring logPath = msdisp::defaultLogPath();
    openLog(logPath);

    msdisp::AppOptions opts;
    opts.configPath = configPath.empty() ? msdisp::defaultConfigPath() : configPath;
    bool existed = msdisp::fileExists(opts.configPath);
    if (!msdisp::loadConfig(opts.configPath, opts.config)) {
        MSD_WARN("config: %s has invalid values (defaults used for those keys)",
                 msdisp::toUtf8(opts.configPath).c_str());
    }
    if (!existed) msdisp::saveConfig(opts.configPath, opts.config);  // so "Abrir configuração" has a file
    if (msdisp::resolvedLogPath(opts.config) != logPath) openLog(msdisp::resolvedLogPath(opts.config));

    MSD_INFO("msdisp starting (config %s)", msdisp::toUtf8(opts.configPath).c_str());

    int rc = 1;
    {
        msdisp::TrayApp app(inst, std::move(opts), msdisp::createPipeline());
        if (app.init()) rc = app.run();
    }
    MSD_INFO("msdisp exiting (%d)", rc);
    msdisp::logSetFile(L"");
    if (mutex) CloseHandle(mutex);
    return rc;
}
