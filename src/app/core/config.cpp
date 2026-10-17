// Config <-> INI (%APPDATA%\msdisp\msdisp.ini) via the Win32 profile API.
// The file is created as UTF-16LE with BOM so WritePrivateProfileStringW keeps non-ASCII paths intact.
#include "msdisp/config.h"

#include <windows.h>

#include <cerrno>
#include <cwchar>
#include <cwctype>

#include "modes.h"
#include "msdisp/log.h"
#include "util.h"

namespace msdisp {

namespace {

constexpr wchar_t kSecDisplay[] = L"display";
constexpr wchar_t kSecApp[] = L"app";
constexpr wchar_t kMissing[] = L"\x01missing\x01";

std::wstring trim(std::wstring s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

class IniReader {
public:
    explicit IniReader(const std::wstring& path) : path_(path) {}

    // False when the key is absent (default is kept silently).
    bool raw(const wchar_t* sec, const wchar_t* key, std::wstring& out) {
        wchar_t buf[2048];
        GetPrivateProfileStringW(sec, key, kMissing, buf, DWORD(std::size(buf)), path_.c_str());
        if (wcscmp(buf, kMissing) == 0) return false;
        out = trim(buf);
        return true;
    }

    void integer(const wchar_t* sec, const wchar_t* key, int& field, long lo, long hi) {
        std::wstring v;
        if (!raw(sec, key, v)) return;
        wchar_t* end = nullptr;
        errno = 0;
        long n = wcstol(v.c_str(), &end, 0);  // base 0: accepts 0x81
        if (v.empty() || *end != L'\0' || errno == ERANGE || n < lo || n > hi) {
            bad(sec, key, v);
            return;
        }
        field = int(n);
    }

    void boolean(const wchar_t* sec, const wchar_t* key, bool& field) {
        std::wstring v;
        if (!raw(sec, key, v)) return;
        for (auto& c : v) c = wchar_t(towlower(c));
        if (v == L"1" || v == L"true" || v == L"yes" || v == L"on" || v == L"sim") {
            field = true;
        } else if (v == L"0" || v == L"false" || v == L"no" || v == L"off" || v == L"nao" || v == L"não") {
            field = false;
        } else {
            bad(sec, key, v);
        }
    }

    void bad(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
        MSD_WARN("config: [%s] %s = \"%s\" is invalid, keeping default", toUtf8(sec).c_str(), toUtf8(key).c_str(),
                 toUtf8(v).c_str());
        ok_ = false;
    }

    bool ok() const { return ok_; }

private:
    std::wstring path_;
    bool ok_ = true;
};

}  // namespace

bool loadConfig(const std::wstring& path, Config& out) {
    out = Config{};
    if (!fileExists(path)) {
        MSD_INFO("config: %s not found, using defaults", toUtf8(path).c_str());
        return true;
    }
    IniReader r(path);

    int rot = int(out.rotation);
    r.integer(kSecDisplay, L"rotation", rot, 0, 270);
    if (rot == 0 || rot == 90 || rot == 180 || rot == 270) {
        out.rotation = Rotation(rot);
    } else {
        r.bad(kSecDisplay, L"rotation", std::to_wstring(rot));
    }

    Mode m = out.mode;
    r.integer(kSecDisplay, L"width", m.width, 64, 4095);
    r.integer(kSecDisplay, L"height", m.height, 64, 4095);
    r.integer(kSecDisplay, L"hz", m.hz, 1, 240);
    int vic = -1;
    r.integer(kSecDisplay, L"vic", vic, 0, 255);
    if (vic < 0) {
        // vic absent or invalid: derive it from the resolution when it is a known mode.
        int known = vicForMode(m.width, m.height, m.hz);
        if (known >= 0) {
            vic = known;
        } else if (m.width != out.mode.width || m.height != out.mode.height || m.hz != out.mode.hz) {
            MSD_WARN("config: mode %dx%d@%d has no known vic and none is set, keeping default mode", m.width, m.height,
                     m.hz);
            m = out.mode;
            vic = out.mode.vic;
        } else {
            vic = out.mode.vic;
        }
    }
    m.vic = uint8_t(vic);
    out.mode = m;

    r.integer(kSecDisplay, L"virtualWidth", out.virtualWidth, 64, 8192);
    r.integer(kSecDisplay, L"virtualHeight", out.virtualHeight, 64, 8192);
    r.integer(kSecDisplay, L"fpsCap", out.fpsCap, 1, 240);
    r.integer(kSecDisplay, L"tileW", out.tileW, 2, 1024);
    r.integer(kSecDisplay, L"tileH", out.tileH, 1, 1024);
    if (out.tileW % 2) {
        r.bad(kSecDisplay, L"tileW", std::to_wstring(out.tileW) + L" (must be even)");
        out.tileW = Config{}.tileW;
    }

    r.boolean(kSecApp, L"stopWinUsbDisplay", out.stopWinUsbDisplay);
    r.boolean(kSecApp, L"showOverlay", out.showOverlay);
    std::wstring log;
    if (r.raw(kSecApp, L"logFile", log)) out.logFile = log;

    return r.ok();
}

bool saveConfig(const std::wstring& path, const Config& cfg) {
    if (path.empty() || !ensureUnicodeIni(path)) return false;
    bool ok = true;
    auto put = [&](const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
        if (!WritePrivateProfileStringW(sec, key, v.c_str(), path.c_str())) {
            MSD_ERROR("config: write [%s] %s failed (err %lu)", toUtf8(sec).c_str(), toUtf8(key).c_str(),
                      GetLastError());
            ok = false;
        }
    };
    auto num = [](int v) { return std::to_wstring(v); };
    wchar_t vic[8];
    swprintf(vic, std::size(vic), L"0x%02X", unsigned(cfg.mode.vic));

    put(kSecDisplay, L"rotation", num(int(cfg.rotation)));
    put(kSecDisplay, L"width", num(cfg.mode.width));
    put(kSecDisplay, L"height", num(cfg.mode.height));
    put(kSecDisplay, L"hz", num(cfg.mode.hz));
    put(kSecDisplay, L"vic", vic);
    put(kSecDisplay, L"virtualWidth", num(cfg.virtualWidth));
    put(kSecDisplay, L"virtualHeight", num(cfg.virtualHeight));
    put(kSecDisplay, L"fpsCap", num(cfg.fpsCap));
    put(kSecDisplay, L"tileW", num(cfg.tileW));
    put(kSecDisplay, L"tileH", num(cfg.tileH));
    put(kSecApp, L"stopWinUsbDisplay", cfg.stopWinUsbDisplay ? L"1" : L"0");
    put(kSecApp, L"showOverlay", cfg.showOverlay ? L"1" : L"0");
    put(kSecApp, L"logFile", cfg.logFile);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());  // flush the profile cache
    return ok;
}

std::wstring defaultConfigPath() {
    std::wstring dir = appDataDir();
    return dir.empty() ? std::wstring(L"msdisp.ini") : dir + L"\\msdisp.ini";
}

}  // namespace msdisp
