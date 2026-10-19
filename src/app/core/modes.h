// msdisp-portrait — output modes offered by the shell (MS912x mode codes). Track D.
#pragma once
#include <cstdint>

#include "msdisp/common.h"

namespace msdisp {

struct KnownMode {
    int width, height, hz;
    uint8_t vic;
};

// Order = order in the tray "Resolução" submenu.
inline constexpr KnownMode kKnownModes[] = {
    {1920, 1080, 60, 0x81},  // sharp, USB 2.0 bound (~7.6 fps full screen)
    {1366, 768, 60, 0x66},   // native 768x1366 desktop: ~2x fewer bytes
    {1280, 720, 60, 0x4F},   // native 720x1280 desktop: ~2.25x fewer bytes
};

inline Mode toMode(const KnownMode& k) { return Mode{k.width, k.height, k.hz, k.vic}; }

// Chip code for a known width/height/hz, or -1.
inline int vicForMode(int w, int h, int hz) {
    for (const auto& k : kKnownModes)
        if (k.width == w && k.height == h && k.hz == hz) return k.vic;
    return -1;
}

}  // namespace msdisp
