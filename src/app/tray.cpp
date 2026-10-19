#include "tray.h"

#include "../pipeline/layout.h"

#include <dbt.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwctype>

#include "core/autostart.h"
#include "core/desktop_icons.h"
#include "core/modes.h"
#include "core/util.h"
#include "core/vendor.h"
#include "icon_data.h"
#include "msdisp/log.h"
#include "overlay.h"
#include "resource.h"

namespace msdisp {

namespace {

constexpr UINT kMsgTray = WM_APP + 1;
constexpr UINT kMsgPipeline = WM_APP + 3;  // worker finished a start/stop
constexpr UINT kTrayId = 1;

constexpr UINT_PTR kTimerStatus = 1;
constexpr UINT_PTR kTimerRestart = 2;
constexpr UINT kStatusPeriodMs = 1000;
constexpr UINT kRestartDebounceMs = 1500;
// Our own (re)start adds/changes a monitor, which echoes back as WM_DISPLAYCHANGE; ignore those for a while.
constexpr DWORD kSettleMs = 4000;
constexpr DWORD kRetryMinMs = 5000, kRetryMaxMs = 60000;

enum : UINT {
    IDM_STATUS = 100,
    IDM_PAUSE,
    IDM_ROT_0 = 110,
    IDM_ROT_90,
    IDM_ROT_180,
    IDM_ROT_270,
    IDM_MODE_BASE = 120,  // + index into kKnownModes
    IDM_OVERLAY = 140,
    IDM_AUTOSTART,
    IDM_VENDOR,
    IDM_OPEN_LOG,
    IDM_OPEN_CONFIG,
    IDM_DESK_RESTORE,
    IDM_DESK_SAVE,
    IDM_EXIT,
};

// GUID_DEVINTERFACE_USB_DEVICE (usbiodef.h)
const GUID kUsbDeviceInterface = {0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};

std::wstring fmt(const wchar_t* f, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, f);
    vswprintf(buf, std::size(buf), f, ap);
    va_end(ap);
    return buf;
}

// Tray icon: IDI_APP from the .rc when it is linked in, else the same image embedded in icon_data.h.
HICON loadTrayIcon(HINSTANCE inst) {
    UINT dpi = GetDpiForSystem();
    int cx = GetSystemMetricsForDpi(SM_CXSMICON, dpi), cy = GetSystemMetricsForDpi(SM_CYSMICON, dpi);
    if (auto h = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, cx, cy, 0))) return h;
    // ICONDIR (6 bytes) + ICONDIRENTRY (16 bytes each); pick the smallest image >= cx, else the largest.
    const unsigned char* ico = kAppIconIco;
    auto u16 = [&](size_t o) { return unsigned(ico[o] | (ico[o + 1] << 8)); };
    auto u32 = [&](size_t o) { return unsigned(u16(o) | (u16(o + 2) << 16)); };
    int best = -1, bestW = 0;
    for (unsigned i = 0, n = u16(4); i < n; ++i) {
        int w = ico[6 + 16 * i] ? ico[6 + 16 * i] : 256;
        bool better = best < 0 || (w >= cx && (bestW < cx || w < bestW)) || (bestW < cx && w > bestW);
        if (better) {
            best = int(i);
            bestW = w;
        }
    }
    if (best < 0) return nullptr;
    size_t e = 6 + 16 * size_t(best);
    return CreateIconFromResourceEx(const_cast<unsigned char*>(ico + u32(e + 12)), u32(e + 8), TRUE, 0x00030000, cx,
                                    cy, LR_DEFAULTCOLOR);
}

bool isOurAdapter(const wchar_t* name) {
    std::wstring s = name ? name : L"";
    for (auto& c : s) c = wchar_t(towlower(c));
    return s.find(L"vid_534d&pid_6021") != std::wstring::npos;
}

}  // namespace

TrayApp::TrayApp(HINSTANCE inst, AppOptions opts, std::unique_ptr<IPipeline> pipeline)
    : inst_(inst), opts_(std::move(opts)), cfg_(opts_.config) {
    controller_ = std::make_unique<PipelineController>(std::move(pipeline), [this] {
        if (hwnd_) PostMessageW(hwnd_, kMsgPipeline, 0, 0);
    });
}

TrayApp::~TrayApp() {
    if (controller_) controller_->shutdown();
    overlayDestroy();
    if (icon_) DestroyIcon(icon_);
}

bool TrayApp::init() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProcThunk;
    wc.hInstance = inst_;
    wc.lpszClassName = kTrayWindowClass;
    if (!RegisterClassExW(&wc)) {
        MSD_ERROR("tray: RegisterClassEx failed (err %lu)", GetLastError());
        return false;
    }
    // A real (hidden) top-level window, not HWND_MESSAGE: it must receive WM_DISPLAYCHANGE and TaskbarCreated.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayWindowClass, L"msdisp", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            inst_, this);
    if (!hwnd_) {
        MSD_ERROR("tray: CreateWindowEx failed (err %lu)", GetLastError());
        return false;
    }
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    icon_ = loadTrayIcon(inst_);
    addIcon();

    DEV_BROADCAST_DEVICEINTERFACE_W filter = {};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = kUsbDeviceInterface;
    devNotify_ = RegisterDeviceNotificationW(hwnd_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!devNotify_) MSD_WARN("tray: RegisterDeviceNotification failed (err %lu)", GetLastError());

    overlayCreate(inst_);
    overlayShow(cfg_.showOverlay);

    SetTimer(hwnd_, kTimerStatus, kStatusPeriodMs, nullptr);
    startPipeline("startup");
    refreshStatus();
    return true;
}

int TrayApp::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return int(msg.wParam);
}

LRESULT CALLBACK TrayApp::wndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<TrayApp*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->wndProc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayApp::wndProc(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_) {  // Explorer restarted: the icon is gone
        iconAdded_ = false;
        addIcon();
        return 0;
    }
    switch (msg) {
        case kMsgTray:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT: showMenu(); break;
            }
            return 0;
        case kMsgSecondInstance:
            balloon(L"msdisp", L"O msdisp já está em execução. Use o ícone na bandeja.");
            return 0;
        case kMsgPipeline: {
            settleUntilTick_ = GetTickCount() + kSettleMs;
            PipelineStatus s = controller_->status();
            if (controller_->wantRunning() && !controller_->busy()) {
                if (s.running) {
                    retryDelayMs_ = 0;
                } else {
                    retryDelayMs_ = retryDelayMs_ ? std::min(retryDelayMs_ * 2, kRetryMaxMs) : kRetryMinMs;
                    MSD_INFO("tray: start failed, retrying in %lu s", retryDelayMs_ / 1000);
                }
            }
            refreshStatus();
            return 0;
        }
        case WM_TIMER: onTimer(wp); return 0;
        case WM_COMMAND: onCommand(LOWORD(wp)); return 0;
        case WM_DISPLAYCHANGE:
            // The pipeline recovers from display changes itself (capture ACCESS_LOST -> re-init); restarting it
            // here would loop, because adding/removing our own virtual monitor is a display change too.
            overlayReposition();
            return 0;
        case WM_DEVICECHANGE: onDeviceChange(wp, lp); return TRUE;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION:
            if (wp) {
                MSD_INFO("tray: session ending");
                controller_->shutdown();
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kTimerStatus);
            KillTimer(hwnd_, kTimerRestart);
            if (devNotify_) UnregisterDeviceNotification(devNotify_);
            devNotify_ = nullptr;
            if (iconAdded_) {
                NOTIFYICONDATAW nid = {};
                nid.cbSize = sizeof(nid);
                nid.hWnd = hwnd_;
                nid.uID = kTrayId;
                Shell_NotifyIconW(NIM_DELETE, &nid);
                iconAdded_ = false;
            }
            controller_->shutdown();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void TrayApp::addIcon() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kMsgTray;
    nid.hIcon = icon_;
    wcsncpy(nid.szTip, lastTip_.empty() ? L"msdisp" : lastTip_.c_str(), std::size(nid.szTip) - 1);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        MSD_WARN("tray: NIM_ADD failed (Explorer not ready?)");
        return;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    iconAdded_ = true;
}

void TrayApp::balloon(const std::wstring& title, const std::wstring& text, DWORD flags) {
    MSD_INFO("tray: balloon \"%s\"", toUtf8(text).c_str());
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = flags;
    wcsncpy(nid.szInfoTitle, title.c_str(), std::size(nid.szInfoTitle) - 1);
    wcsncpy(nid.szInfo, text.c_str(), std::size(nid.szInfo) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

std::wstring TrayApp::statusLine() const {
    if (paused_) return L"Pausado";
    if (lastBusy_) return L"Iniciando…";
    if (last_.running) return fmt(L"%.1f fps · %.1f MB/s", last_.fps, last_.mbps);
    if (!last_.lastError.empty()) return L"Erro: " + fromUtf8(last_.lastError);
    return last_.text.empty() ? std::wstring(L"Parado") : fromUtf8(last_.text);
}

void TrayApp::refreshStatus() {
    last_ = controller_->status();
    lastBusy_ = controller_->busy();
    if (!last_.displayName.empty()) setOverlayMonitor(last_.displayName);

    std::wstring tip = L"msdisp — " + statusLine();
    if (tip.size() > 127) tip.resize(127);
    if (tip != lastTip_ && iconAdded_) {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        nid.uFlags = NIF_TIP | NIF_SHOWTIP;
        wcsncpy(nid.szTip, tip.c_str(), std::size(nid.szTip) - 1);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    lastTip_ = tip;

    if (cfg_.showOverlay) {
        std::wstring text;
        if (last_.running && !paused_) {
            text = fmt(L"msdisp %.1f fps  %.1f MB/s\nlatência %.0f ms  descartados %llu", last_.fps, last_.mbps,
                       last_.latencyMs, static_cast<unsigned long long>(last_.dropped));
        } else {
            text = L"msdisp " + statusLine();
        }
        overlaySetText(text);
    }
}

void TrayApp::onTimer(UINT_PTR id) {
    if (id == kTimerRestart) {
        KillTimer(hwnd_, kTimerRestart);
        if (!paused_) startPipeline("debounced event");
        return;
    }
    refreshStatus();
    // Retry with backoff while we want to run but the pipeline is down (adapter missing, start error, crash).
    if (!paused_ && controller_->wantRunning() && !controller_->busy() && !last_.running) {
        DWORD wait = retryDelayMs_ ? retryDelayMs_ : kRetryMinMs;
        if (GetTickCount() - lastStartTick_ >= wait) startPipeline("retry");
    }
}

void TrayApp::onDeviceChange(WPARAM event, LPARAM data) {
    if (event != DBT_DEVICEARRIVAL && event != DBT_DEVICEREMOVECOMPLETE) return;
    auto* hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(data);
    if (!hdr || hdr->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE) return;
    auto* di = reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(hdr);
    if (!isOurAdapter(di->dbcc_name)) return;
    bool arrival = event == DBT_DEVICEARRIVAL;
    MSD_INFO("tray: adapter %s", arrival ? "connected" : "removed");
    // A running pipeline reconnects to the adapter by itself; only kick a stopped one.
    if (arrival) {
        retryDelayMs_ = 0;
        if (!last_.running) scheduleRestart("adapter connected");
    }
}

void TrayApp::scheduleRestart(const char* why) {
    if (paused_) return;
    MSD_DEBUG("tray: restart scheduled (%s)", why);
    SetTimer(hwnd_, kTimerRestart, kRestartDebounceMs, nullptr);  // re-arming resets the debounce
}

void TrayApp::startPipeline(const char* why) {
    MSD_INFO("tray: (re)start pipeline: %s", why);
    // Both apps claim the adapter: make sure the vendor app is gone before every start (it may have been relaunched).
    if (cfg_.stopWinUsbDisplay && isVendorRunning()) {
        MSD_INFO("tray: stopping WinUsbDisplay.exe");
        if (!stopVendor()) balloon(L"msdisp", L"Não foi possível fechar o WinUsbDisplay.exe.", NIIF_WARNING);
    }
    lastStartTick_ = GetTickCount();
    controller_->requestStart(cfg_);
    refreshStatus();
}

void TrayApp::applyAndSave() {
    if (!saveConfig(opts_.configPath, cfg_)) {
        balloon(L"msdisp", L"Não foi possível salvar a configuração.", NIIF_WARNING);
    }
    if (!paused_) startPipeline("settings changed");
}

void TrayApp::showMenu() {
    refreshStatus();
    HMENU menu = CreatePopupMenu();
    HMENU rot = CreatePopupMenu();
    HMENU res = CreatePopupMenu();

    AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_STATUS, (L"Status: " + statusLine()).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_PAUSE, paused_ ? L"Retomar" : L"Pausar");

    const int rots[] = {0, 90, 180, 270};
    for (int i = 0; i < 4; ++i) AppendMenuW(rot, MF_STRING, IDM_ROT_0 + i, fmt(L"%d°", rots[i]).c_str());
    for (int i = 0; i < 4; ++i)
        if (int(cfg_.rotation) == rots[i]) CheckMenuRadioItem(rot, IDM_ROT_0, IDM_ROT_270, IDM_ROT_0 + i, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(rot), L"Rotação");

    const UINT nModes = UINT(std::size(kKnownModes));
    for (UINT i = 0; i < nModes; ++i) {
        const auto& k = kKnownModes[i];
        const wchar_t* hint = k.width == 1920 ? L"  (nítido)" : L"  (mais fluido)";
        AppendMenuW(res, MF_STRING, IDM_MODE_BASE + i, fmt(L"%dx%d@%d%ls", k.width, k.height, k.hz, hint).c_str());
        if (k.width == cfg_.mode.width && k.height == cfg_.mode.height && k.hz == cfg_.mode.hz)
            CheckMenuRadioItem(res, IDM_MODE_BASE, IDM_MODE_BASE + nModes - 1, IDM_MODE_BASE + i, MF_BYCOMMAND);
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(res), L"Resolução");

    AppendMenuW(menu, MF_STRING | (cfg_.showOverlay ? MF_CHECKED : 0), IDM_OVERLAY, L"Mostrar métricas na tela");
    AppendMenuW(menu, MF_STRING | (autostartEnabled() ? MF_CHECKED : 0), IDM_AUTOSTART, L"Iniciar com o Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    HMENU desk = CreatePopupMenu();
    AppendMenuW(desk, MF_STRING, IDM_DESK_RESTORE, L"Restaurar organização dos ícones");
    AppendMenuW(desk, MF_STRING, IDM_DESK_SAVE, L"Salvar organização atual");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(desk), L"Área de trabalho");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_VENDOR, L"Voltar ao driver original");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_LOG, L"Abrir log");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_CONFIG, L"Abrir configuração");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Sair");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);  // required so the menu closes when clicking elsewhere
    UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    UINT cmd = UINT(TrackPopupMenuEx(menu, flags, pt.x, pt.y, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);  // also destroys the submenus
    if (cmd) onCommand(cmd);
}

void TrayApp::onCommand(UINT id) {
    if (id >= IDM_ROT_0 && id <= IDM_ROT_270) {
        static const Rotation r[] = {Rotation::None, Rotation::CW90, Rotation::CW180, Rotation::CW270};
        Rotation want = r[id - IDM_ROT_0];
        if (want == cfg_.rotation) return;
        MSD_INFO("tray: rotation -> %d", int(want));
        cfg_.rotation = want;
        applyAndSave();
        return;
    }
    if (id >= IDM_MODE_BASE && id < IDM_MODE_BASE + std::size(kKnownModes)) {
        Mode m = toMode(kKnownModes[id - IDM_MODE_BASE]);
        if (m.width == cfg_.mode.width && m.height == cfg_.mode.height && m.hz == cfg_.mode.hz) return;
        MSD_INFO("tray: mode -> %dx%d@%d (vic 0x%02X)", m.width, m.height, m.hz, unsigned(m.vic));
        cfg_.mode = m;
        applyAndSave();
        return;
    }
    switch (id) {
        case IDM_PAUSE:
            paused_ = !paused_;
            MSD_INFO("tray: %s", paused_ ? "paused" : "resumed");
            KillTimer(hwnd_, kTimerRestart);
            if (paused_) {
                controller_->requestStop();
            } else {
                retryDelayMs_ = 0;
                startPipeline("resume");
            }
            refreshStatus();
            break;
        case IDM_OVERLAY:
            cfg_.showOverlay = !cfg_.showOverlay;
            saveConfig(opts_.configPath, cfg_);
            refreshStatus();
            overlayShow(cfg_.showOverlay);
            break;
        case IDM_AUTOSTART: {
            bool want = !autostartEnabled();
            if (!setAutostart(want)) balloon(L"msdisp", L"Não foi possível alterar o início automático.", NIIF_WARNING);
            break;
        }
        case IDM_VENDOR: returnToVendor(); break;
        case IDM_DESK_SAVE: {
            DesktopIconResult r = saveDesktopLayout(defaultDesktopLayoutPath());
            balloon(L"msdisp", r.ok ? fmt(L"Organização salva: %d ícones.", r.total) : L"Falhou: " + r.error,
                    r.ok ? NIIF_INFO : NIIF_WARNING);
            break;
        }
        case IDM_DESK_RESTORE: {
            const std::wstring path = defaultDesktopLayoutPath();
            if (!fileExists(path)) {
                balloon(L"msdisp", L"Nenhuma organização salva ainda. Use \"Salvar organização atual\" primeiro.",
                        NIIF_WARNING);
                break;
            }
            DesktopIconResult r = restoreDesktopLayout(path);
            balloon(L"msdisp",
                    r.ok ? fmt(L"Ícones reorganizados: %d de %d (%d novos).", r.placed + r.added, r.total, r.added)
                         : L"Falhou: " + r.error,
                    r.ok ? NIIF_INFO : NIIF_WARNING);
            break;
        }
        case IDM_OPEN_LOG: openFile(resolvedLogPath(cfg_)); break;
        case IDM_OPEN_CONFIG:
            if (!fileExists(opts_.configPath)) saveConfig(opts_.configPath, cfg_);
            openFile(opts_.configPath);
            break;
        case IDM_EXIT:
            MSD_INFO("tray: exit requested");
            DestroyWindow(hwnd_);
            break;
    }
}

void TrayApp::returnToVendor() {
    MSD_INFO("tray: returning to the vendor driver app");
    controller_->shutdown();  // releases the USB adapter before the vendor app grabs it
    reattachVendorOutputs();  // its monitor may still be parked (we park it while running)
    if (vendorAutostartRemovedByUs(opts_.configPath)) {
        std::wstring err;
        if (!setVendorAutostart(true, opts_.configPath, &err)) MSD_WARN("tray: vendor autostart not restored");
    }
    if (!startVendor(opts_.configPath)) {
        MessageBoxW(hwnd_,
                    L"Não foi possível iniciar o WinUsbDisplay.exe.\nInicie-o manualmente pelo menu Iniciar.",
                    L"msdisp", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
    DestroyWindow(hwnd_);
}

void TrayApp::openFile(const std::wstring& path) {
    if (!fileExists(path)) {
        balloon(L"msdisp", L"Arquivo não encontrado:\n" + path, NIIF_WARNING);
        return;
    }
    auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32) {  // no association for .log/.ini: fall back to Notepad
        std::wstring quoted = L"\"" + path + L"\"";
        ShellExecuteW(hwnd_, L"open", L"notepad.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

}  // namespace msdisp
