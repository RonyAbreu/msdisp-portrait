// msdisp-portrait — tray application (hidden window, notify icon, menu, device/display events). Track D.
#pragma once
#include <windows.h>
#include <shellapi.h>

#include <memory>
#include <string>

#include "core/controller.h"
#include "msdisp/config.h"

namespace msdisp {

constexpr wchar_t kTrayWindowClass[] = L"msdisp-portrait-tray";
constexpr UINT kMsgSecondInstance = WM_APP + 2;  // posted by a second msdisp.exe before it exits

struct AppOptions {
    std::wstring configPath;
    Config config;
};

class TrayApp {
public:
    TrayApp(HINSTANCE inst, AppOptions opts, std::unique_ptr<IPipeline> pipeline);
    ~TrayApp();
    bool init();
    int run();  // message loop; returns the exit code

private:
    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT wndProc(UINT msg, WPARAM wp, LPARAM lp);

    void addIcon();
    void balloon(const std::wstring& title, const std::wstring& text, DWORD flags = NIIF_INFO);
    void showMenu();
    void onCommand(UINT id);
    void onTimer(UINT_PTR id);
    void onDeviceChange(WPARAM event, LPARAM data);
    void scheduleRestart(const char* why);
    void startPipeline(const char* why);
    void applyAndSave();
    void refreshStatus();
    std::wstring statusLine() const;
    void returnToVendor();
    void openFile(const std::wstring& path);

    HINSTANCE inst_;
    AppOptions opts_;
    Config& cfg_;
    std::unique_ptr<PipelineController> controller_;
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    HDEVNOTIFY devNotify_ = nullptr;
    UINT taskbarCreatedMsg_ = 0;
    bool paused_ = false;
    bool iconAdded_ = false;
    PipelineStatus last_;
    bool lastBusy_ = false;
    DWORD lastStartTick_ = 0;
    DWORD retryDelayMs_ = 0;
    DWORD settleUntilTick_ = 0;
    std::wstring lastTip_;
};

}  // namespace msdisp
