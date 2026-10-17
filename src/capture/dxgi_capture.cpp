// DXGI Desktop Duplication capture of one output.
//
// Frame ownership: acquire() copies the duplicated desktop into an internal texture and calls ReleaseFrame() right
// away (early release), so DWM gets its surface back immediately and the caller may hold CapturedFrame::texture as
// long as it likes until the next acquire(). release() is therefore only bookkeeping.
//
// Cursor: Desktop Duplication does not draw the hardware cursor into the image. We keep two textures: clean_ (the
// desktop as duplicated) and out_ (clean_ + cursor composited on the CPU from GetFramePointerShape). Pointer-only
// updates (LastPresentTime == 0) don't copy the desktop; they repaint the old cursor area from clean_, draw the
// cursor at the new place and report both rects dirty. Updates that change nothing visible are not returned.
#include "msdisp/capture.h"

// clang-format off
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
// clang-format on

#include <algorithm>
#include <cstring>

#include "capture_rects.h"
#include "msdisp/log.h"

using Microsoft::WRL::ComPtr;

namespace msdisp {

namespace {

struct Cursor {
    bool visible = false;
    POINT pos{};  // top-left of the shape in output coordinates
    DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
    std::vector<uint8_t> shape;

    Rect rect() const {
        if (!visible || shape.empty()) return {};
        int h = int(info.Height);
        if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) h /= 2;
        return {int(pos.x), int(pos.y), int(info.Width), h};
    }
};

class DxgiCapture final : public ICapture {
public:
    ~DxgiCapture() override { shutdown(); }

    bool init(const std::wstring& gdiName) override {
        shutdown();
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput1> output;
        for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
            ComPtr<IDXGIOutput> o;
            for (UINT i = 0; adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_OUTPUT_DESC od;
                if (SUCCEEDED(o->GetDesc(&od)) && _wcsicmp(od.DeviceName, gdiName.c_str()) == 0) {
                    o.As(&output);
                    break;
                }
            }
            if (!output) adapter.Reset();
        }
        if (!output) {
            MSD_ERROR("capture: output %ls not found in DXGI", gdiName.c_str());
            return false;
        }
        DXGI_ADAPTER_DESC1 ad;
        adapter->GetDesc1(&ad);

        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &dev_, nullptr,
                                       &ctx_);
        if (FAILED(hr)) {
            MSD_ERROR("capture: D3D11CreateDevice on %ls failed (0x%08lx)", ad.Description, hr);
            return false;
        }
        hr = output->DuplicateOutput(dev_.Get(), &dup_);
        if (FAILED(hr)) {
            MSD_ERROR("capture: DuplicateOutput(%ls) failed (0x%08lx)", gdiName.c_str(), hr);
            shutdown();
            return false;
        }
        dup_->GetDesc(&desc_);
        if (desc_.Rotation != DXGI_MODE_ROTATION_IDENTITY && desc_.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
            // The duplicated image would be in scan-out orientation; we only support native (unrotated) outputs.
            MSD_ERROR("capture: %ls is rotated (DXGI rotation %d); use a native portrait mode instead",
                      gdiName.c_str(), int(desc_.Rotation));
            shutdown();
            return false;
        }
        width_ = int(desc_.ModeDesc.Width);
        height_ = int(desc_.ModeDesc.Height);

        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(width_);
        td.Height = UINT(height_);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &clean_))) return fail("CreateTexture2D(clean)");
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &out_))) return fail("CreateTexture2D(out)");

        first_ = true;
        MSD_INFO("capture: %ls %dx%d on '%ls' (desktop image in system memory: %d)", gdiName.c_str(), width_, height_,
                 ad.Description, int(desc_.DesktopImageInSystemMemory));
        return true;
    }

    ID3D11Device* device() const override { return dev_.Get(); }
    ID3D11DeviceContext* context() const override { return ctx_.Get(); }

    AcquireResult acquire(CapturedFrame& out, unsigned timeoutMs) override {
        if (!dup_) return AcquireResult::Error;
        const int64_t deadline = nowUs() + int64_t(timeoutMs) * 1000;
        std::vector<Rect> dirty;
        for (;;) {
            int64_t left = (deadline - nowUs()) / 1000;
            DXGI_OUTDUPL_FRAME_INFO info;
            ComPtr<IDXGIResource> res;
            HRESULT hr = dup_->AcquireNextFrame(UINT(std::max<int64_t>(left, 0)), &info, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) return AcquireResult::Timeout;
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                MSD_WARN("capture: access lost");
                dup_.Reset();
                return AcquireResult::Lost;
            }
            if (FAILED(hr)) {
                MSD_ERROR("capture: AcquireNextFrame failed (0x%08lx)", hr);
                return AcquireResult::Error;
            }

            const bool newImage = info.LastPresentTime.QuadPart != 0;
            const Rect oldCursor = clipped(cursor_.rect());
            bool cursorChanged = false;
            if (info.LastMouseUpdateTime.QuadPart != 0) cursorChanged = updateCursor(info);
            if ((!newImage && !cursorChanged) || (first_ && !newImage)) {
                dup_->ReleaseFrame();
                if (nowUs() >= deadline) return AcquireResult::Timeout;
                continue;  // pointer-only update with no visible change: not a frame
            }

            bool full = first_;
            ComPtr<ID3D11Texture2D> tex;
            if (newImage) {
                if (!full && !readMetadata(info, dirty)) full = true;
                if (FAILED(res.As(&tex)) || !checkDesktop(tex.Get())) {
                    dup_->ReleaseFrame();
                    return AcquireResult::Error;
                }
            }
            // Blend the cursor first: its small readback then doesn't wait for the full-frame copies queued below.
            const Rect newCursor = clipped(cursor_.rect());
            const bool cursorDrawn = blendCursor(newImage ? tex.Get() : clean_.Get(), newCursor);
            if (newImage) {
                ctx_->CopyResource(clean_.Get(), tex.Get());
                ctx_->CopyResource(out_.Get(), tex.Get());
            } else {
                restoreArea(oldCursor);
            }
            dup_->ReleaseFrame();
            if (cursorDrawn) {
                const Rect& c = newCursor;
                D3D11_BOX box{UINT(c.x), UINT(c.y), 0, UINT(c.right()), UINT(c.bottom()), 1};
                ctx_->UpdateSubresource(out_.Get(), 0, &box, pixels_.data(), UINT(newCursor.w) * 4, 0);
            }
            if (cursorChanged) {
                capture::addDirty(dirty, oldCursor, width_, height_);
                capture::addDirty(dirty, newCursor, width_, height_);
            }

            first_ = false;
            out.texture = out_.Get();
            out.width = width_;
            out.height = height_;
            out.full = full;
            if (full) dirty.assign(1, Rect{0, 0, width_, height_});
            out.dirty = std::move(dirty);
            out.timeUs = nowUs();
            return AcquireResult::Frame;
        }
    }

    void release() override {}

    void shutdown() override {
        dup_.Reset();
        out_.Reset();
        clean_.Reset();
        staging_.Reset();
        stagingW_ = stagingH_ = 0;
        ctx_.Reset();
        dev_.Reset();
        cursor_ = {};
        width_ = height_ = 0;
    }

private:
    bool fail(const char* what) {
        MSD_ERROR("capture: %s failed", what);
        shutdown();
        return false;
    }

    Rect clipped(const Rect& r) const { return capture::clipRect(r, width_, height_); }

    bool checkDesktop(ID3D11Texture2D* tex) {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM || int(d.Width) != width_ || int(d.Height) != height_) {
            MSD_ERROR("capture: unexpected desktop texture %ux%u format %d", d.Width, d.Height, int(d.Format));
            return false;
        }
        return true;
    }

    // Returns false if the metadata could not be read (caller then marks the frame full).
    bool readMetadata(const DXGI_OUTDUPL_FRAME_INFO& info, std::vector<Rect>& dirty) {
        if (info.TotalMetadataBufferSize == 0) return false;
        if (meta_.size() < info.TotalMetadataBufferSize) meta_.resize(info.TotalMetadataBufferSize);
        UINT used = 0;
        if (FAILED(dup_->GetFrameMoveRects(UINT(meta_.size()),
                                           reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta_.data()), &used)))
            return false;
        std::vector<capture::MoveRect> moves;
        auto* mr = reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta_.data());
        for (UINT i = 0; i < used / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i) {
            const RECT& r = mr[i].DestinationRect;
            moves.push_back({int(mr[i].SourcePoint.x), int(mr[i].SourcePoint.y),
                             {int(r.left), int(r.top), int(r.right - r.left), int(r.bottom - r.top)}});
        }
        capture::foldMoveRects(moves, dirty, width_, height_);
        UINT moveBytes = used;
        if (FAILED(dup_->GetFrameDirtyRects(UINT(meta_.size() - moveBytes),
                                            reinterpret_cast<RECT*>(meta_.data() + moveBytes), &used)))
            return false;
        auto* dr = reinterpret_cast<RECT*>(meta_.data() + moveBytes);
        for (UINT i = 0; i < used / sizeof(RECT); ++i)
            capture::addDirty(dirty, {int(dr[i].left), int(dr[i].top), int(dr[i].right - dr[i].left),
                                      int(dr[i].bottom - dr[i].top)},
                              width_, height_);
        return true;
    }

    // Returns true if what is drawn changed (position, visibility or shape).
    bool updateCursor(const DXGI_OUTDUPL_FRAME_INFO& info) {
        bool changed = false;
        const auto& p = info.PointerPosition;
        if (bool(p.Visible) != cursor_.visible ||
            (p.Visible && (p.Position.x != cursor_.pos.x || p.Position.y != cursor_.pos.y)))
            changed = true;
        cursor_.visible = p.Visible;
        if (p.Visible) cursor_.pos = p.Position;
        if (info.PointerShapeBufferSize) {
            cursor_.shape.resize(info.PointerShapeBufferSize);
            UINT need = 0;
            if (FAILED(dup_->GetFramePointerShape(UINT(cursor_.shape.size()), cursor_.shape.data(), &need,
                                                  &cursor_.info)))
                cursor_.shape.clear();
            changed = true;
        }
        return changed;
    }

    bool ensureStaging(int w, int h) {
        if (staging_ && stagingW_ >= w && stagingH_ >= h) return true;
        staging_.Reset();
        stagingW_ = std::max(w, stagingW_);
        stagingH_ = std::max(h, stagingH_);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(stagingW_);
        td.Height = UINT(stagingH_);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &staging_));
    }

    void restoreArea(const Rect& r) {
        if (r.empty()) return;
        D3D11_BOX box{UINT(r.x), UINT(r.y), 0, UINT(r.right()), UINT(r.bottom()), 1};
        ctx_->CopySubresourceRegion(out_.Get(), 0, UINT(r.x), UINT(r.y), 0, clean_.Get(), 0, &box);
    }

    // Reads the desktop pixels of src under the cursor rect r (clipped to the desktop) and blends the cursor into
    // pixels_. Returns false if there is nothing to draw.
    bool blendCursor(ID3D11Texture2D* src, const Rect& r) {
        if (r.empty() || !ensureStaging(r.w, r.h)) return false;
        D3D11_BOX box{UINT(r.x), UINT(r.y), 0, UINT(r.right()), UINT(r.bottom()), 1};
        ctx_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, src, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        pixels_.resize(size_t(r.w) * r.h);
        for (int y = 0; y < r.h; ++y)
            std::memcpy(&pixels_[size_t(y) * r.w], static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch,
                        size_t(r.w) * 4);
        ctx_->Unmap(staging_.Get(), 0);

        const auto& si = cursor_.info;
        const int ox = r.x - int(cursor_.pos.x), oy = r.y - int(cursor_.pos.y);  // offset into the shape
        const uint8_t* s = cursor_.shape.data();
        for (int y = 0; y < r.h; ++y) {
            uint32_t* dst = &pixels_[size_t(y) * r.w];
            const int sy = y + oy;
            for (int x = 0; x < r.w; ++x) {
                const int sx = x + ox;
                uint32_t d = dst[x];
                if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                    const int h = int(si.Height) / 2;
                    const uint8_t bit = uint8_t(0x80 >> (sx & 7));
                    const bool andBit = s[size_t(sy) * si.Pitch + sx / 8] & bit;
                    const bool xorBit = s[size_t(sy + h) * si.Pitch + sx / 8] & bit;
                    d = ((d & (andBit ? 0xFFFFFFFFu : 0xFF000000u)) ^ (xorBit ? 0x00FFFFFFu : 0u)) | 0xFF000000u;
                } else {
                    uint32_t c;
                    std::memcpy(&c, s + size_t(sy) * si.Pitch + size_t(sx) * 4, 4);
                    if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                        d = ((c >> 24) == 0 ? c : (c ^ d)) | 0xFF000000u;
                    } else {  // color: straight alpha
                        const uint32_t a = c >> 24;
                        uint32_t o = 0xFF000000u;
                        for (int sh = 0; sh < 24; sh += 8) {
                            uint32_t cs = (c >> sh) & 0xFF, cd = (d >> sh) & 0xFF;
                            o |= ((cs * a + cd * (255 - a) + 127) / 255) << sh;
                        }
                        d = o;
                    }
                }
                dst[x] = d;
            }
        }
        return true;
    }

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> clean_, out_, staging_;
    int stagingW_ = 0, stagingH_ = 0;
    DXGI_OUTDUPL_DESC desc_{};
    int width_ = 0, height_ = 0;
    bool first_ = true;
    std::vector<uint8_t> meta_;
    std::vector<uint32_t> pixels_;
    Cursor cursor_;
};

}  // namespace

std::unique_ptr<ICapture> createDxgiCapture() { return std::make_unique<DxgiCapture>(); }

}  // namespace msdisp
