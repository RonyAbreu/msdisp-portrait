// Desktop layout helpers: park the vendor's phantom "MS Idd" monitor while we run and put our portrait
// monitor where it was, so the mouse moves into the physical screen naturally.
#pragma once
#include <string>
#include <vector>

namespace msdisp {

struct SavedOutput {
    std::wstring gdiName;
    int x = 0, y = 0, w = 0, h = 0, hz = 0;
};

// Detaches every attached output whose adapter string contains "MS Idd". Returns what was detached.
std::vector<SavedOutput> detachVendorOutputs();
// Re-attaches outputs previously returned by detachVendorOutputs().
void restoreOutputs(const std::vector<SavedOutput>& outs);
// Re-attaches every detached "MS Idd" output (e.g. parked by a run that was killed), using its registry mode or
// 1920x1080, left of the primary. Used before handing the adapter back to the vendor app.
void reattachVendorOutputs();
// Moves gdiName to (x,y). If anchor is given (a detached vendor output), the monitor takes its place:
// right-aligned when the anchor was left of the primary, otherwise left-aligned.
bool placeOutput(const std::wstring& gdiName, int width, const SavedOutput* anchor);

}  // namespace msdisp
