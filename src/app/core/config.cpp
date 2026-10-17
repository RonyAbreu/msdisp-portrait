// STUB — track D replaces this file.
#include "msdisp/config.h"
namespace msdisp {
bool loadConfig(const std::wstring&, Config&) { return true; }
bool saveConfig(const std::wstring&, const Config&) { return false; }
std::wstring defaultConfigPath() { return L""; }
}  // namespace msdisp
