// msdisp-portrait — start msdisp.exe at logon for the current user (HKCU Run, no admin). Track D.
#pragma once
#include <string>

namespace msdisp {

constexpr wchar_t kAutostartValue[] = L"msdisp-portrait";

// Command stored in the Run value: "<path of this exe>".
std::wstring autostartCommand();
// True if the Run value exists (whatever exe it points to) and is not disabled in Task Manager.
bool autostartEnabled();
// True if the value exists but points to another msdisp.exe (e.g. an old build directory).
bool autostartPointsElsewhere();
bool setAutostart(bool enable);

}  // namespace msdisp
