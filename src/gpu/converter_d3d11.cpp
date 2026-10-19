// GPU converter: rotate + BGRA->UYVY (BT.601 limited, rhgndf/ms912x integer math) + tile diff in one D3D11
// compute dispatch, then readback of only the dirty tile rows.
//
// Buffer layout (all GPU buffers are raw / ByteAddress):
//   packed  : DEFAULT, (outW/2) x outH uint32, row-major, one UYVY pair per uint32 (bytes U Y0 V Y1 in memory),
//             so a row is exactly outW*2 bytes and a tile row (tileH pixel rows) is one contiguous byte range.
//             It holds the last converted frame; the shader compares in place (each thread owns one element)
//             and only writes + flags when the value changed, so no separate "previous" buffer is needed.
//   flags   : DEFAULT, tilesX*tilesY uint32, cleared every frame, set to 1 for tiles that changed.
//   staging : STAGING copies of both. The packed staging buffer is persistent: each frame only the dirty tile
//             rows are copied into it, so it always mirrors the full current frame.
#include "msdisp/gpu.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

#include "msdisp/log.h"

namespace msdisp {

namespace {

template <class T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    T** put() {
        reset();
        return &p_;
    }
    void assign(T* p) {
        if (p) p->AddRef();
        reset();
        p_ = p;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

constexpr int kGroupX = 32;  // pairs (= 64 output pixels)
constexpr int kGroupY = 8;

const char kShader[] = R"hlsl(
Texture2D<float4> src : register(t0);
SamplerState lin : register(s0);
RWByteAddressBuffer packed : register(u0);
RWByteAddressBuffer flags : register(u1);

cbuffer Params : register(b0) {
    uint outPairs;   // outW / 2
    uint outH;
    uint srcW;
    uint srcH;
    uint rot;        // 0 = none, 1 = CW90, 2 = CW180, 3 = CW270
    uint tilePairs;  // tileW / 2
    uint tileH;
    uint tilesX;
    uint scaled;     // 1 when the rotated source size differs from the output
    float scaleX;    // rotated-source pixels per output pixel
    float scaleY;
    uint pad;
};

// Continuous position in rotated space -> normalized source UV (inverse of the integer mapping below).
float2 srcUv(float rx, float ry) {
    float2 p;
    if (rot == 0) p = float2(rx, ry);
    else if (rot == 1) p = float2(ry, float(srcH) - rx);
    else if (rot == 2) p = float2(float(srcW) - rx, float(srcH) - ry);
    else p = float2(float(srcW) - ry, rx);
    return p / float2(srcW, srcH);
}

int3 rgbAt(uint ox, uint oy) {
    if (scaled != 0) {
        // Downscale: 2x2 supersampling of bilinear taps covers the source footprint (box-like filter, sharp text).
        float cx = (float(ox) + 0.5f) * scaleX, cy = (float(oy) + 0.5f) * scaleY;
        float dx = 0.25f * scaleX, dy = 0.25f * scaleY;
        float3 c = src.SampleLevel(lin, srcUv(cx - dx, cy - dy), 0).rgb + src.SampleLevel(lin, srcUv(cx + dx, cy - dy), 0).rgb +
                   src.SampleLevel(lin, srcUv(cx - dx, cy + dy), 0).rgb + src.SampleLevel(lin, srcUv(cx + dx, cy + dy), 0).rgb;
        return int3(saturate(c * 0.25f) * 255.0f + 0.5f);
    }
    uint sx, sy;
    if (rot == 0) { sx = ox; sy = oy; }
    else if (rot == 1) { sx = oy; sy = srcH - 1 - ox; }
    else if (rot == 2) { sx = srcW - 1 - ox; sy = srcH - 1 - oy; }
    else { sx = srcW - 1 - oy; sy = ox; }
    float4 c = src.Load(int3(sx, sy, 0));  // UNORM view of BGRA8: .rgb is red, green, blue
    return int3(c.rgb * 255.0f + 0.5f);
}

[numthreads(32, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outPairs || id.y >= outH) return;
    int3 a = rgbAt(id.x * 2, id.y);
    int3 b = rgbAt(id.x * 2 + 1, id.y);
    int y0 = ((16 << 16) + 16763 * a.r + 32904 * a.g + 6391 * a.b) >> 16;
    int y1 = ((16 << 16) + 16763 * b.r + 32904 * b.g + 6391 * b.b) >> 16;
    int u0 = ((128 << 16) - 9676 * a.r - 18996 * a.g + 28672 * a.b) >> 16;
    int u1 = ((128 << 16) - 9676 * b.r - 18996 * b.g + 28672 * b.b) >> 16;
    int v0 = ((128 << 16) + 28672 * a.r - 24009 * a.g - 4663 * a.b) >> 16;
    int v1 = ((128 << 16) + 28672 * b.r - 24009 * b.g - 4663 * b.b) >> 16;
    uint u = uint(u0 + u1) >> 1;
    uint v = uint(v0 + v1) >> 1;
    uint value = u | (uint(y0) << 8) | (v << 16) | (uint(y1) << 24);
    uint addr = (id.y * outPairs + id.x) * 4;
    if (packed.Load(addr) != value) {
        packed.Store(addr, value);
        flags.Store(((id.y / tileH) * tilesX + id.x / tilePairs) * 4, 1u);
    }
}
)hlsl";

struct Params {
    uint32_t outPairs, outH, srcW, srcH, rot, tilePairs, tileH, tilesX;
    uint32_t scaled;
    float scaleX, scaleY;
    uint32_t pad;
};

int rotIndex(Rotation r) {
    switch (r) {
        case Rotation::CW90: return 1;
        case Rotation::CW180: return 2;
        case Rotation::CW270: return 3;
        default: return 0;
    }
}

bool swapsAxes(Rotation r) { return r == Rotation::CW90 || r == Rotation::CW270; }

inline uint8_t clampByte(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

class D3D11Converter final : public IConverter {
public:
    ~D3D11Converter() override { done(); }

    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, int outW, int outH, int tileW, int tileH) override {
        done();
        if (!dev || !ctx || outW <= 0 || outH <= 0 || (outW & 1) || tileW < 2 || (tileW & 1) || tileH < 1) {
            MSD_ERROR("gpu: bad init args out=%dx%d tile=%dx%d", outW, outH, tileW, tileH);
            return false;
        }
        if (dev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
            MSD_ERROR("gpu: feature level 0x%x < 11_0, compute shader cs_5_0 unavailable",
                      unsigned(dev->GetFeatureLevel()));
            return false;
        }
        dev_.assign(dev);
        ctx_.assign(ctx);
        outW_ = outW;
        outH_ = outH;
        tileW_ = tileW;
        tileH_ = tileH;
        tilesX_ = (outW + tileW - 1) / tileW;
        tilesY_ = (outH + tileH - 1) / tileH;
        stagingValid_ = false;
        lastSrc_ = nullptr;
        srv_.reset();
        copyTex_.reset();
        memset(&lastParams_, 0xff, sizeof(lastParams_));

        Com<ID3DBlob> code, errors;
        HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "msdisp_uyvy", nullptr, nullptr, "main", "cs_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
        if (FAILED(hr)) {
            MSD_ERROR("gpu: D3DCompile failed 0x%08lx: %s", hr,
                      errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
            return false;
        }
        if (!check(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, cs_.put()),
                   "CreateComputeShader"))
            return false;

        const UINT frameBytes = UINT(outW) * 2 * UINT(outH);
        const UINT flagBytes = UINT(tilesX_ * tilesY_) * 4;
        if (!makeRawBuffer(frameBytes, packed_, packedUav_) || !makeRawBuffer(flagBytes, flags_, flagsUav_))
            return false;
        if (!makeStaging(frameBytes, packedStaging_) || !makeStaging(flagBytes, flagsStaging_)) return false;

        D3D11_BUFFER_DESC cb = {};
        cb.ByteWidth = sizeof(Params);
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (!check(dev->CreateBuffer(&cb, nullptr, cbuf_.put()), "CreateBuffer(cb)")) return false;
        D3D11_SAMPLER_DESC sdsc = {};
        sdsc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sdsc.AddressU = sdsc.AddressV = sdsc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sdsc.MaxLOD = D3D11_FLOAT32_MAX;
        if (!check(dev->CreateSamplerState(&sdsc, sampler_.put()), "CreateSamplerState")) return false;

        // Start from an all-zero "previous" frame.
        const UINT zero[4] = {};
        ctx->ClearUnorderedAccessViewUint(packedUav_.get(), zero);
        tileFlags_.assign(size_t(tilesX_) * tilesY_, 0);
        MSD_INFO("gpu: converter ready out=%dx%d tile=%dx%d (%dx%d tiles)", outW, outH, tileW, tileH, tilesX_,
                 tilesY_);
        return true;
    }

    bool process(ID3D11Texture2D* src, Rotation rot, const std::vector<Rect>& srcDirty, bool forceFull,
                 ConvertResult& out) override {
        done();
        out.uyvy = nullptr;
        out.pitch = 0;
        out.dirty.clear();
        if (!cs_ || !src) return false;

        D3D11_TEXTURE2D_DESC sd;
        src->GetDesc(&sd);
        const int rw = swapsAxes(rot) ? int(sd.Height) : int(sd.Width);
        const int rh = swapsAxes(rot) ? int(sd.Width) : int(sd.Height);
        const bool scaled = rw != outW_ || rh != outH_;
        if (scaled && (rw < outW_ || rh < outH_)) {
            MSD_ERROR("gpu: source %ux%u rotated %d is %dx%d, smaller than output %dx%d (upscaling unsupported)",
                      sd.Width, sd.Height, int(rot), rw, rh, outW_, outH_);
            return false;
        }
        if (sd.Format != DXGI_FORMAT_B8G8R8A8_UNORM && sd.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS) {
            MSD_ERROR("gpu: unsupported source format %d (want B8G8R8A8_UNORM)", int(sd.Format));
            return false;
        }
        if (!bindSource(src, sd)) return false;

        Params p = {uint32_t(outW_ / 2), uint32_t(outH_), sd.Width, sd.Height, uint32_t(rotIndex(rot)),
                    uint32_t(tileW_ / 2), uint32_t(tileH_), uint32_t(tilesX_), scaled ? 1u : 0u,
                    float(rw) / float(outW_), float(rh) / float(outH_), 0};
        if (memcmp(&p, &lastParams_, sizeof(p)) != 0) {
            ctx_->UpdateSubresource(cbuf_.get(), 0, nullptr, &p, 0, 0);
            lastParams_ = p;
        }

        ID3D11DeviceContext* c = ctx_.get();
        const UINT zero[4] = {};
        c->ClearUnorderedAccessViewUint(flagsUav_.get(), zero);
        ID3D11UnorderedAccessView* uavs[2] = {packedUav_.get(), flagsUav_.get()};
        ID3D11ShaderResourceView* srv = srv_.get();
        ID3D11Buffer* cb = cbuf_.get();
        ID3D11SamplerState* smp = sampler_.get();
        c->CSSetSamplers(0, 1, &smp);
        c->CSSetShader(cs_.get(), nullptr, 0);
        c->CSSetShaderResources(0, 1, &srv);
        c->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        c->CSSetConstantBuffers(0, 1, &cb);
        c->Dispatch(UINT((outW_ / 2 + kGroupX - 1) / kGroupX), UINT((outH_ + kGroupY - 1) / kGroupY), 1);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ID3D11UnorderedAccessView* nullUavs[2] = {};
        c->CSSetShaderResources(0, 1, &nullSrv);
        c->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
        c->CopyResource(flagsStaging_.get(), flags_.get());

        D3D11_MAPPED_SUBRESOURCE m;
        if (!check(c->Map(flagsStaging_.get(), 0, D3D11_MAP_READ, 0, &m), "Map(flags)")) return false;
        const uint32_t* f = static_cast<const uint32_t*>(m.pData);
        for (size_t i = 0; i < tileFlags_.size(); ++i) tileFlags_[i] = f[i] ? 1 : 0;
        c->Unmap(flagsStaging_.get(), 0);

        // Copy dirty tile rows (contiguous byte ranges) into the persistent staging mirror.
        const UINT rowBytes = UINT(outW_) * 2;
        auto copyRows = [&](int y0, int y1) {
            D3D11_BOX box = {UINT(y0) * rowBytes, 0, 0, UINT(y1) * rowBytes, 1, 1};
            c->CopySubresourceRegion(packedStaging_.get(), 0, box.left, 0, 0, packed_.get(), 0, &box);
        };
        if (!stagingValid_ || forceFull) {
            c->CopyResource(packedStaging_.get(), packed_.get());
            stagingValid_ = true;
        } else {
            int runStart = -1;
            for (int ty = 0; ty <= tilesY_; ++ty) {
                const bool d = ty < tilesY_ && rowDirty(ty);
                if (d && runStart < 0) runStart = ty;
                if (!d && runStart >= 0) {
                    copyRows(runStart * tileH_, std::min(ty * tileH_, outH_));
                    runStart = -1;
                }
            }
        }
        if (!check(c->Map(packedStaging_.get(), 0, D3D11_MAP_READ, 0, &m), "Map(frame)")) return false;
        mapped_ = true;
        out.uyvy = static_cast<const uint8_t*>(m.pData);
        out.pitch = int(rowBytes);

        if (forceFull)
            out.dirty.push_back(Rect{0, 0, outW_, outH_});
        else
            mergeDirty(out.dirty);
        return true;
    }

    void done() override {
        if (mapped_) {
            ctx_->Unmap(packedStaging_.get(), 0);
            mapped_ = false;
        }
    }

private:
    bool check(HRESULT hr, const char* what) {
        if (SUCCEEDED(hr)) return true;
        MSD_ERROR("gpu: %s failed 0x%08lx", what, hr);
        return false;
    }

    bool makeRawBuffer(UINT bytes, Com<ID3D11Buffer>& buf, Com<ID3D11UnorderedAccessView>& uav) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = bytes;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (!check(dev_->CreateBuffer(&d, nullptr, buf.put()), "CreateBuffer(raw)")) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
        u.Format = DXGI_FORMAT_R32_TYPELESS;
        u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements = bytes / 4;
        u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        return check(dev_->CreateUnorderedAccessView(buf.get(), &u, uav.put()), "CreateUnorderedAccessView");
    }

    bool makeStaging(UINT bytes, Com<ID3D11Buffer>& buf) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = bytes;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return check(dev_->CreateBuffer(&d, nullptr, buf.put()), "CreateBuffer(staging)");
    }

    // Binds src as SRV; textures that cannot be sampled directly (no SRV bind flag, MSAA) are copied first.
    bool bindSource(ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& sd) {
        if (src == lastSrc_ && srv_) {
            if (copyTex_) refreshCopy(src, sd);
            return true;
        }
        srv_.reset();
        lastSrc_ = nullptr;
        ID3D11Texture2D* viewTex = src;
        if (!(sd.BindFlags & D3D11_BIND_SHADER_RESOURCE) || sd.SampleDesc.Count > 1 || sd.MipLevels != 1 ||
            sd.ArraySize != 1) {
            D3D11_TEXTURE2D_DESC cd = {};
            if (copyTex_) copyTex_->GetDesc(&cd);
            if (!copyTex_ || cd.Width != sd.Width || cd.Height != sd.Height) {
                cd = {};
                cd.Width = sd.Width;
                cd.Height = sd.Height;
                cd.MipLevels = 1;
                cd.ArraySize = 1;
                cd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                cd.SampleDesc.Count = 1;
                cd.Usage = D3D11_USAGE_DEFAULT;
                cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (!check(dev_->CreateTexture2D(&cd, nullptr, copyTex_.put()), "CreateTexture2D(copy)"))
                    return false;
            }
            refreshCopy(src, sd);
            viewTex = copyTex_.get();
        } else {
            copyTex_.reset();
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        if (!check(dev_->CreateShaderResourceView(viewTex, &v, srv_.put()), "CreateShaderResourceView"))
            return false;
        lastSrc_ = src;  // the SRV (or copy) keeps a reference, so pointer equality means same texture
        if (copyTex_)
            srcRef_.assign(src);
        else
            srcRef_.reset();
        return true;
    }

    void refreshCopy(ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& sd) {
        if (sd.SampleDesc.Count > 1)
            ctx_->ResolveSubresource(copyTex_.get(), 0, src, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
        else
            ctx_->CopySubresourceRegion(copyTex_.get(), 0, 0, 0, 0, src, 0, nullptr);
    }

    bool rowDirty(int ty) const {
        const uint8_t* r = &tileFlags_[size_t(ty) * tilesX_];
        for (int tx = 0; tx < tilesX_; ++tx)
            if (r[tx]) return true;
        return false;
    }

    // Horizontal runs per tile row, then runs with an identical x-span in consecutive rows are merged.
    void mergeDirty(std::vector<Rect>& outRects) const {
        std::vector<Rect> open, next;
        for (int ty = 0; ty < tilesY_; ++ty) {
            next.clear();
            const uint8_t* row = &tileFlags_[size_t(ty) * tilesX_];
            const int y = ty * tileH_;
            const int h = std::min(tileH_, outH_ - y);
            size_t oi = 0;
            for (int tx = 0; tx < tilesX_;) {
                if (!row[tx]) {
                    ++tx;
                    continue;
                }
                int tx1 = tx;
                while (tx1 < tilesX_ && row[tx1]) ++tx1;
                const int x = tx * tileW_;
                const int w = std::min(tx1 * tileW_, outW_) - x;
                tx = tx1;
                // open is sorted by x: flush the ones left of this run, extend an exact match.
                while (oi < open.size() && open[oi].x < x) outRects.push_back(open[oi++]);
                if (oi < open.size() && open[oi].x == x && open[oi].w == w) {
                    Rect r = open[oi++];
                    r.h += h;
                    next.push_back(r);
                } else {
                    next.push_back(Rect{x, y, w, h});
                }
            }
            while (oi < open.size()) outRects.push_back(open[oi++]);
            open.swap(next);
        }
        outRects.insert(outRects.end(), open.begin(), open.end());
    }

    Com<ID3D11Device> dev_;
    Com<ID3D11DeviceContext> ctx_;
    Com<ID3D11ComputeShader> cs_;
    Com<ID3D11Buffer> packed_, flags_, packedStaging_, flagsStaging_, cbuf_;
    Com<ID3D11UnorderedAccessView> packedUav_, flagsUav_;
    Com<ID3D11ShaderResourceView> srv_;
    Com<ID3D11SamplerState> sampler_;
    Com<ID3D11Texture2D> copyTex_, srcRef_;
    ID3D11Texture2D* lastSrc_ = nullptr;
    Params lastParams_ = {};
    std::vector<uint8_t> tileFlags_;
    int outW_ = 0, outH_ = 0, tileW_ = 64, tileH_ = 8, tilesX_ = 0, tilesY_ = 0;
    bool stagingValid_ = false;
    bool mapped_ = false;
};

}  // namespace

std::unique_ptr<IConverter> createD3D11Converter() { return std::make_unique<D3D11Converter>(); }

void cpuBgraToUyvyRotated(const uint8_t* bgra, int srcW, int srcH, int srcPitch, Rotation rot, uint8_t* uyvy,
                          int dstPitch) {
    const int outW = swapsAxes(rot) ? srcH : srcW;
    const int outH = swapsAxes(rot) ? srcW : srcH;
    for (int oy = 0; oy < outH; ++oy) {
        uint8_t* d = uyvy + size_t(oy) * dstPitch;
        for (int ox = 0; ox + 1 < outW; ox += 2) {
            int y[2], u[2], v[2];
            for (int k = 0; k < 2; ++k) {
                const int x = ox + k;
                int sx, sy;
                switch (rot) {
                    case Rotation::CW90: sx = oy, sy = srcH - 1 - x; break;
                    case Rotation::CW180: sx = srcW - 1 - x, sy = srcH - 1 - oy; break;
                    case Rotation::CW270: sx = srcW - 1 - oy, sy = x; break;
                    default: sx = x, sy = oy; break;
                }
                const uint8_t* s = bgra + size_t(sy) * srcPitch + size_t(sx) * 4;
                const int b = s[0], g = s[1], r = s[2];
                y[k] = ((16 << 16) + 16763 * r + 32904 * g + 6391 * b) >> 16;
                u[k] = ((128 << 16) - 9676 * r - 18996 * g + 28672 * b) >> 16;
                v[k] = ((128 << 16) + 28672 * r - 24009 * g - 4663 * b) >> 16;
            }
            *d++ = clampByte((u[0] + u[1]) / 2);
            *d++ = clampByte(y[0]);
            *d++ = clampByte((v[0] + v[1]) / 2);
            *d++ = clampByte(y[1]);
        }
    }
}

}  // namespace msdisp
