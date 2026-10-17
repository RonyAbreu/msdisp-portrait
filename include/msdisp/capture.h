// msdisp-portrait — desktop capture of one Windows output. FROZEN CONTRACT.
// Owner: track B (src/capture). Implementation: DXGI Desktop Duplication (fallback: WGC).
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "msdisp/common.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace msdisp {

struct CapturedFrame {
    ID3D11Texture2D* texture = nullptr;  // BGRA8, owned by the capture; valid until release()
    int width = 0, height = 0;           // desktop size of the output as Windows sees it
    std::vector<Rect> dirty;             // changed areas in desktop coords (move rects folded in)
    bool full = false;                   // true => treat whole frame as dirty
    int64_t timeUs = 0;                  // nowUs() at acquire
};

enum class AcquireResult { Frame, Timeout, Lost /* call init() again */, Error };

class ICapture {
public:
    virtual ~ICapture() = default;
    // gdiName like L"\\\\.\\DISPLAY23". Creates its own D3D11 device on the adapter owning the output.
    virtual bool init(const std::wstring& gdiName) = 0;
    virtual ID3D11Device* device() const = 0;
    virtual ID3D11DeviceContext* context() const = 0;
    virtual AcquireResult acquire(CapturedFrame& out, unsigned timeoutMs) = 0;
    virtual void release() = 0;  // must be called after each Frame result
    virtual void shutdown() = 0;
};

std::unique_ptr<ICapture> createDxgiCapture();

}  // namespace msdisp
