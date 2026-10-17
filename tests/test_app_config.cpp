// Track D: config INI round-trip, defaults, malformed values; autostart/vendor queries are read-only.
#include <windows.h>

#include <cstdio>
#include <string>

#include "../src/app/core/autostart.h"
#include "../src/app/core/util.h"
#include "../src/app/core/vendor.h"
#include "msdisp/config.h"
#include "msdisp/log.h"

using namespace msdisp;

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

static bool sameConfig(const Config& a, const Config& b) {
    return a.rotation == b.rotation && a.mode.width == b.mode.width && a.mode.height == b.mode.height &&
           a.mode.hz == b.mode.hz && a.mode.vic == b.mode.vic && a.virtualWidth == b.virtualWidth &&
           a.virtualHeight == b.virtualHeight && a.fpsCap == b.fpsCap && a.tileW == b.tileW && a.tileH == b.tileH &&
           a.stopWinUsbDisplay == b.stopWinUsbDisplay && a.showOverlay == b.showOverlay && a.logFile == b.logFile;
}

static void writeFileW(const std::wstring& path, const std::wstring& text) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    std::string u = toUtf8(text);  // the profile API reads ANSI/UTF-8-less files fine for ASCII content
    fwrite(u.data(), 1, u.size(), f);
    fclose(f);
}

static void deleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                deleteTree(p);
            } else {
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring root = std::wstring(tmp) + L"msdisp_test_" + std::to_wstring(GetCurrentProcessId());
    deleteTree(root);

    // 1. Missing file => defaults, success.
    {
        Config c;
        c.fpsCap = 1;
        CHECK(loadConfig(root + L"\\missing.ini", c));
        CHECK(sameConfig(c, Config{}));
    }

    // 2. Round trip, into a directory that does not exist yet, with a non-ASCII log path.
    std::wstring ini = root + L"\\sub dir\\ação\\msdisp.ini";
    {
        Config c;
        c.rotation = Rotation::CW270;
        c.mode = Mode{1280, 720, 60, 0x4F};
        c.virtualWidth = 1200;
        c.virtualHeight = 1920;
        c.fpsCap = 30;
        c.tileW = 32;
        c.tileH = 16;
        c.stopWinUsbDisplay = false;
        c.showOverlay = true;
        c.logFile = L"C:\\Usuários\\João\\msdisp ç.log";
        CHECK(saveConfig(ini, c));
        CHECK(fileExists(ini));
        Config back;
        CHECK(loadConfig(ini, back));
        CHECK(sameConfig(c, back));

        // Saving again over an existing file keeps other sections (e.g. the vendor backup).
        WritePrivateProfileStringW(L"vendor_backup", L"scope", L"user", ini.c_str());
        c.rotation = Rotation::None;
        CHECK(saveConfig(ini, c));
        wchar_t buf[32];
        GetPrivateProfileStringW(L"vendor_backup", L"scope", L"", buf, 32, ini.c_str());
        CHECK(std::wstring(buf) == L"user");
        CHECK(loadConfig(ini, back));
        CHECK(back.rotation == Rotation::None);
    }

    // 3. Malformed values: keep defaults for those keys, keep valid ones, report false.
    std::wstring bad = root + L"\\bad.ini";
    {
        writeFileW(bad,
                   L"[display]\r\n"
                   L"rotation=45\r\n"
                   L"width=1280\r\n"
                   L"height=720\r\n"
                   L"fpsCap=abc\r\n"
                   L"tileW=7\r\n"
                   L"tileH=99999\r\n"
                   L"virtualWidth= 1000 \r\n"
                   L"[app]\r\n"
                   L"showOverlay=maybe\r\n"
                   L"stopWinUsbDisplay=no\r\n");
        Config c;
        CHECK(!loadConfig(bad, c));
        Config d;
        CHECK(c.rotation == d.rotation);
        CHECK(c.fpsCap == d.fpsCap);
        CHECK(c.tileW == d.tileW);
        CHECK(c.tileH == d.tileH);
        CHECK(c.showOverlay == d.showOverlay);
        CHECK(c.virtualWidth == 1000);
        CHECK(!c.stopWinUsbDisplay);
        // vic missing for a known mode => inferred from the mode table.
        CHECK(c.mode.width == 1280 && c.mode.height == 720 && c.mode.vic == 0x4F);
    }

    // 4. Unknown mode without vic => whole mode falls back to default; explicit hex vic is honoured.
    {
        writeFileW(bad, L"[display]\r\nwidth=1024\r\nheight=768\r\n");
        Config c;
        loadConfig(bad, c);
        CHECK(c.mode.width == 1920 && c.mode.height == 1080 && c.mode.vic == 0x81);
        writeFileW(bad, L"[display]\r\nwidth=1024\r\nheight=768\r\nhz=60\r\nvic=0x4A\r\n");
        CHECK(loadConfig(bad, c));
        CHECK(c.mode.width == 1024 && c.mode.height == 768 && c.mode.vic == 0x4A);
        writeFileW(bad, L"[display]\r\nvic=300\r\n");
        CHECK(!loadConfig(bad, c));
        CHECK(c.mode.vic == 0x81);
    }

    // 5. Default paths resolve under %APPDATA%\msdisp.
    {
        std::wstring p = defaultConfigPath();
        CHECK(p.size() > 11 && p.substr(p.size() - 11) == L"\\msdisp.ini");
        Config c;
        CHECK(resolvedLogPath(c).find(L"msdisp.log") != std::wstring::npos);
        c.logFile = L"X:\\a.log";
        CHECK(resolvedLogPath(c) == L"X:\\a.log");
    }

    // 6. Read-only registry/process queries must not crash (no state is changed here).
    {
        bool a = autostartEnabled();
        bool e = autostartPointsElsewhere();
        CHECK(autostartCommand().find(L".exe") != std::wstring::npos);
        bool v = vendorAutostartEnabled();
        VendorAutostart q = queryVendorAutostart();
        bool r = isVendorRunning();
        std::wstring exe = vendorExePath(ini);
        CHECK(exe.find(kVendorExeName) != std::wstring::npos);
        CHECK(!vendorAutostartRemovedByUs(bad));  // no backup in that INI
        MSD_INFO("autostart=%d elsewhere=%d vendorAutostart=%d scope=%d vendorRunning=%d exe=%s", a, e, v,
                 int(q.scope), r, toUtf8(exe).c_str());
        // The elevated helper rejects malformed requests without touching the registry.
        wchar_t op[] = L"frobnicate", sc[] = L"user";
        wchar_t* args[] = {op, sc};
        CHECK(runElevatedHelper(2, args) != 0);
        CHECK(runElevatedHelper(0, nullptr) != 0);
    }

    deleteTree(root);
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_app_config: OK\n");
    return 0;
}
