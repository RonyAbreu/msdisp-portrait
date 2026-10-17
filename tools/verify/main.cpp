// verify: end-to-end correctness check of capture + GPU convert + dirty-rect logic, without USB.
// Keeps a CPU "shadow" of what the chip would hold (applying every ConvertResult's dirty rects) and compares
// it, every frame, with a CPU reference conversion of the captured texture.
//   verify [--sec N] [--rot 90] [--no-cursor] [--no-paint]
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "msdisp/capture.h"
#include "msdisp/gpu.h"
#include "msdisp/log.h"
#include "msdisp/vdd.h"

using namespace msdisp;

namespace {
std::atomic<bool> g_quit{false};
RECT g_area{};

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER) {
        static std::mt19937 rng(1);
        HDC dc = GetDC(h);
        RECT rc;
        GetClientRect(h, &rc);
        for (int i = 0; i < 3; ++i) {
            int x = int(rng() % (rc.right - 40)), y = int(rng() % (rc.bottom - 40));
            RECT r{x, y, x + 5 + int(rng() % 120), y + 5 + int(rng() % 60)};
            HBRUSH b = CreateSolidBrush(RGB(rng() & 255, rng() & 255, rng() & 255));
            FillRect(dc, &r, b);
            DeleteObject(b);
        }
        ReleaseDC(h, dc);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void painterThread() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"msdisp_verify";
    wc.hbrBackground = HBRUSH(GetStockObject(GRAY_BRUSH));
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"verify",
                             WS_POPUP | WS_VISIBLE, g_area.left + 50, g_area.top + 300, 900, 1000, nullptr, nullptr,
                             wc.hInstance, nullptr);
    SetTimer(h, 1, 30, nullptr);
    MSG msg;
    while (!g_quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(5);
    }
    DestroyWindow(h);
}

void cursorThread() {
    POINT saved;
    GetCursorPos(&saved);
    double t = 0;
    while (!g_quit) {
        int cx = (g_area.left + g_area.right) / 2, cy = (g_area.top + g_area.bottom) / 2;
        SetCursorPos(cx + int(400 * cos(t)), cy + int(700 * sin(t * 0.7)));
        t += 0.05;
        Sleep(16);
    }
    SetCursorPos(saved.x, saved.y);
}

bool readTexture(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, std::vector<uint8_t>& out,
                 int& w, int& h) {
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    static ID3D11Texture2D* st = nullptr;
    static D3D11_TEXTURE2D_DESC sd{};
    if (!st || sd.Width != d.Width || sd.Height != d.Height) {
        if (st) st->Release();
        sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &st))) return false;
    }
    ctx->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) return false;
    w = int(d.Width), h = int(d.Height);
    out.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        memcpy(&out[size_t(y) * w * 4], static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(w) * 4);
    ctx->Unmap(st, 0);
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    int sec = 15;
    Rotation rot = Rotation::CW90;
    bool cursor = true, paint = true;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--sec") && i + 1 < argc) sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rot") && i + 1 < argc) rot = Rotation(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--no-cursor")) cursor = false;
        else if (!strcmp(argv[i], "--no-paint")) paint = false;
    }
    const int OW = 1920, OH = 1080;
    auto vdd = createParsecVdd();
    if (!vdd || !vdd->open()) return 1;
    int idx = vdd->add();
    std::wstring gdi;
    for (int i = 0; i < 50 && gdi.empty(); ++i, Sleep(100)) gdi = vdd->gdiName(idx);
    if (gdi.empty()) return 1;
    setDisplayMode(gdi, 1080, 1920, 60);
    Sleep(500);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(gdi.c_str(), ENUM_CURRENT_SETTINGS, &dm);
    g_area = {dm.dmPosition.x, dm.dmPosition.y, LONG(dm.dmPosition.x + dm.dmPelsWidth),
              LONG(dm.dmPosition.y + dm.dmPelsHeight)};
    printf("virtual %ls at %ld,%ld %lux%lu\n", gdi.c_str(), g_area.left, g_area.top, dm.dmPelsWidth, dm.dmPelsHeight);

    auto cap = createDxgiCapture();
    auto conv = createD3D11Converter();
    if (!cap->init(gdi) || !conv->init(cap->device(), cap->context(), OW, OH)) return 1;
    std::thread tp, tc;
    if (paint) tp = std::thread(painterThread);
    if (cursor) tc = std::thread(cursorThread);

    std::vector<uint8_t> shadow(size_t(OW) * OH * 2, 0), ref(shadow.size()), bgra;
    int frames = 0, timeouts = 0, bad = 0, emptyDirty = 0, lost = 0;
    long long worst = 0;
    bool forceFull = true;
    int64_t end = nowUs() + int64_t(sec) * 1000000;
    while (nowUs() < end) {
        CapturedFrame f;
        AcquireResult r = cap->acquire(f, 50);
        if (r == AcquireResult::Timeout) { ++timeouts; continue; }
        if (r != AcquireResult::Frame) {
            ++lost;
            cap->init(gdi);
            conv = createD3D11Converter();
            conv->init(cap->device(), cap->context(), OW, OH);
            forceFull = true;
            continue;
        }
        ConvertResult cr;
        if (!conv->process(f.texture, rot, f.dirty, forceFull, cr)) { printf("process failed\n"); break; }
        forceFull = false;
        ++frames;
        if (cr.dirty.empty()) ++emptyDirty;
        for (const Rect& d : cr.dirty)
            for (int y = d.y; y < d.bottom(); ++y)
                memcpy(&shadow[(size_t(y) * OW + d.x) * 2], cr.uyvy + size_t(y) * cr.pitch + size_t(d.x) * 2,
                       size_t(d.w) * 2);
        conv->done();
        int w, h;
        if (!readTexture(cap->device(), cap->context(), f.texture, bgra, w, h)) break;
        cap->release();
        cpuBgraToUyvyRotated(bgra.data(), w, h, w * 4, rot, ref.data(), OW * 2);
        long long diff = 0;
        int fx = -1, fy = -1;
        for (int y = 0; y < OH; ++y)
            for (int x = 0; x < OW * 2; ++x)
                if (shadow[size_t(y) * OW * 2 + x] != ref[size_t(y) * OW * 2 + x]) {
                    if (fx < 0) fx = x / 2, fy = y;
                    ++diff;
                }
        if (diff) {
            ++bad;
            if (diff > worst) worst = diff;
            if (bad <= 10)
                printf("frame %d: %lld bytes differ, first at out(%d,%d); dirty rects=%zu src dirty=%zu full=%d\n",
                       frames, diff, fx, fy, cr.dirty.size(), f.dirty.size(), int(f.full));
            memcpy(shadow.data(), ref.data(), ref.size());  // resync to keep counting independent errors
        }
    }
    g_quit = true;
    if (tp.joinable()) tp.join();
    if (tc.joinable()) tc.join();
    printf("frames=%d timeouts=%d lost=%d emptyDirty=%d BAD=%d worstBytes=%lld\n", frames, timeouts, lost, emptyDirty,
           bad, worst);
    cap->shutdown();
    vdd->remove(idx);
    vdd->close();
    return bad ? 3 : 0;
}
