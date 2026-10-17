#include "util.h"

#include <windows.h>
#include <shlobj.h>

#include "msdisp/log.h"

namespace msdisp {

std::string toUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::wstring parentDirectory(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

bool fileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ensureDirectory(const std::wstring& dir) {
    if (dir.empty()) return false;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring parent = parentDirectory(dir);
    // Stop at drive roots ("C:") and UNC prefixes; CreateDirectoryW reports the real error.
    if (!parent.empty() && parent.back() != L':' && parent != L"\\" && parent != L"\\\\") ensureDirectory(parent);
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ensureUnicodeIni(const std::wstring& path) {
    if (fileExists(path)) return true;
    std::wstring dir = parentDirectory(path);
    if (!dir.empty() && !ensureDirectory(dir)) {
        MSD_ERROR("ini: cannot create directory %s (err %lu)", toUtf8(dir).c_str(), GetLastError());
        return false;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_EXISTS) return true;
        MSD_ERROR("ini: cannot create %s (err %lu)", toUtf8(path).c_str(), GetLastError());
        return false;
    }
    static const wchar_t kHeader[] = L"\xFEFF; msdisp-portrait configuration\r\n";
    DWORD written = 0;
    BOOL ok = WriteFile(h, kHeader, DWORD(sizeof(kHeader) - sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

std::wstring appDataDir() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &base)) && base) {
        dir = std::wstring(base) + L"\\msdisp";
    }
    CoTaskMemFree(base);
    if (!dir.empty()) ensureDirectory(dir);
    return dir;
}

std::wstring defaultLogPath() {
    std::wstring dir = appDataDir();
    return dir.empty() ? std::wstring(L"msdisp.log") : dir + L"\\msdisp.log";
}

std::wstring resolvedLogPath(const Config& c) { return c.logFile.empty() ? defaultLogPath() : c.logFile; }

std::wstring currentExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

}  // namespace msdisp
