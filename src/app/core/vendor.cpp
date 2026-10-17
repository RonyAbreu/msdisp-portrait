// Vendor app (WinUsbDisplay.exe) process control and its Run-key autostart value.
#include "vendor.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cwchar>
#include <cwctype>
#include <vector>

#include "msdisp/log.h"
#include "util.h"

namespace msdisp {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedRun[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kApprovedRun32[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32";
constexpr wchar_t kBackupSec[] = L"vendor_backup";
constexpr DWORD kHelperTimeoutMs = 120000;

HKEY rootOf(RunScope s) { return s == RunScope::User ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE; }

REGSAM viewOf(RunScope s) {
    if (s == RunScope::Machine) return KEY_WOW64_64KEY;
    if (s == RunScope::Machine32) return KEY_WOW64_32KEY;
    return 0;
}

const wchar_t* scopeName(RunScope s) {
    switch (s) {
        case RunScope::User: return L"user";
        case RunScope::Machine: return L"machine";
        case RunScope::Machine32: return L"machine32";
        default: return L"none";
    }
}

RunScope scopeFromName(const std::wstring& n) {
    if (n == L"user") return RunScope::User;
    if (n == L"machine") return RunScope::Machine;
    if (n == L"machine32") return RunScope::Machine32;
    return RunScope::None;
}

std::wstring toHex(const std::wstring& s) {
    static const wchar_t* digits = L"0123456789abcdef";
    std::wstring out;
    for (wchar_t c : s) {
        unsigned v = unsigned(c);
        for (int sh = 12; sh >= 0; sh -= 4) out += digits[(v >> sh) & 0xF];
    }
    return out;
}

bool fromHex(const std::wstring& h, std::wstring& out) {
    if (h.size() % 4) return false;
    out.clear();
    for (size_t i = 0; i < h.size(); i += 4) {
        unsigned v = 0;
        for (size_t j = 0; j < 4; ++j) {
            wchar_t c = h[i + j];
            int d = (c >= L'0' && c <= L'9') ? c - L'0' : (c >= L'a' && c <= L'f') ? c - L'a' + 10
                                                         : (c >= L'A' && c <= L'F') ? c - L'A' + 10
                                                                                    : -1;
            if (d < 0) return false;
            v = (v << 4) | unsigned(d);
        }
        out += wchar_t(v);
    }
    return true;
}

bool readRunValue(RunScope scope, std::wstring& data, DWORD& type) {
    HKEY key;
    if (RegOpenKeyExW(rootOf(scope), kRunKey, 0, KEY_QUERY_VALUE | viewOf(scope), &key) != ERROR_SUCCESS) return false;
    DWORD size = 0;
    LSTATUS st = RegQueryValueExW(key, kVendorRunValue, nullptr, &type, nullptr, &size);
    bool ok = false;
    if (st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(key, kVendorRunValue, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &size) ==
            ERROR_SUCCESS) {
            data = buf.data();
            ok = true;
        }
    }
    RegCloseKey(key);
    return ok;
}

// Task Manager's "Disable" writes a binary flag under StartupApproved; odd first byte = disabled.
bool disabledInStartupApproved(RunScope scope) {
    HKEY key;
    const wchar_t* path = scope == RunScope::Machine32 ? kApprovedRun32 : kApprovedRun;
    if (RegOpenKeyExW(rootOf(scope), path, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) return false;
    BYTE buf[16] = {};
    DWORD size = sizeof(buf), type = 0;
    bool disabled = RegQueryValueExW(key, kVendorRunValue, nullptr, &type, buf, &size) == ERROR_SUCCESS &&
                    type == REG_BINARY && size >= 1 && (buf[0] & 1);
    RegCloseKey(key);
    return disabled;
}

// Direct registry write; succeeds for HKCU, needs admin for HKLM.
DWORD writeRunValue(RunScope scope, const std::wstring& data, DWORD type) {
    HKEY key;
    LSTATUS st = RegCreateKeyExW(rootOf(scope), kRunKey, 0, nullptr, 0, KEY_SET_VALUE | viewOf(scope), nullptr, &key,
                                 nullptr);
    if (st != ERROR_SUCCESS) return DWORD(st);
    st = RegSetValueExW(key, kVendorRunValue, 0, type, reinterpret_cast<const BYTE*>(data.c_str()),
                        DWORD((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return DWORD(st);
}

DWORD deleteRunValue(RunScope scope) {
    HKEY key;
    LSTATUS st = RegOpenKeyExW(rootOf(scope), kRunKey, 0, KEY_SET_VALUE | viewOf(scope), &key);
    if (st != ERROR_SUCCESS) return DWORD(st);
    st = RegDeleteValueW(key, kVendorRunValue);
    RegCloseKey(key);
    return st == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : DWORD(st);
}

std::wstring errorText(DWORD err) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   err, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring s = msg ? msg : L"";
    LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s + L" (" + std::to_wstring(err) + L")";
}

// Runs this exe elevated with the hidden helper flag and returns its exit code (a Win32 error code).
DWORD runElevated(const std::wstring& params) {
    std::wstring exe = currentExePath();
    std::wstring args = std::wstring(kElevatedFlag) + L" " + params;
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return GetLastError();  // ERROR_CANCELLED if the UAC prompt was declined
    if (!sei.hProcess) return ERROR_INVALID_HANDLE;
    DWORD code = ERROR_TIMEOUT;
    if (WaitForSingleObject(sei.hProcess, kHelperTimeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code;
}

DWORD applyWrite(RunScope scope, const std::wstring& data, DWORD type) {
    if (scope == RunScope::User) return writeRunValue(scope, data, type);
    return runElevated(std::wstring(L"set ") + scopeName(scope) + L" " + std::to_wstring(type) + L" " + toHex(data));
}

DWORD applyDelete(RunScope scope) {
    if (scope == RunScope::User) return deleteRunValue(scope);
    return runElevated(std::wstring(L"delete ") + scopeName(scope));
}

struct Backup {
    RunScope scope = RunScope::None;
    std::wstring data;
    DWORD type = REG_SZ;
};

std::wstring iniGet(const std::wstring& ini, const wchar_t* key) {
    wchar_t buf[4096];
    GetPrivateProfileStringW(kBackupSec, key, L"", buf, DWORD(std::size(buf)), ini.c_str());
    return buf;
}

bool readBackup(const std::wstring& ini, Backup& b) {
    if (ini.empty() || !fileExists(ini)) return false;
    b.scope = scopeFromName(iniGet(ini, L"scope"));
    // data_hex is authoritative: the profile API strips surrounding quotes from plain values.
    if (b.scope == RunScope::None || !fromHex(iniGet(ini, L"data_hex"), b.data) || b.data.empty()) return false;
    b.type = iniGet(ini, L"type") == L"2" ? REG_EXPAND_SZ : REG_SZ;
    return true;
}

bool writeBackup(const std::wstring& ini, const VendorAutostart& v) {
    if (!ensureUnicodeIni(ini)) return false;
    bool ok = WritePrivateProfileStringW(kBackupSec, L"scope", scopeName(v.scope), ini.c_str()) &&
              WritePrivateProfileStringW(kBackupSec, L"type", std::to_wstring(v.type).c_str(), ini.c_str()) &&
              WritePrivateProfileStringW(kBackupSec, L"data", v.data.c_str(), ini.c_str()) &&
              WritePrivateProfileStringW(kBackupSec, L"data_hex", toHex(v.data).c_str(), ini.c_str());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
    Backup check;
    return ok && readBackup(ini, check) && check.scope == v.scope && check.data == v.data;
}

std::wstring exeFromCommandLine(const std::wstring& cmd, DWORD type) {
    std::wstring s = cmd;
    if (type == REG_EXPAND_SZ) {
        wchar_t buf[4096];
        if (ExpandEnvironmentStringsW(cmd.c_str(), buf, DWORD(std::size(buf)))) s = buf;
    }
    size_t b = s.find_first_not_of(L" \t");
    if (b == std::wstring::npos) return {};
    if (s[b] == L'"') {
        size_t e = s.find(L'"', b + 1);
        return s.substr(b + 1, e == std::wstring::npos ? std::wstring::npos : e - b - 1);
    }
    std::wstring lower = s;
    for (auto& c : lower) c = wchar_t(towlower(c));
    size_t e = lower.find(L".exe", b);
    return e == std::wstring::npos ? s.substr(b) : s.substr(b, e + 4 - b);
}

template <typename Fn>
void forEachVendorProcess(Fn&& fn) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    DWORD mySession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &mySession);
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        DWORD session = 0;
        if (_wcsicmp(pe.szExeFile, kVendorExeName) == 0 && ProcessIdToSessionId(pe.th32ProcessID, &session) &&
            session == mySession) {
            fn(pe.th32ProcessID);
        }
    }
    CloseHandle(snap);
}

}  // namespace

bool isVendorRunning() {
    bool found = false;
    forEachVendorProcess([&](DWORD) { found = true; });
    return found;
}

bool stopVendor(unsigned long timeoutMs) {
    std::vector<HANDLE> procs;
    forEachVendorProcess([&](DWORD pid) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (!h) {
            MSD_WARN("vendor: cannot open pid %lu (err %lu)", pid, GetLastError());
            return;
        }
        if (TerminateProcess(h, 0)) {
            MSD_INFO("vendor: terminated WinUsbDisplay.exe pid %lu", pid);
            procs.push_back(h);
        } else {
            MSD_WARN("vendor: terminate pid %lu failed (err %lu)", pid, GetLastError());
            CloseHandle(h);
        }
    });
    DWORD deadline = GetTickCount() + timeoutMs;
    for (HANDLE h : procs) {
        DWORD now = GetTickCount();
        WaitForSingleObject(h, now < deadline ? deadline - now : 0);
        CloseHandle(h);
    }
    // The process can linger briefly after its handle is signalled; poll until the snapshot is clean.
    while (isVendorRunning()) {
        if (GetTickCount() >= deadline) {
            MSD_ERROR("vendor: WinUsbDisplay.exe still running after %lu ms", timeoutMs);
            return false;
        }
        Sleep(50);
    }
    return true;
}

std::wstring vendorExePath(const std::wstring& iniPath) {
    std::wstring path;
    forEachVendorProcess([&](DWORD pid) {
        if (!path.empty()) return;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return;
        wchar_t buf[MAX_PATH * 2];
        DWORD n = DWORD(std::size(buf));
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) path.assign(buf, n);
        CloseHandle(h);
    });
    if (!path.empty()) return path;

    VendorAutostart a = queryVendorAutostart();
    if (a.scope != RunScope::None) path = exeFromCommandLine(a.data, a.type);
    if (!path.empty() && fileExists(path)) return path;

    Backup b;
    if (readBackup(iniPath, b)) path = exeFromCommandLine(b.data, b.type);
    if (!path.empty() && fileExists(path)) return path;

    wchar_t pf[MAX_PATH];
    if (!ExpandEnvironmentStringsW(L"%ProgramFiles%", pf, MAX_PATH)) wcscpy(pf, L"C:\\Program Files");
    return std::wstring(pf) + L"\\USM USB Display\\" + kVendorExeName;
}

bool startVendor(const std::wstring& iniPath) {
    if (isVendorRunning()) return true;
    std::wstring exe = vendorExePath(iniPath);
    if (!fileExists(exe)) {
        MSD_ERROR("vendor: %s not found", toUtf8(exe).c_str());
        return false;
    }
    std::wstring dir = parentDirectory(exe);
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"open";
    sei.lpFile = exe.c_str();
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        MSD_ERROR("vendor: launch %s failed (err %lu)", toUtf8(exe).c_str(), GetLastError());
        return false;
    }
    MSD_INFO("vendor: launched %s", toUtf8(exe).c_str());
    return true;
}

VendorAutostart queryVendorAutostart() {
    VendorAutostart v;
    for (RunScope s : {RunScope::User, RunScope::Machine, RunScope::Machine32}) {
        std::wstring data;
        DWORD type = 0;
        if (readRunValue(s, data, type)) {
            v.scope = s;
            v.data = data;
            v.type = type;
            v.disabledByUser = disabledInStartupApproved(s);
            break;
        }
    }
    return v;
}

bool vendorAutostartEnabled() {
    VendorAutostart v = queryVendorAutostart();
    return v.scope != RunScope::None && !v.disabledByUser;
}

bool vendorAutostartRemovedByUs(const std::wstring& iniPath) {
    Backup b;
    return queryVendorAutostart().scope == RunScope::None && readBackup(iniPath, b);
}

bool setVendorAutostart(bool enable, const std::wstring& iniPath, std::wstring* error) {
    auto fail = [&](const std::wstring& why) {
        MSD_ERROR("vendor autostart: %s", toUtf8(why).c_str());
        if (error) *error = why;
        return false;
    };
    VendorAutostart cur = queryVendorAutostart();
    if (!enable) {
        if (cur.scope == RunScope::None) return true;  // already off
        if (!writeBackup(iniPath, cur)) return fail(L"não foi possível salvar o valor original em " + iniPath);
        DWORD err = applyDelete(cur.scope);
        if (err != ERROR_SUCCESS) return fail(L"remover o valor falhou: " + errorText(err));
        if (queryVendorAutostart().scope != RunScope::None) return fail(L"o valor ainda existe após a remoção");
        MSD_INFO("vendor autostart: removed (%s), backup in %s", toUtf8(scopeName(cur.scope)).c_str(),
                 toUtf8(iniPath).c_str());
        return true;
    }
    if (cur.scope != RunScope::None) return true;  // already on
    Backup b;
    if (!readBackup(iniPath, b)) {
        // No backup: recreate the value the vendor installer writes (HKCU on the reference machine).
        b.scope = RunScope::User;
        b.data = vendorExePath(iniPath);
        b.type = REG_SZ;
        MSD_WARN("vendor autostart: no backup, recreating default %s", toUtf8(b.data).c_str());
    }
    DWORD err = applyWrite(b.scope, b.data, b.type);
    if (err != ERROR_SUCCESS) return fail(L"restaurar o valor falhou: " + errorText(err));
    if (queryVendorAutostart().scope == RunScope::None) return fail(L"o valor não aparece após a restauração");
    MSD_INFO("vendor autostart: restored (%s) %s", toUtf8(scopeName(b.scope)).c_str(), toUtf8(b.data).c_str());
    return true;
}

// argv[0..] = arguments after kElevatedFlag: "set <scope> <type> <hexdata>" | "delete <scope>".
// Only ever touches the vendor's own value name.
int runElevatedHelper(int argc, wchar_t** argv) {
    if (argc < 2) return ERROR_INVALID_PARAMETER;
    std::wstring op = argv[0];
    RunScope scope = scopeFromName(argv[1]);
    if (scope == RunScope::None) return ERROR_INVALID_PARAMETER;
    if (op == L"delete" && argc == 2) return int(deleteRunValue(scope));
    if (op == L"set" && argc == 4) {
        std::wstring data;
        DWORD type = wcscmp(argv[2], L"2") == 0 ? REG_EXPAND_SZ : REG_SZ;
        if (!fromHex(argv[3], data) || data.empty()) return ERROR_INVALID_DATA;
        return int(writeRunValue(scope, data, type));
    }
    return ERROR_INVALID_PARAMETER;
}

}  // namespace msdisp
