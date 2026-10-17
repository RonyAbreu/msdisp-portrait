// Track C tests: GPU converter vs CPU reference (bit-exact, all rotations) and tile-diff reporting.
#include <windows.h>
#include <d3d11.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "msdisp/gpu.h"

using namespace msdisp;

namespace {

int g_failures = 0;

#define EXPECT(cond, ...)                                       \
    do {                                                        \
        if (!(cond)) {                                          \
            ++g_failures;                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                       \
            fprintf(stderr, "\n");                              \
        }                                                       \
    } while (0)

struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ~Gpu() {
        if (ctx) ctx->Release();
        if (dev) dev->Release();
    }
    bool create() {
        const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        for (D3D_DRIVER_TYPE t : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
            if (SUCCEEDED(D3D11CreateDevice(nullptr, t, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx)) &&
                dev->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0) {
                printf("device: %s\n", t == D3D_DRIVER_TYPE_HARDWARE ? "hardware" : "WARP");
                return true;
            }
        }
        return false;
    }
};

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // BGRA, pitch w*4
    uint8_t* at(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
};

Image randomImage(int w, int h, uint32_t seed) {
    Image im{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
    std::mt19937 rng(seed);
    for (auto& b : im.px) b = uint8_t(rng());
    // Include the extremes so clamping/rounding edge cases are covered.
    const uint8_t edge[][4] = {{0, 0, 0, 255}, {255, 255, 255, 255}, {255, 0, 0, 0}, {0, 255, 0, 0}, {0, 0, 255, 0}};
    for (int i = 0; i < 5; ++i) memcpy(im.at(i, 0), edge[i], 4);
    return im;
}

ID3D11Texture2D* makeTexture(ID3D11Device* dev, const Image& im, UINT bind = D3D11_BIND_SHADER_RESOURCE) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = UINT(im.w);
    d.Height = UINT(im.h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init = {im.px.data(), UINT(im.w * 4), 0};
    ID3D11Texture2D* t = nullptr;
    dev->CreateTexture2D(&d, &init, &t);
    return t;
}

void upload(ID3D11DeviceContext* ctx, ID3D11Texture2D* t, const Image& im) {
    ctx->UpdateSubresource(t, 0, nullptr, im.px.data(), UINT(im.w * 4), 0);
}

bool swaps(Rotation r) { return r == Rotation::CW90 || r == Rotation::CW270; }

// Source pixel -> output pixel (inverse of the spec's output->source mapping).
std::pair<int, int> srcToOut(int sx, int sy, int srcW, int srcH, Rotation r) {
    switch (r) {
        case Rotation::CW90: return {srcH - 1 - sy, sx};
        case Rotation::CW180: return {srcW - 1 - sx, srcH - 1 - sy};
        case Rotation::CW270: return {sy, srcW - 1 - sx};
        default: return {sx, sy};
    }
}

using TileSet = std::set<std::pair<int, int>>;

// Validates rect invariants and returns the covered tiles.
TileSet tilesOf(const std::vector<Rect>& rects, int outW, int outH, int tw, int th, const char* what) {
    TileSet s;
    for (const Rect& r : rects) {
        EXPECT(!r.empty() && r.x >= 0 && r.y >= 0 && r.right() <= outW && r.bottom() <= outH, "%s: rect out of frame",
               what);
        EXPECT(r.x % 2 == 0 && r.w % 2 == 0, "%s: odd rect x/w %d,%d", what, r.x, r.w);
        EXPECT(r.x % tw == 0 && r.y % th == 0, "%s: rect not tile aligned", what);
        EXPECT(r.right() == outW || r.w % tw == 0, "%s: rect width not tile multiple", what);
        EXPECT(r.bottom() == outH || r.h % th == 0, "%s: rect height not tile multiple", what);
        for (int y = r.y; y < r.bottom(); y += th)
            for (int x = r.x; x < r.right(); x += tw) {
                EXPECT(s.insert({x / tw, y / th}).second, "%s: overlapping rects", what);
            }
    }
    return s;
}

// Tiles whose UYVY bytes differ between two CPU reference frames.
TileSet diffTiles(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int outW, int outH, int tw, int th) {
    TileSet s;
    for (int y = 0; y < outH; ++y)
        for (int x = 0; x < outW; x += 2) {
            const size_t o = size_t(y) * outW * 2 + size_t(x) * 2;
            if (memcmp(&a[o], &b[o], 4) != 0) s.insert({x / tw, y / th});
        }
    return s;
}

std::vector<uint8_t> cpuRef(const Image& im, Rotation r) {
    const int ow = swaps(r) ? im.h : im.w, oh = swaps(r) ? im.w : im.h;
    std::vector<uint8_t> out(size_t(ow) * oh * 2);
    cpuBgraToUyvyRotated(im.px.data(), im.w, im.h, im.w * 4, r, out.data(), ow * 2);
    return out;
}

bool sameFrame(const ConvertResult& res, const std::vector<uint8_t>& ref, int outW, int outH, const char* what) {
    for (int y = 0; y < outH; ++y) {
        const uint8_t* g = res.uyvy + size_t(y) * res.pitch;
        const uint8_t* c = &ref[size_t(y) * outW * 2];
        if (memcmp(g, c, size_t(outW) * 2) != 0) {
            int x = 0;
            while (g[x] == c[x]) ++x;
            EXPECT(false, "%s: mismatch at row %d byte %d: gpu %d cpu %d", what, y, x, g[x], c[x]);
            return false;
        }
    }
    return true;
}

void testCpuReferenceKnownValues() {
    // White, black, pure red/green/blue pairs against values computed by hand from the formulas.
    const uint8_t px[] = {255, 255, 255, 0, 255, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 255, 0, 0};
    uint8_t out[12];
    cpuBgraToUyvyRotated(px, 6, 1, 24, Rotation::None, out, 12);
    // white: y=(16<<16 + 255*56058)>>16 = 234, u=v=128 (chroma coefficients sum to 0). black: 16/128/128.
    const uint8_t want01[] = {128, 234, 128, 234, 128, 16, 128, 16};
    EXPECT(memcmp(out, want01, 8) == 0, "white/black: got %d %d %d %d / %d %d %d %d", out[0], out[1], out[2], out[3],
           out[4], out[5], out[6], out[7]);
    // red y=(16<<16 + 16763*255)>>16 = 81, u0=(128<<16 - 9676*255)>>16=90, v0=(128<<16+28672*255)>>16=239;
    // green y=144 u=54 v=34 -> averages u=(90+54)/2=72 v=(239+34)/2=136.
    EXPECT(out[8] == 72 && out[9] == 81 && out[10] == 136 && out[11] == 144, "red/green: got %d %d %d %d", out[8],
           out[9], out[10], out[11]);
}

void testBitExact(IConverter& conv, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    const int srcW = 1080, srcH = 1920;  // portrait source; None/CW180 use the landscape image below
    Image portrait = randomImage(srcW, srcH, 1234);
    Image landscape = randomImage(1920, 1080, 99);
    ID3D11Texture2D* tp = makeTexture(dev, portrait);
    ID3D11Texture2D* tl = makeTexture(dev, landscape);
    EXPECT(tp && tl, "texture creation");
    for (Rotation r : {Rotation::None, Rotation::CW90, Rotation::CW180, Rotation::CW270}) {
        const Image& im = swaps(r) ? portrait : landscape;
        ConvertResult res;
        const bool ok = conv.process(swaps(r) ? tp : tl, r, {}, true, res);
        EXPECT(ok, "process rot %d", int(r));
        if (ok) {
            EXPECT(res.pitch == 1920 * 2, "pitch %d", res.pitch);
            if (sameFrame(res, cpuRef(im, r), 1920, 1080, "bit-exact")) printf("bit-exact rot %d: ok\n", int(r));
            EXPECT(res.dirty.size() == 1 && res.dirty[0].x == 0 && res.dirty[0].y == 0 && res.dirty[0].w == 1920 &&
                       res.dirty[0].h == 1080,
                   "forceFull must report one full rect");
        }
        conv.done();
    }
    tp->Release();
    tl->Release();
}

void testDirty(IConverter& conv, ID3D11Device* dev, ID3D11DeviceContext* ctx, Rotation r, int tw, int th) {
    const int srcW = swaps(r) ? 1080 : 1920, srcH = swaps(r) ? 1920 : 1080;
    const int outW = 1920, outH = 1080;
    char what[64];
    snprintf(what, sizeof(what), "dirty rot %d tile %dx%d", int(r), tw, th);
    EXPECT(conv.init(dev, ctx, outW, outH, tw, th), "%s: init", what);
    Image im = randomImage(srcW, srcH, 7 + int(r));
    ID3D11Texture2D* t = makeTexture(dev, im);
    ConvertResult res;

    EXPECT(conv.process(t, r, {}, true, res), "%s: first", what);
    conv.done();
    EXPECT(conv.process(t, r, {}, false, res) && res.dirty.empty(), "%s: unchanged frame has %zu rects", what,
           res.dirty.size());
    conv.done();

    // 10x10 blocks at awkward positions (odd offsets, straddling tile edges, frame corners).
    const int blocks[][2] = {{3, 5}, {61, 7}, {srcW - 10, srcH - 10}, {0, 0}, {517, 1001}};
    for (const auto& bp : blocks) {
        std::vector<uint8_t> before = cpuRef(im, r);
        TileSet geo;
        for (int y = bp[1]; y < bp[1] + 10; ++y)
            for (int x = bp[0]; x < bp[0] + 10; ++x) {
                uint8_t* p = im.at(x, y);
                for (int k = 0; k < 3; ++k) p[k] ^= 0x80;  // always changes Y (no coefficient subset cancels)
                auto [ox, oy] = srcToOut(x, y, srcW, srcH, r);
                geo.insert({ox / tw, oy / th});
            }
        std::vector<uint8_t> after = cpuRef(im, r);
        upload(ctx, t, im);
        const bool ok = conv.process(t, r, {}, false, res);
        EXPECT(ok, "%s: process", what);
        if (ok) {
            TileSet got = tilesOf(res.dirty, outW, outH, tw, th, what);
            EXPECT(got == geo, "%s: block (%d,%d): got %zu tiles in %zu rects, want %zu", what, bp[0], bp[1],
                   got.size(), res.dirty.size(), geo.size());
            EXPECT(diffTiles(before, after, outW, outH, tw, th) == geo, "%s: cpu diff disagrees with geometry", what);
            sameFrame(res, after, outW, outH, what);
        }
        conv.done();
        EXPECT(conv.process(t, r, {}, false, res) && res.dirty.empty(), "%s: repeat frame not clean", what);
        conv.done();
    }

    // Large change: everything changes -> rects must cover the whole frame, merged into few rects.
    Image other = randomImage(srcW, srcH, 555);
    upload(ctx, t, other);
    if (conv.process(t, r, {}, false, res)) {
        TileSet got = tilesOf(res.dirty, outW, outH, tw, th, what);
        const size_t all = size_t((outW + tw - 1) / tw) * ((outH + th - 1) / th);
        EXPECT(got.size() == all, "%s: full change covers %zu of %zu tiles", what, got.size(), all);
        EXPECT(res.dirty.size() == 1, "%s: full change merged into %zu rects", what, res.dirty.size());
        sameFrame(res, cpuRef(other, r), outW, outH, what);
    }
    conv.done();
    // forceFull on unchanged content still reports the full frame.
    EXPECT(conv.process(t, r, {}, true, res) && res.dirty.size() == 1 && res.dirty[0].w == outW &&
               res.dirty[0].h == outH,
           "%s: forceFull", what);
    conv.done();
    t->Release();
    printf("%s: done\n", what);
}

void testMergeShape(IConverter& conv, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    // Two separated blocks on the same tile rows + an L shape: checks run splitting and vertical merging.
    EXPECT(conv.init(dev, ctx, 1920, 1080, 64, 8), "merge: init");
    Image im = randomImage(1920, 1080, 42);
    ID3D11Texture2D* t = makeTexture(dev, im);
    ConvertResult res;
    conv.process(t, Rotation::None, {}, true, res);
    conv.done();
    auto touch = [&](int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) im.at(x, y)[1] ^= 0x80;
    };
    touch(0, 0, 128, 24);      // tiles x0-1, rows 0-2 -> one 128x24 rect
    touch(640, 0, 704, 16);    // tile x10, rows 0-1 -> one 64x16 rect
    touch(1856, 1072, 1920, 1080);  // last tile (clipped bottom row is exactly 8 high here)
    upload(ctx, t, im);
    EXPECT(conv.process(t, Rotation::None, {}, false, res), "merge: process");
    std::set<std::tuple<int, int, int, int>> got, want = {{0, 0, 128, 24}, {640, 0, 64, 16}, {1856, 1072, 64, 8}};
    for (const Rect& r : res.dirty) got.insert({r.x, r.y, r.w, r.h});
    EXPECT(got == want, "merge: got %zu rects", res.dirty.size());
    for (const Rect& r : res.dirty) printf("  rect %d,%d %dx%d\n", r.x, r.y, r.w, r.h);
    conv.done();
    t->Release();
}

void testEdgeCases(IConverter& conv, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    // Size mismatch -> false (scaling unsupported).
    EXPECT(conv.init(dev, ctx, 1920, 1080), "edge: init");
    Image wrong = randomImage(1280, 720, 1);
    ID3D11Texture2D* t = makeTexture(dev, wrong);
    ConvertResult res;
    EXPECT(!conv.process(t, Rotation::None, {}, true, res), "edge: size mismatch must fail");
    t->Release();
    Image portrait = randomImage(1080, 1920, 2);
    t = makeTexture(dev, portrait);
    EXPECT(!conv.process(t, Rotation::None, {}, true, res), "edge: unrotated portrait must fail");
    t->Release();

    // Texture without SHADER_RESOURCE bind (like some duplication textures) goes through the internal copy,
    // and a frame size that is not a tile multiple (100x30 with 16x4 tiles) exercises clipping.
    EXPECT(conv.init(dev, ctx, 100, 30, 16, 4), "edge: init small");
    Image small = randomImage(30, 100, 3);
    t = makeTexture(dev, small, D3D11_BIND_RENDER_TARGET);
    EXPECT(t != nullptr, "edge: rt texture");
    if (t && conv.process(t, Rotation::CW90, {}, true, res)) {
        sameFrame(res, cpuRef(small, Rotation::CW90), 100, 30, "edge: copy path");
        conv.done();
        small.at(29, 99)[0] ^= 0x80;  // -> output (0, 29): tile (0, 7), clipped to 16x2
        upload(ctx, t, small);
        EXPECT(conv.process(t, Rotation::CW90, {}, false, res), "edge: process 2");
        EXPECT(res.dirty.size() == 1 && res.dirty[0].x == 0 && res.dirty[0].y == 28 && res.dirty[0].w == 16 &&
                   res.dirty[0].h == 2,
               "edge: clipped rect");
        sameFrame(res, cpuRef(small, Rotation::CW90), 100, 30, "edge: copy path 2");
        small.at(0, 0)[0] ^= 0x80;  // -> output (99, 0): tile (6, 0), clipped to 4 wide
        upload(ctx, t, small);
        EXPECT(conv.process(t, Rotation::CW90, {}, false, res), "edge: process 3");
        EXPECT(res.dirty.size() == 1 && res.dirty[0].x == 96 && res.dirty[0].y == 0 && res.dirty[0].w == 4 &&
                   res.dirty[0].h == 4,
               "edge: clipped rect right");
        conv.done();
    }
    if (t) t->Release();
}

}  // namespace

int main() {
    Gpu gpu;
    if (!gpu.create()) {
        fprintf(stderr, "FAIL: no D3D11 device\n");
        return 1;
    }
    testCpuReferenceKnownValues();
    auto conv = createD3D11Converter();
    if (!conv || !conv->init(gpu.dev, gpu.ctx, 1920, 1080)) {
        fprintf(stderr, "FAIL: converter init\n");
        return 1;
    }
    testBitExact(*conv, gpu.dev, gpu.ctx);
    for (Rotation r : {Rotation::None, Rotation::CW90, Rotation::CW180, Rotation::CW270}) {
        testDirty(*conv, gpu.dev, gpu.ctx, r, 64, 8);
    }
    testDirty(*conv, gpu.dev, gpu.ctx, Rotation::CW270, 32, 16);
    testMergeShape(*conv, gpu.dev, gpu.ctx);
    testEdgeCases(*conv, gpu.dev, gpu.ctx);
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("all gpu tests passed\n");
    return 0;
}
