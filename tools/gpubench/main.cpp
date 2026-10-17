// msdisp-gpubench: times IConverter::process()+done() (GPU convert/diff + readback) on the real GPU.
// Usage: msdisp-gpubench [frames] [--warp]
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "msdisp/gpu.h"

using namespace msdisp;

namespace {

constexpr int kSrcW = 1080, kSrcH = 1920, kOutW = 1920, kOutH = 1080;

ID3D11Texture2D* makeTexture(ID3D11Device* dev, const std::vector<uint8_t>& px) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = kSrcW;
    d.Height = kSrcH;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {px.data(), kSrcW * 4, 0};
    ID3D11Texture2D* t = nullptr;
    dev->CreateTexture2D(&d, &init, &t);
    return t;
}

std::vector<uint8_t> randomPixels(uint32_t seed) {
    std::vector<uint8_t> px(size_t(kSrcW) * kSrcH * 4);
    std::mt19937 rng(seed);
    for (size_t i = 0; i < px.size(); i += 4) {
        const uint32_t v = rng();
        memcpy(&px[i], &v, 4);
    }
    return px;
}

struct Stats {
    double minMs, avgMs, p95Ms, maxMs;
};

Stats stats(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    return {v.front(), sum / double(v.size()), v[std::min(v.size() - 1, size_t(double(v.size()) * 0.95))], v.back()};
}

// Alternates textures a/b each frame; returns per-frame process()+done() time in ms.
Stats run(IConverter& conv, ID3D11Texture2D* a, ID3D11Texture2D* b, bool forceFull, int frames, size_t* rects,
          int64_t* dirtyPx) {
    ConvertResult res;
    std::vector<double> t;
    for (int i = 0; i < frames + 20; ++i) {
        const int64_t t0 = nowUs();
        if (!conv.process((i & 1) ? b : a, Rotation::CW270, {}, forceFull, res)) {
            fprintf(stderr, "process failed\n");
            exit(1);
        }
        conv.done();
        const int64_t t1 = nowUs();
        if (i >= 20) t.push_back(double(t1 - t0) / 1000.0);  // first 20 frames are warm-up
        *rects = res.dirty.size();
        *dirtyPx = 0;
        for (const Rect& r : res.dirty) *dirtyPx += int64_t(r.w) * r.h;
    }
    return stats(t);
}

}  // namespace

int main(int argc, char** argv) {
    int frames = 300;
    bool warp = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--warp"))
            warp = true;
        else
            frames = std::max(10, atoi(argv[i]));
    }
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 2,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        fprintf(stderr, "D3D11CreateDevice failed\n");
        return 1;
    }
    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC ad = {};
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) &&
        SUCCEEDED(dxgiDev->GetAdapter(&adapter)))
        adapter->GetDesc(&ad);
    printf("adapter: %ls\n", ad.Description);
    printf("source %dx%d BGRA -> CW270 -> %dx%d UYVY, tile 64x8, %d frames (after 20 warm-up)\n", kSrcW, kSrcH, kOutW,
           kOutH, frames);

    auto conv = createD3D11Converter();
    if (!conv->init(dev, ctx, kOutW, kOutH)) return 1;

    std::vector<uint8_t> pa = randomPixels(1), pc = randomPixels(2);
    std::vector<uint8_t> pb = pa;
    for (int y = 500; y < 532; ++y)  // 32x32 "cursor" block
        for (int x = 300; x < 332; ++x) pb[(size_t(y) * kSrcW + x) * 4 + 1] ^= 0x80;
    ID3D11Texture2D* a = makeTexture(dev, pa);
    ID3D11Texture2D* b = makeTexture(dev, pb);
    ID3D11Texture2D* c = makeTexture(dev, pc);

    struct Case {
        const char* name;
        ID3D11Texture2D *x, *y;
        bool force;
    } cases[] = {
        {"no change", a, a, false},
        {"small change (32x32)", a, b, false},
        {"full change", a, c, false},
        {"forceFull", a, a, true},
    };
    printf("%-22s %8s %8s %8s %8s %6s %10s\n", "case", "min ms", "avg ms", "p95 ms", "max ms", "rects", "dirty px");
    for (const Case& k : cases) {
        size_t rects = 0;
        int64_t px = 0;
        const Stats s = run(*conv, k.x, k.y, k.force, frames, &rects, &px);
        printf("%-22s %8.3f %8.3f %8.3f %8.3f %6zu %10lld\n", k.name, s.minMs, s.avgMs, s.p95Ms, s.maxMs, rects,
               static_cast<long long>(px));
    }
    // How fast the consumer can read the mapped frame (staging memory may be uncached on some drivers).
    {
        ConvertResult res;
        std::vector<uint8_t> copy(size_t(kOutW) * 2 * kOutH);
        conv->process(a, Rotation::CW270, {}, true, res);
        double best = 1e9;
        volatile uint8_t sink = 0;
        for (int i = 0; i < 20; ++i) {
            const int64_t t0 = nowUs();
            memcpy(copy.data(), res.uyvy, copy.size());
            sink = sink + copy[size_t(i) * 4099];
            best = std::min(best, double(nowUs() - t0) / 1000.0);
        }
        conv->done();
        printf("CPU memcpy of mapped frame (%zu bytes): %.3f ms (%.1f GB/s)\n", copy.size(), best,
               double(copy.size()) / (best * 1e6));
    }
    conv.reset();
    a->Release();
    b->Release();
    c->Release();
    if (adapter) adapter->Release();
    if (dxgiDev) dxgiDev->Release();
    ctx->Release();
    dev->Release();
    return 0;
}
