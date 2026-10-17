#include "overlay.h"

#include <mutex>

#include "core/util.h"
#include "msdisp/log.h"

namespace msdisp {

namespace {

constexpr wchar_t kClass[] = L"msdisp-overlay";
constexpr UINT kMsgReposition = WM_APP + 50;
constexpr BYTE kAlpha = 200;
constexpr COLORREF kBg = RGB(16, 16, 16);
constexpr COLORREF kFg = RGB(120, 255, 140);

HWND g_wnd = nullptr;
HFONT g_font = nullptr;
UINT g_fontDpi = 0;
std::wstring g_text = L"msdisp";
bool g_visible = false;
std::mutex g_monitorMutex;
std::wstring g_monitorName;

struct FindCtx {
    std::wstring name;
    RECT rc;
    bool found;
};

BOOL CALLBACK findMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindCtx*>(lp);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi) && _wcsicmp(mi.szDevice, ctx->name.c_str()) == 0) {
        ctx->rc = mi.rcMonitor;
        ctx->found = true;
        return FALSE;
    }
    return TRUE;
}

RECT targetMonitorRect() {
    FindCtx ctx;
    {
        std::lock_guard<std::mutex> lock(g_monitorMutex);
        ctx.name = g_monitorName;
    }
    ctx.found = false;
    if (!ctx.name.empty()) EnumDisplayMonitors(nullptr, nullptr, findMonitor, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found) return ctx.rc;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcMonitor;
}

void ensureFont(UINT dpi) {
    if (g_font && dpi == g_fontDpi) return;
    if (g_font) DeleteObject(g_font);
    g_font = CreateFontW(-MulDiv(10, int(dpi), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
                         L"Consolas");
    g_fontDpi = dpi;
}

void layout() {
    if (!g_wnd) return;
    RECT mon = targetMonitorRect();
    // Move first so GetDpiForWindow reports the DPI of the target monitor (per-monitor aware).
    SetWindowPos(g_wnd, HWND_TOPMOST, mon.left, mon.top, 1, 1, SWP_NOACTIVATE);
    UINT dpi = GetDpiForWindow(g_wnd);
    if (!dpi) dpi = 96;
    ensureFont(dpi);
    HDC dc = GetDC(g_wnd);
    HGDIOBJ old = SelectObject(dc, g_font);
    RECT r = {0, 0, 0, 0};
    DrawTextW(dc, g_text.c_str(), -1, &r, DT_CALCRECT | DT_NOPREFIX | DT_LEFT);
    SelectObject(dc, old);
    ReleaseDC(g_wnd, dc);
    int pad = MulDiv(6, int(dpi), 96), margin = MulDiv(8, int(dpi), 96);
    SetWindowPos(g_wnd, HWND_TOPMOST, mon.left + margin, mon.top + margin, r.right + 2 * pad, r.bottom + 2 * pad,
                 SWP_NOACTIVATE | (g_visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    InvalidateRect(g_wnd, nullptr, TRUE);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(kBg);
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            int pad = MulDiv(6, int(g_fontDpi ? g_fontDpi : 96), 96);
            InflateRect(&rc, -pad, -pad);
            HGDIOBJ old = SelectObject(dc, g_font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kFg);
            DrawTextW(dc, g_text.c_str(), -1, &rc, DT_NOPREFIX | DT_LEFT);
            SelectObject(dc, old);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
        case kMsgReposition: layout(); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool overlayCreate(HINSTANCE inst) {
    if (g_wnd) return true;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            kClass, L"msdisp overlay", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!g_wnd) {
        MSD_ERROR("overlay: CreateWindowEx failed (err %lu)", GetLastError());
        return false;
    }
    SetLayeredWindowAttributes(g_wnd, 0, kAlpha, LWA_ALPHA);
    layout();
    return true;
}

void overlayDestroy() {
    if (g_wnd) DestroyWindow(g_wnd);
    g_wnd = nullptr;
    if (g_font) DeleteObject(g_font);
    g_font = nullptr;
}

void overlayShow(bool show) {
    g_visible = show;
    if (g_wnd) layout();
}

void overlaySetText(const std::wstring& text) {
    if (text == g_text) return;
    g_text = text;
    if (g_wnd && g_visible) layout();
}

void overlayReposition() { layout(); }

void setOverlayMonitor(const std::wstring& gdiName) {
    {
        std::lock_guard<std::mutex> lock(g_monitorMutex);
        if (g_monitorName == gdiName) return;
        g_monitorName = gdiName;
    }
    MSD_INFO("overlay: monitor = %s", gdiName.empty() ? "(primary)" : toUtf8(gdiName).c_str());
    if (g_wnd) PostMessageW(g_wnd, kMsgReposition, 0, 0);
}

}  // namespace msdisp
