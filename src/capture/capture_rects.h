// msdisp-portrait — dirty-rect helpers for the capture (header-only, unit-tested in tests/test_vdd_capture.cpp).
#pragma once
#include <algorithm>
#include <vector>

#include "msdisp/common.h"

namespace msdisp::capture {

// A DXGI move rect: the pixels of dst were copied from the same-sized rect at (srcX, srcY).
struct MoveRect {
    int srcX = 0, srcY = 0;
    Rect dst;
};

inline Rect clipRect(const Rect& r, int width, int height) {
    int x0 = std::max(r.x, 0), y0 = std::max(r.y, 0);
    int x1 = std::min(r.right(), width), y1 = std::min(r.bottom(), height);
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

// Appends r clipped to the desktop; drops empty rects and exact duplicates.
inline void addDirty(std::vector<Rect>& dirty, const Rect& r, int width, int height) {
    Rect c = clipRect(r, width, height);
    if (c.empty()) return;
    for (const Rect& d : dirty)
        if (d.x == c.x && d.y == c.y && d.w == c.w && d.h == c.h) return;
    dirty.push_back(c);
}

// Folds move rects into plain dirty rects: both the destination (new content) and the source (content that moved
// away and was then repainted / is covered by other dirty rects) are marked. Downstream only needs "what changed".
inline void foldMoveRects(const std::vector<MoveRect>& moves, std::vector<Rect>& dirty, int width, int height) {
    for (const MoveRect& m : moves) {
        addDirty(dirty, m.dst, width, height);
        addDirty(dirty, {m.srcX, m.srcY, m.dst.w, m.dst.h}, width, height);
    }
}

}  // namespace msdisp::capture
