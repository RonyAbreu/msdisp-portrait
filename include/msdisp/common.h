// msdisp-portrait — shared types. FROZEN CONTRACT: changes go through the integrator.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace msdisp {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    int right() const { return x + w; }
    int bottom() const { return y + h; }
};

// Output video mode understood by the MS912x chip.
struct Mode {
    int width = 1920;
    int height = 1080;
    int hz = 60;
    uint8_t vic = 0x81;  // chip mode code (see ms912x mode table)
};

// Clockwise rotation applied to the captured (source) image before it is sent.
// Portrait monitor physically turned 90° clockwise => Rotation::CW270 usually; configurable.
enum class Rotation : int { None = 0, CW90 = 90, CW180 = 180, CW270 = 270 };

// Monotonic microseconds (QPC based).
int64_t nowUs();

}  // namespace msdisp
