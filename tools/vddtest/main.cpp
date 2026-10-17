// msdisp-vddtest: track B end-to-end check of the Parsec virtual display + DXGI capture.
//   msdisp-vddtest [--sec N] [--bmp file.bmp] [--slot S] [--no-cursor]   add a 1080x1920 virtual display, capture it
//   msdisp-vddtest --primary [--sec N] [--bmp file.bmp]                  capture the primary display only
//   add --verify to check dirty rects against full readbacks (and sweep the cursor over the display)
//   msdisp-vddtest --list                                                 list Parsec targets and display devices
//   msdisp-vddtest --rotated-probe                                        fallback probe: 1920x1080 + DMDO_90, print
//                                                                         what DXGI duplication reports
// The virtual display is always removed and the other monitors' layout restored, also on error and Ctrl+C.
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

#include "../../src/vdd/vdd_internal.h"
#include "msdisp/capture.h"
#include "msdisp/log.h"
#include "msdisp/vdd.h"

using Microsoft::WRL::ComPtr;
using namespace msdisp;

namespace {

std::atomic<bool> g_stop{false};
HANDLE g_cleanedUp = nullptr;

BOOL WINAPI onCtrl(DWORD type) {
    g_stop = true;
    // For console close / logoff the process dies when we return: give main() time to remove the display.
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) WaitForSingleObject(g_cleanedUp, 5000);
    return TRUE;
}

struct MonitorPos {
    std::wstring name;
    DEVMODEW dm{};
};

std::vector<MonitorPos> snapshotLayout() {
    std::vector<MonitorPos> v;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        MonitorPos m;
        m.name = dd.DeviceName;
        m.dm.dmSize = sizeof(m.dm);
        if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &m.dm)) continue;
        MSD_INFO("layout: %ls %lux%lu at %ld,%ld", dd.DeviceName, m.dm.dmPelsWidth, m.dm.dmPelsHeight,
                 m.dm.dmPosition.x, m.dm.dmPosition.y);
        v.push_back(m);
    }
    return v;
}

void restoreLayout(const std::vector<MonitorPos>& before) {
    bool changed = false;
    for (auto& m : before) {
        DEVMODEW cur{};
        cur.dmSize = sizeof(cur);
        if (!EnumDisplaySettingsW(m.name.c_str(), ENUM_CURRENT_SETTINGS, &cur)) continue;
        if (cur.dmPosition.x == m.dm.dmPosition.x && cur.dmPosition.y == m.dm.dmPosition.y &&
            cur.dmPelsWidth == m.dm.dmPelsWidth && cur.dmPelsHeight == m.dm.dmPelsHeight)
            continue;
        MSD_WARN("restoring %ls to %ldx%ld at %ld,%ld", m.name.c_str(), m.dm.dmPelsWidth, m.dm.dmPelsHeight,
                 m.dm.dmPosition.x, m.dm.dmPosition.y);
        DEVMODEW dm = m.dm;
        dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_DISPLAYORIENTATION;
        ChangeDisplaySettingsExW(m.name.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        changed = true;
    }
    if (changed) ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    MSD_INFO("layout %s", changed ? "restored" : "unchanged");
}

// Owns the virtual display: removing it and restoring the layout happens in the destructor.
struct VirtualDisplayGuard {
    std::unique_ptr<IVirtualDisplay> vdd = createParsecVdd();
    int index = -1;
    std::vector<MonitorPos> layout = snapshotLayout();
    ~VirtualDisplayGuard() {
        if (index >= 0) vdd->remove(index);
        vdd->close();
        if (index >= 0) {
            Sleep(500);
            restoreLayout(layout);
        }
    }
};

bool placeAt(const std::wstring& name, int x, int y) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return false;
    dm.dmPosition.x = x;
    dm.dmPosition.y = y;
    dm.dmFields = DM_POSITION;
    if (ChangeDisplaySettingsExW(name.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr) !=
        DISP_CHANGE_SUCCESSFUL)
        return false;
    return ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr) == DISP_CHANGE_SUCCESSFUL;
}

bool desktopRect(const std::wstring& name, RECT& r) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return false;
    r = {dm.dmPosition.x, dm.dmPosition.y, LONG(dm.dmPosition.x + dm.dmPelsWidth),
         LONG(dm.dmPosition.y + dm.dmPelsHeight)};
    return true;
}

// ---- test pattern window: a counter encoded in its color + a moving box, repainted every vblank ----------------
constexpr int kWinW = 480, kWinH = 360, kProbeX = 8, kProbeY = 8;
std::atomic<uint32_t> g_counter{0};
int g_paintMs = 1;  // --paint-ms: delay between test pattern paints (0 = flat out, -1 = DwmFlush)
std::vector<int64_t> g_paintTime(1 << 16);

struct Animator {
    std::thread th;
    std::atomic<bool> quit{false};
    HWND hwnd = nullptr;

    void start(int x, int y) {
        th = std::thread([this, x, y] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"msdisp_vddtest";
            RegisterClassW(&wc);
            hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"vddtest",
                                   WS_POPUP | WS_VISIBLE, x, y, kWinW, kWinH, nullptr, nullptr, wc.hInstance, nullptr);
            HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
            int box = 0;
            while (!quit) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
                uint32_t c = (g_counter + 1) & 0xFFFF;
                HDC dc = GetDC(hwnd);
                HBRUSH b = CreateSolidBrush(RGB(c & 0xFF, (c >> 8) & 0xFF, 0x80));
                RECT all{0, 0, kWinW, kWinH};
                FillRect(dc, &all, b);
                DeleteObject(b);
                box = (box + 6) % (kWinW - 40);
                RECT br{box, 200, box + 40, 240};
                FillRect(dc, &br, white);
                GdiFlush();
                ReleaseDC(hwnd, dc);
                g_paintTime[c] = nowUs();
                g_counter = c;
                if (g_paintMs < 0)
                    DwmFlush();  // one paint per DWM composition of the primary display
                else if (g_paintMs > 0)
                    Sleep(DWORD(g_paintMs));
            }
            DeleteObject(white);
            DestroyWindow(hwnd);
        });
    }
    void stop() {
        quit = true;
        if (th.joinable()) th.join();
    }
    ~Animator() { stop(); }
};

// Reads one pixel of the captured texture (BGRA).
uint32_t readPixel(ICapture& cap, ID3D11Texture2D* tex, int x, int y) {
    static ComPtr<ID3D11Texture2D> st;
    if (!st) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        cap.device()->CreateTexture2D(&td, nullptr, &st);
    }
    D3D11_BOX box{UINT(x), UINT(y), 0, UINT(x + 1), UINT(y + 1), 1};
    cap.context()->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t v = 0;
    if (SUCCEEDED(cap.context()->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        v = *static_cast<uint32_t*>(m.pData);
        cap.context()->Unmap(st.Get(), 0);
    }
    return v;
}

bool saveBmp(ICapture& cap, const CapturedFrame& f, const wchar_t* path) {
    D3D11_TEXTURE2D_DESC td;
    f.texture->GetDesc(&td);
    td.BindFlags = 0;
    td.MiscFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(cap.device()->CreateTexture2D(&td, nullptr, &st))) return false;
    cap.context()->CopyResource(st.Get(), f.texture);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(cap.context()->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    FILE* fp = _wfopen(path, L"wb");
    if (!fp) {
        cap.context()->Unmap(st.Get(), 0);
        return false;
    }
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    const DWORD rowBytes = DWORD(f.width) * 4, imgBytes = rowBytes * DWORD(f.height);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + imgBytes;
    ih.biSize = sizeof(ih);
    ih.biWidth = f.width;
    ih.biHeight = -f.height;  // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = imgBytes;
    fwrite(&fh, sizeof(fh), 1, fp);
    fwrite(&ih, sizeof(ih), 1, fp);
    for (int y = 0; y < f.height; ++y) fwrite(static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, rowBytes, 1, fp);
    fclose(fp);
    cap.context()->Unmap(st.Get(), 0);
    return true;
}

// Full readback into a tightly packed BGRA buffer (for --verify).
bool readAll(ICapture& cap, ID3D11Texture2D* tex, std::vector<uint32_t>& px) {
    static ComPtr<ID3D11Texture2D> st;
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    if (st) {
        D3D11_TEXTURE2D_DESC sd;
        st->GetDesc(&sd);
        if (sd.Width != td.Width || sd.Height != td.Height) st.Reset();
    }
    if (!st) {
        td.BindFlags = td.MiscFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(cap.device()->CreateTexture2D(&td, nullptr, &st))) return false;
    }
    cap.context()->CopyResource(st.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(cap.context()->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    px.resize(size_t(td.Width) * td.Height);
    for (UINT y = 0; y < td.Height; ++y)
        memcpy(&px[size_t(y) * td.Width], static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, td.Width * 4);
    cap.context()->Unmap(st.Get(), 0);
    return true;
}

struct Stats {
    int frames = 0, fullFrames = 0, timeouts = 0;
    int verified = 0;
    long long mismatchFrames = 0, mismatchPixels = 0;
    long long rects = 0, area = 0;
    std::vector<double> latMs;       // paint -> captured (ms), from the color counter
    std::vector<double> acquireMs;   // wall time of acquire() calls returning a frame
};

void printStats(const Stats& s, double sec, int w, int h) {
    auto pct = [](std::vector<double> v, double p) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, size_t(p * double(v.size())))];
    };
    printf("frames %d in %.1f s = %.1f fps, full %d, timeouts %d\n", s.frames, sec, s.frames / sec, s.fullFrames,
           s.timeouts);
    if (s.frames)
        printf("dirty rects/frame %.1f, dirty area/frame %.1f%% of %dx%d\n", double(s.rects) / s.frames,
               100.0 * double(s.area) / s.frames / (double(w) * h), w, h);
    if (!s.latMs.empty())
        printf("paint->capture latency ms: p50 %.2f p90 %.2f max %.2f (n=%zu)\n", pct(s.latMs, 0.5), pct(s.latMs, 0.9),
               pct(s.latMs, 1.0), s.latMs.size());
    if (s.verified)
        printf("verify: %d frames checked, %lld frames / %lld pixels differ from the dirty-rect mirror\n", s.verified,
               s.mismatchFrames, s.mismatchPixels);
    if (!s.acquireMs.empty())
        printf("acquire() call ms (incl. wait): p50 %.2f p90 %.2f max %.2f\n", pct(s.acquireMs, 0.5),
               pct(s.acquireMs, 0.9), pct(s.acquireMs, 1.0));
}

// Captures `name` for `sec` seconds. probe = desktop-relative position of the animated window's probe pixel.
// verify: keeps a CPU mirror updated only through the reported dirty rects and compares it with a full readback
// of every frame; during the last second the cursor is swept across the display (pointer-only updates).
int captureLoop(const std::wstring& name, double sec, const wchar_t* bmp, POINT probe, bool animated, bool cursor,
                RECT deskRect, bool verify) {
    auto cap = createDxgiCapture();
    if (!cap->init(name)) return 1;
    Stats s;
    std::vector<uint32_t> mirror, cur;
    POINT savedCursor{};
    GetCursorPos(&savedCursor);
    bool sweeping = false;
    int w = 0, h = 0;
    const int64_t t0 = nowUs();
    while (!g_stop && nowUs() - t0 < int64_t(sec * 1e6)) {
        const double el = (nowUs() - t0) / 1e6;
        if (verify && cursor && el > sec - 1.0) {
            sweeping = true;
            double t = el - (sec - 1.0);
            SetCursorPos(deskRect.left + 50 + int(t * 900), deskRect.top + 600 + int(t * 300));
        }
        CapturedFrame f;
        int64_t a = nowUs();
        AcquireResult r = cap->acquire(f, 100);
        if (r == AcquireResult::Timeout) {
            ++s.timeouts;
            continue;
        }
        if (r == AcquireResult::Lost) {
            MSD_WARN("lost, re-init");
            Sleep(50);
            if (!cap->init(name)) return 1;
            continue;
        }
        if (r != AcquireResult::Frame) return 1;
        s.acquireMs.push_back((nowUs() - a) / 1000.0);
        ++s.frames;
        s.fullFrames += f.full;
        s.rects += static_cast<long long>(f.dirty.size());
        for (auto& d : f.dirty) s.area += static_cast<long long>(d.w) * d.h;
        w = f.width, h = f.height;
        if (animated && !f.full) {
            bool hit = false;
            for (auto& d : f.dirty)
                hit |= probe.x >= d.x && probe.x < d.right() && probe.y >= d.y && probe.y < d.bottom();
            if (hit) {
                uint32_t px = readPixel(*cap, f.texture, probe.x, probe.y);
                uint32_t c = ((px >> 16) & 0xFF) | (((px >> 8) & 0xFF) << 8);  // R | G<<8 (BGRA)
                if ((px & 0xFF) == 0x80 && g_paintTime[c]) s.latMs.push_back((f.timeUs - g_paintTime[c]) / 1000.0);
            }
        }
        if (verify && readAll(*cap, f.texture, cur)) {
            if (mirror.size() != cur.size()) mirror.assign(cur.size(), 0);
            for (auto& d : f.dirty)
                for (int y = d.y; y < d.bottom(); ++y)
                    memcpy(&mirror[size_t(y) * f.width + d.x], &cur[size_t(y) * f.width + d.x], size_t(d.w) * 4);
            long long diff = 0;
            for (size_t i = 0; i < cur.size(); ++i) diff += mirror[i] != cur[i];
            ++s.verified;
            if (diff) {
                ++s.mismatchFrames;
                s.mismatchPixels += diff;
                mirror = cur;
            }
        }
        cap->release();
    }
    if (sweeping) SetCursorPos(savedCursor.x, savedCursor.y);
    printStats(s, (nowUs() - t0) / 1e6, w, h);

    // Final frame for the BMP, with the cursor parked over the captured display (restored afterwards).
    POINT saved{};
    GetCursorPos(&saved);
    if (cursor) SetCursorPos(deskRect.left + 300, deskRect.top + 120);
    CapturedFrame f;
    bool saved_ok = false;
    for (int i = 0; i < 20 && !saved_ok; ++i) {
        AcquireResult r = cap->acquire(f, 100);
        if (r == AcquireResult::Frame) {
            if (i >= 3 || !cursor) saved_ok = saveBmp(*cap, f, bmp);
            cap->release();
        } else if (r == AcquireResult::Lost) {
            if (!cap->init(name)) break;
        }
    }
    if (cursor) SetCursorPos(saved.x, saved.y);
    printf("bmp %ls: %s\n", bmp, saved_ok ? "saved" : "FAILED");
    cap->shutdown();
    return saved_ok ? 0 : 1;
}

void listAll() {
    for (auto& t : vdd::listParsecTargets(false))
        printf("parsec target id=%u connector=%u active=%d gdi=%ls\n  %ls\n", t.targetId, t.connector, t.active,
               t.gdiName.c_str(), t.monitorPath.c_str());
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm);
        printf("%ls '%ls' %lux%lu@%lu orient=%lu at %ld,%ld\n", dd.DeviceName, dd.DeviceString, dm.dmPelsWidth,
               dm.dmPelsHeight, dm.dmDisplayFrequency, dm.dmDisplayOrientation, dm.dmPosition.x, dm.dmPosition.y);
    }
    ComPtr<IDXGIFactory1> f;
    CreateDXGIFactory1(IID_PPV_ARGS(&f));
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 ad;
        a->GetDesc1(&ad);
        printf("DXGI adapter %u '%ls'\n", i, ad.Description);
        ComPtr<IDXGIOutput> o;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j) {
            DXGI_OUTPUT_DESC od;
            o->GetDesc(&od);
            printf("  output %ls rot=%d\n", od.DeviceName, int(od.Rotation));
        }
    }
}

// Fallback probe: landscape mode rotated by Windows, report what DXGI duplication gives.
void rotatedProbe(const std::wstring& name) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(name.c_str(), ENUM_CURRENT_SETTINGS, &dm);
    dm.dmPelsWidth = 1080;
    dm.dmPelsHeight = 1920;
    dm.dmDisplayFrequency = 60;
    dm.dmDisplayOrientation = DMDO_90;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_DISPLAYORIENTATION;
    LONG rc = ChangeDisplaySettingsExW(name.c_str(), &dm, nullptr, 0, nullptr);
    printf("1920x1080 + DMDO_90: ChangeDisplaySettingsEx = %ld\n", long(rc));
    Sleep(1000);
    ComPtr<IDXGIFactory1> f;
    CreateDXGIFactory1(IID_PPV_ARGS(&f));
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        ComPtr<IDXGIOutput> o;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j) {
            DXGI_OUTPUT_DESC od;
            o->GetDesc(&od);
            if (_wcsicmp(od.DeviceName, name.c_str())) continue;
            printf("output desktop %ldx%ld rot=%d\n", od.DesktopCoordinates.right - od.DesktopCoordinates.left,
                   od.DesktopCoordinates.bottom - od.DesktopCoordinates.top, int(od.Rotation));
            ComPtr<ID3D11Device> dev;
            D3D11CreateDevice(a.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                              nullptr, nullptr);
            ComPtr<IDXGIOutput1> o1;
            o.As(&o1);
            ComPtr<IDXGIOutputDuplication> dup;
            HRESULT hr = o1->DuplicateOutput(dev.Get(), &dup);
            if (FAILED(hr)) {
                printf("DuplicateOutput failed 0x%08lx\n", hr);
                continue;
            }
            DXGI_OUTDUPL_DESC dd;
            dup->GetDesc(&dd);
            printf("OUTDUPL_DESC ModeDesc %ux%u Rotation=%d\n", dd.ModeDesc.Width, dd.ModeDesc.Height,
                   int(dd.Rotation));
            DXGI_OUTDUPL_FRAME_INFO fi;
            ComPtr<IDXGIResource> res;
            if (SUCCEEDED(dup->AcquireNextFrame(500, &fi, &res))) {
                ComPtr<ID3D11Texture2D> t;
                res.As(&t);
                D3D11_TEXTURE2D_DESC td;
                t->GetDesc(&td);
                printf("acquired texture %ux%u format %d\n", td.Width, td.Height, int(td.Format));
                dup->ReleaseFrame();
            }
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    double sec = 5;
    const wchar_t* bmp = L"vddtest.bmp";
    int slot = 0;
    bool primary = false, cursor = true, probeRot = false, verify = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--sec" && i + 1 < argc) sec = _wtof(argv[++i]);
        else if (a == L"--bmp" && i + 1 < argc) bmp = argv[++i];
        else if (a == L"--slot" && i + 1 < argc) slot = _wtoi(argv[++i]);
        else if (a == L"--primary") primary = true;
        else if (a == L"--no-cursor") cursor = false;
        else if (a == L"--verify") verify = true;
        else if (a == L"--paint-ms" && i + 1 < argc) g_paintMs = _wtoi(argv[++i]);
        else if (a == L"--rotated-probe") probeRot = true;
        else if (a == L"--list") {
            listAll();
            return 0;
        } else {
            fprintf(stderr, "usage: msdisp-vddtest [--primary] [--sec N] [--bmp f.bmp] [--slot S] [--no-cursor] "
                            "[--verify] [--list] [--rotated-probe]\n");
            return 2;
        }
    }
    g_cleanedUp = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(onCtrl, TRUE);

    if (primary) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        return captureLoop(mi.szDevice, sec, bmp, {}, false, false, mi.rcMonitor, verify);
    }

    int rc = 1;
    {
        // The custom mode only takes effect if written before the display is added (driver reads it on plug).
        if (!parsecVddSetCustomMode(slot, 1080, 1920, 60))
            MSD_WARN("could not write custom mode slot %d (not elevated?) - relying on the existing registry value",
                     slot);
        VirtualDisplayGuard g;
        if (!g.vdd->open()) return 1;
        g.index = g.vdd->add();
        if (g.index < 0) return 1;
        std::wstring name;
        for (int i = 0; i < 50 && !g_stop && (name = g.vdd->gdiName(g.index)).empty(); ++i) Sleep(100);
        if (name.empty()) {
            MSD_ERROR("virtual display did not attach to the desktop");
        } else if (probeRot) {
            rotatedProbe(name);
            rc = 0;
        } else {
            MSD_INFO("virtual display %d = %ls", g.index, name.c_str());
            if (!setDisplayMode(name, 1080, 1920, 60))
                MSD_WARN("native 1080x1920 not available, keeping the current mode");
            LONG right = 0;
            for (auto& m : g.layout) right = std::max(right, LONG(m.dm.dmPosition.x + m.dm.dmPelsWidth));
            if (!placeAt(name, int(right), 0)) MSD_WARN("could not position %ls", name.c_str());
            Sleep(300);
            RECT dr{};
            desktopRect(name, dr);
            MSD_INFO("%ls at %ld,%ld size %ldx%ld", name.c_str(), dr.left, dr.top, dr.right - dr.left,
                     dr.bottom - dr.top);
            Animator anim;
            anim.start(dr.left + 100, dr.top + 100);
            rc = captureLoop(name, sec, bmp, {100 + kProbeX, 100 + kProbeY}, true, cursor, dr, verify);
        }
    }
    SetEvent(g_cleanedUp);
    return rc;
}
