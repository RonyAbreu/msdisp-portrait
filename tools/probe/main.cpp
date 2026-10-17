// msdisp-probe: hardware spikes for the MS912x adapter.
//   msdisp-probe info                      HID only (safe while the vendor app runs)
//   msdisp-probe bars  [--720] [--sec N]   color bars + moving box (needs exclusive access)
//   msdisp-probe bench [--720] [--sec N]   full-frame throughput
//   msdisp-probe multi [--sec N]           two rects per bulk transfer test
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/usb/hid_control.h"
#include "msdisp/device.h"
#include "msdisp/log.h"

using namespace msdisp;

namespace {

void rgbToYuv(int r, int g, int b, int& y, int& u, int& v) {
    y = ((16 << 16) + 16763 * r + 32904 * g + 6391 * b) >> 16;
    u = ((128 << 16) - 9676 * r - 18996 * g + 28672 * b) >> 16;
    v = ((128 << 16) + 28672 * r - 24009 * g - 4663 * b) >> 16;
}

void fillRect(std::vector<uint8_t>& f, int pitch, Rect r, int R, int G, int B) {
    int y, u, v;
    rgbToYuv(R, G, B, y, u, v);
    for (int row = r.y; row < r.bottom(); ++row) {
        uint8_t* p = f.data() + size_t(row) * pitch + size_t(r.x & ~1) * 2;
        for (int x = 0; x < r.w; x += 2, p += 4) p[0] = uint8_t(u), p[1] = uint8_t(y), p[2] = uint8_t(v), p[3] = uint8_t(y);
    }
}

void drawBars(std::vector<uint8_t>& f, int w, int h, int pitch) {
    const int cols[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
                            {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0}};
    int bw = (w / 8) & ~1;
    for (int i = 0; i < 8; ++i) fillRect(f, pitch, {i * bw, 0, i == 7 ? w - 7 * bw : bw, h}, cols[i][0], cols[i][1], cols[i][2]);
}

int cmdInfo() {
    HidControl hid;
    if (!hid.open(0x534D, 0x6021, 0)) return 1;
    uint8_t chip[3], port = 0, hpd = 0;
    if (!hid.readXdata(0xF000, chip, 3) || !hid.readByte(0x31, port) || !hid.readByte(0x32, hpd)) return 1;
    const char* variant = (chip[1] == 0x16 && chip[2] == 0x0a)
                              ? (chip[0] == 0xb7 ? "MS912C" : chip[0] == 0xa7 ? "MS912A" : "MS9120")
                              : "unknown";
    printf("chip id: %02x %02x %02x (%s)\nvideo port: %d (5=HDMI)\nhotplug: %d\n", chip[0], chip[1], chip[2], variant,
           port, hpd);
    std::vector<uint8_t> edid(128);
    if (!hid.readXdata(0xC000, edid.data(), 128)) return 1;
    bool hdr = edid[0] == 0 && edid[1] == 0xff && edid[7] == 0;
    uint8_t sum = 0;
    for (uint8_t b : edid) sum += b;
    printf("edid: header=%s checksum=%s ext=%d\n", hdr ? "ok" : "BAD", sum == 0 ? "ok" : "BAD", edid[126]);
    for (int d = 54; d < 126; d += 18) {
        if (edid[d] == 0 && edid[d + 1] == 0 && edid[d + 3] == 0xfc) {
            char name[14] = {0};
            memcpy(name, &edid[d + 5], 13);
            for (char& c : name) if (c == '\n') c = 0;
            printf("monitor name: %s\n", name);
        } else if (edid[d] || edid[d + 1]) {
            int pclk = (edid[d] | edid[d + 1] << 8) * 10;
            int ha = edid[d + 2] | (edid[d + 4] & 0xf0) << 4, va = edid[d + 5] | (edid[d + 7] & 0xf0) << 4;
            printf("detailed timing: %dx%d pixclk=%d kHz\n", ha, va, pclk);
        }
    }
    return 0;
}

struct Opts {
    bool m720 = false;
    int sec = 5;
};

Mode pickMode(const Opts& o) {
    Mode m;
    if (o.m720) m.width = 1280, m.height = 720, m.hz = 60, m.vic = 0x4f;
    return m;
}

// Moving 32x32 square, 4 variants (color coded) of ZLP / double-buffer damage / pacing.
int cmdTrail(const Opts& o) {
    struct V { const char* name; const char* zlp; const char* dbl; const char* single; int paceMs; int r, g, b; };
    const V vs[] = {{"RED    single zlp=1 nbuf=1", "1", "1", "1", 0, 255, 0, 0},
                    {"GREEN  single zlp=1 nbuf=2", "1", "2", "1", 0, 0, 255, 0},
                    {"BLUE   single zlp=1 nbuf=3", "1", "3", "1", 0, 0, 80, 255},
                    {"YELLOW single zlp=1 nbuf=4", "1", "4", "1", 0, 255, 255, 0}};
    Mode m = pickMode(o);
    int pitch = m.width * 2;
    std::vector<uint8_t> frame(size_t(pitch) * m.height);
    for (const V& v : vs) {
        SetEnvironmentVariableA("MSDISP_ZLP", v.zlp);
        _putenv_s("MSDISP_ZLP", v.zlp);
        _putenv_s("MSDISP_NBUF", v.dbl);
        _putenv_s("MSDISP_SINGLE_RECT", v.single);
        auto dev = createMs912xDevice();
        if (!dev->open() || !dev->start(m)) return 1;
        fillRect(frame, pitch, {0, 0, m.width, m.height}, 60, 60, 60);
        FrameUpdate full{frame.data(), pitch, {{0, 0, m.width, m.height}}};
        dev->submit(full);
        dev->submit(full);
        dev->flush(2000);
        printf("variant %s\n", v.name), fflush(stdout);
        int x = 100, y = 100, dx = 6, dy = 4, n = 0;
        int64_t end = nowUs() + int64_t(o.sec) * 1000000;
        while (nowUs() < end) {
            Rect old{x, y, 32, 32};
            fillRect(frame, pitch, old, 60, 60, 60);
            x += dx, y += dy;
            if (x < 0 || x + 32 > m.width) dx = -dx, x += 2 * dx;
            if (y < 0 || y + 32 > m.height) dy = -dy, y += 2 * dy;
            Rect cur{x, y, 32, 32};
            fillRect(frame, pitch, cur, v.r, v.g, v.b);
            FrameUpdate up{frame.data(), pitch, {old, cur}};
            dev->submit(up);
            dev->flush(1000);
            if (v.paceMs) Sleep(DWORD(v.paceMs));
            ++n;
        }
        printf("  %d updates; holding 4 s\n", n), fflush(stdout);
        Sleep(4000);
        dev->stop();
        dev->close();
        Sleep(1500);
    }
    return 0;
}

int cmdRun(const std::string& cmd, const Opts& o) {
    auto dev = createMs912xDevice();
    if (!dev->open()) return 1;
    Mode m = pickMode(o);
    if (!dev->start(m)) return 1;
    int pitch = m.width * 2;
    std::vector<uint8_t> frame(size_t(pitch) * m.height);
    drawBars(frame, m.width, m.height, pitch);
    FrameUpdate full{frame.data(), pitch, {{0, 0, m.width, m.height}}};
    dev->submit(full);
    dev->flush(2000);
    printf("first frame: %.1f ms\n", dev->stats().lastTransferMs);

    int64_t t0 = nowUs(), end = t0 + int64_t(o.sec) * 1000000;
    int n = 0;
    if (cmd == "bench") {
        while (nowUs() < end) {
            if (dev->submit(full)) ++n;
            else dev->flush(50);
        }
        dev->flush(2000);
    } else {
        // moving box: each update sends old + new position (2 rects)
        int bs = m.height / 6, x = 0, y = m.height / 3, dx = 8;
        while (nowUs() < end) {
            Rect old{x, y, bs, bs};
            fillRect(frame, pitch, old, 40, 40, 40);
            x += dx;
            if (x < 0 || x + bs > m.width) dx = -dx, x += 2 * dx;
            Rect cur{x, y, bs, bs};
            fillRect(frame, pitch, cur, 255, 255, 255);
            FrameUpdate up{frame.data(), pitch, {}};
            if (cmd == "multi") up.rects = {old, cur};
            else up.rects = {{std::min(old.x, cur.x), y, bs + std::abs(dx), bs}};
            while (!dev->submit(up)) dev->flush(20);
            ++n;
        }
        dev->flush(2000);
    }
    double secs = double(nowUs() - t0) / 1e6;
    DeviceStats s = dev->stats();
    printf("%s: %d updates in %.2fs = %.1f/s, %.1f MB/s, dropped=%llu, last transfer %.1f ms\n", cmd.c_str(), n, secs,
           n / secs, double(s.bytesSent) / secs / 1e6, (unsigned long long)s.framesDropped, s.lastTransferMs);
    printf("leaving image on screen; press Enter to stop..."), fflush(stdout);
    if (o.sec >= 0) getchar();
    dev->stop();
    dev->close();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: msdisp-probe info|bars|bench|multi [--720] [--sec N]\n");
        return 2;
    }
    std::string cmd = argv[1];
    Opts o;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--720")) o.m720 = true;
        else if (!strcmp(argv[i], "--sec") && i + 1 < argc) o.sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--debug")) logSetLevel(LogLevel::Debug);
    }
    if (cmd == "info") return cmdInfo();
    if (cmd == "trail") return cmdTrail(o);
    if (cmd == "bars" || cmd == "bench" || cmd == "multi") return cmdRun(cmd, o);
    fprintf(stderr, "unknown command\n");
    return 2;
}
