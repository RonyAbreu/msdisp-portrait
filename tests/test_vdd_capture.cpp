// Track B tests that run unattended: dirty/move rect folding on synthetic input, Parsec VDD argument checks, and a
// 1 s DXGI capture of the primary display. No virtual display is added here (see tools/vddtest for that).
#include <windows.h>

#include <cstdio>

#include "../src/capture/capture_rects.h"
#include "msdisp/capture.h"
#include "msdisp/log.h"
#include "msdisp/vdd.h"

using namespace msdisp;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

bool same(const Rect& a, const Rect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

void testRects() {
    using namespace msdisp::capture;
    CHECK(same(clipRect({-10, -10, 30, 30}, 100, 100), Rect{0, 0, 20, 20}));
    CHECK(same(clipRect({90, 95, 30, 30}, 100, 100), Rect{90, 95, 10, 5}));
    CHECK(clipRect({100, 0, 10, 10}, 100, 100).empty());
    CHECK(clipRect({5, 5, 0, 10}, 100, 100).empty());

    std::vector<Rect> d;
    addDirty(d, {10, 10, 5, 5}, 100, 100);
    addDirty(d, {10, 10, 5, 5}, 100, 100);   // duplicate dropped
    addDirty(d, {200, 10, 5, 5}, 100, 100);  // off-screen dropped
    CHECK(d.size() == 1);

    // A window dragged right by 30 px: content of (0,0,40,20) moved to (30,0,40,20).
    std::vector<Rect> f;
    foldMoveRects({{0, 0, {30, 0, 40, 20}}}, f, 100, 100);
    CHECK(f.size() == 2);
    CHECK(f.size() == 2 && same(f[0], Rect{30, 0, 40, 20}));
    CHECK(f.size() == 2 && same(f[1], Rect{0, 0, 40, 20}));

    // Scroll near the bottom edge: destination clipped, source kept.
    f.clear();
    foldMoveRects({{0, 50, {0, 80, 100, 50}}}, f, 100, 100);
    CHECK(f.size() == 2 && same(f[0], Rect{0, 80, 100, 20}) && same(f[1], Rect{0, 50, 100, 50}));

    // Degenerate move (src == dst) folds into a single rect.
    f.clear();
    foldMoveRects({{5, 5, {5, 5, 10, 10}}}, f, 100, 100);
    CHECK(f.size() == 1);
}

void testVddArgs() {
    CHECK(!parsecVddSetCustomMode(-1, 1080, 1920, 60));
    CHECK(!parsecVddSetCustomMode(0, 0, 1920, 60));
    CHECK(!setDisplayMode(L"\\\\.\\DISPLAY_DOES_NOT_EXIST", 1080, 1920, 60));
    auto v = createParsecVdd();
    CHECK(v != nullptr);
    if (v && v->open()) {  // driver installed: opening and closing (VERSION ioctl only) is harmless
        CHECK(v->gdiName(0).empty());  // nothing added by us
        CHECK(!v->remove(0));          // never removes displays it did not add
        v->close();
    }
}

void testPrimaryCapture() {
    if (GetSystemMetrics(SM_REMOTESESSION)) {
        printf("SKIP primary capture (remote session)\n");
        return;
    }
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
    const bool rotated = dm.dmDisplayOrientation != DMDO_DEFAULT;

    auto cap = createDxgiCapture();
    CHECK(cap != nullptr);
    if (!cap) return;
    CHECK(!cap->init(L"\\\\.\\NO_SUCH_DISPLAY"));
    if (!cap->init(mi.szDevice)) {
        CHECK(rotated);  // only rotated outputs are refused
        return;
    }
    CHECK(cap->device() != nullptr && cap->context() != nullptr);
    int frames = 0, bad = 0;
    const int64_t t0 = nowUs();
    while (nowUs() - t0 < 1000000) {
        CapturedFrame f;
        AcquireResult r = cap->acquire(f, 100);
        if (r == AcquireResult::Lost) {
            CHECK(cap->init(mi.szDevice));
            continue;
        }
        if (r != AcquireResult::Frame) continue;
        if (frames == 0) CHECK(f.full);
        ++frames;
        bad += f.texture == nullptr || f.width != int(dm.dmPelsWidth) || f.height != int(dm.dmPelsHeight);
        bad += f.timeUs <= 0 || f.dirty.empty();
        for (const Rect& d : f.dirty)
            bad += d.empty() || d.x < 0 || d.y < 0 || d.right() > f.width || d.bottom() > f.height;
        cap->release();
    }
    printf("primary %ls: %d frames in 1 s\n", mi.szDevice, frames);
    CHECK(frames >= 1);
    CHECK(bad == 0);
    cap->shutdown();
}

}  // namespace

int main() {
    logSetLevel(LogLevel::Warn);
    testRects();
    testVddArgs();
    testPrimaryCapture();
    if (g_failures) fprintf(stderr, "%d check(s) failed\n", g_failures);
    else printf("OK\n");
    return g_failures ? 1 : 0;
}
