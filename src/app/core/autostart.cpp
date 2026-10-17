#include "autostart.h"

#include <windows.h>

#include <vector>

#include "msdisp/log.h"
#include "util.h"

namespace msdisp {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedRun[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

bool readValue(std::wstring& out) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    bool ok = false;
    if (RegQueryValueExW(key, kAutostartValue, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, L'\0');
        ok = RegQueryValueExW(key, kAutostartValue, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &size) ==
             ERROR_SUCCESS;
        if (ok) out = buf.data();
    }
    RegCloseKey(key);
    return ok;
}

bool disabledInTaskManager() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedRun, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    BYTE buf[16] = {};
    DWORD size = sizeof(buf), type = 0;
    bool disabled = RegQueryValueExW(key, kAutostartValue, nullptr, &type, buf, &size) == ERROR_SUCCESS &&
                    type == REG_BINARY && size >= 1 && (buf[0] & 1);
    RegCloseKey(key);
    return disabled;
}

// Clears a Task Manager "disabled" flag so re-enabling from the tray really takes effect.
void clearTaskManagerFlag() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedRun, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    RegDeleteValueW(key, kAutostartValue);
    RegCloseKey(key);
}

}  // namespace

std::wstring autostartCommand() { return L"\"" + currentExePath() + L"\""; }

bool autostartEnabled() {
    std::wstring v;
    return readValue(v) && !disabledInTaskManager();
}

bool autostartPointsElsewhere() {
    std::wstring v;
    return readValue(v) && _wcsicmp(v.c_str(), autostartCommand().c_str()) != 0;
}

bool setAutostart(bool enable) {
    HKEY key;
    LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) {
        MSD_ERROR("autostart: open HKCU Run failed (err %ld)", long(st));
        return false;
    }
    if (enable) {
        std::wstring cmd = autostartCommand();
        st = RegSetValueExW(key, kAutostartValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kAutostartValue);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) {
        MSD_ERROR("autostart: %s failed (err %ld)", enable ? "enable" : "disable", long(st));
        return false;
    }
    clearTaskManagerFlag();
    MSD_INFO("autostart: %s", enable ? "enabled" : "disabled");
    return true;
}

}  // namespace msdisp
