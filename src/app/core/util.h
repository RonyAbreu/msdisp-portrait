// msdisp-portrait — app shell helpers (paths, string conversion). Track D.
#pragma once
#include <string>

#include "msdisp/config.h"

namespace msdisp {

std::string toUtf8(const std::wstring& s);
std::wstring fromUtf8(const std::string& s);

// %APPDATA%\msdisp, created on demand. Empty if APPDATA cannot be resolved.
std::wstring appDataDir();
std::wstring defaultLogPath();                  // %APPDATA%\msdisp\msdisp.log
std::wstring resolvedLogPath(const Config& c);  // c.logFile, or the default when empty
std::wstring currentExePath();

// Creates every missing directory of `dir` (like mkdir -p). True if it exists afterwards.
bool ensureDirectory(const std::wstring& dir);
std::wstring parentDirectory(const std::wstring& path);  // "" when there is no separator
bool fileExists(const std::wstring& path);

// Creates `path` (and its directory) as an empty UTF-16LE INI with BOM if it does not exist, so the
// profile API (WritePrivateProfileStringW) stores Unicode instead of the ANSI code page.
bool ensureUnicodeIni(const std::wstring& path);

}  // namespace msdisp
