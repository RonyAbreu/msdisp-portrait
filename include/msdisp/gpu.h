// msdisp-portrait — GPU rotate + BGRA->UYVY + tile diff. FROZEN CONTRACT.
// Owner: track C (src/gpu).
#pragma once
#include <memory>
#include <vector>

#include "msdisp/common.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace msdisp {

struct ConvertResult {
    const uint8_t* uyvy = nullptr;  // CPU-visible full output frame (outW x outH, 2 B/px)
    int pitch = 0;                  // bytes per row
    std::vector<Rect> dirty;        // changed output rects (tile aligned, x/w even), merged
};

class IConverter {
public:
    virtual ~IConverter() = default;
    // outW/outH = chip mode size (e.g. 1920x1080). tile = diff tile size in output pixels.
    virtual bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, int outW, int outH, int tileW = 64,
                      int tileH = 8) = 0;
    // src: BGRA8 texture whose size, after rotation, must equal outW x outH (scaling: optional,
    // see scaleToFit). srcDirty: optional hint in source coords (may be empty = unknown/full).
    // forceFull: ignore diff, report the whole frame dirty (first frame / after mode change).
    virtual bool process(ID3D11Texture2D* src, Rotation rot, const std::vector<Rect>& srcDirty, bool forceFull,
                         ConvertResult& out) = 0;
    // Must be called once the caller is done with out.uyvy (unmaps staging).
    virtual void done() = 0;
};

std::unique_ptr<IConverter> createD3D11Converter();

// CPU reference used by tests (and as a fallback). BT.601 limited range, same math as rhgndf/ms912x.
void cpuBgraToUyvyRotated(const uint8_t* bgra, int srcW, int srcH, int srcPitch, Rotation rot, uint8_t* uyvy,
                          int dstPitch);

}  // namespace msdisp
