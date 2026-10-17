// msdisp-portrait — coexistence with the MacroSilicon vendor app (WinUsbDisplay.exe). Track D.
// Both programs claim the USB adapter, so msdisp stops the vendor app while it runs and can start it again.
#pragma once
#include <string>

namespace msdisp {

constexpr wchar_t kVendorExeName[] = L"WinUsbDisplay.exe";
constexpr wchar_t kVendorRunValue[] = L"Windows Usb Display";

// Where the vendor's Run value lives. On the reference machine it is HKCU (no admin needed);
// the installer may also use HKLM, which needs the elevated helper below.
enum class RunScope { None, User, Machine, Machine32 };

struct VendorAutostart {
    RunScope scope = RunScope::None;  // None = value not present anywhere
    std::wstring data;                // command line stored in the value
    unsigned long type = 0;           // REG_SZ / REG_EXPAND_SZ
    bool disabledByUser = false;      // present but switched off in Task Manager (StartupApproved)
};

// Full path of WinUsbDisplay.exe: running process > Run value > INI backup > default install dir.
std::wstring vendorExePath(const std::wstring& iniPath = {});

bool isVendorRunning();
// Terminates every WinUsbDisplay.exe of this session and waits until they are gone.
bool stopVendor(unsigned long timeoutMs = 5000);
// Launches the vendor app (non-elevated) unless it already runs.
bool startVendor(const std::wstring& iniPath = {});

VendorAutostart queryVendorAutostart();
bool vendorAutostartEnabled();  // value present and not disabled in Task Manager

// enable=false: backs the value up into [vendor_backup] of `iniPath`, verifies the backup, then deletes it.
// enable=true:  restores the value from the backup (or a default pointing at vendorExePath()).
// HKLM values are changed through an elevated copy of msdisp.exe (UAC prompt). On failure `error` says why.
bool setVendorAutostart(bool enable, const std::wstring& iniPath, std::wstring* error = nullptr);

// True if the INI holds a backup of a value that is currently missing (i.e. we removed it).
bool vendorAutostartRemovedByUs(const std::wstring& iniPath);

// Hidden command line of the elevated helper. `args` are the arguments after kElevatedFlag.
constexpr wchar_t kElevatedFlag[] = L"--elevated-vendor-run";
int runElevatedHelper(int argc, wchar_t** argv);

}  // namespace msdisp
