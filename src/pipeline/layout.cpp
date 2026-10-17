#include "layout.h"

#include <windows.h>

#include "msdisp/log.h"

namespace msdisp {

namespace {
bool applyPending() {
    LONG r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) MSD_WARN("layout: apply failed (%ld)", r);
    return r == DISP_CHANGE_SUCCESSFUL;
}
}  // namespace

std::vector<SavedOutput> detachVendorOutputs() {
    std::vector<SavedOutput> out;
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd.cb = sizeof(dd)) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        if (!wcsstr(dd.DeviceString, L"MS Idd")) continue;
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) continue;
        SavedOutput s{dd.DeviceName, int(dm.dmPosition.x), int(dm.dmPosition.y), int(dm.dmPelsWidth),
                      int(dm.dmPelsHeight), int(dm.dmDisplayFrequency)};
        DEVMODEW off{};
        off.dmSize = sizeof(off);
        off.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
        LONG r = ChangeDisplaySettingsExW(dd.DeviceName, &off, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        if (r == DISP_CHANGE_SUCCESSFUL) {
            MSD_INFO("layout: parked vendor output %ls (%d,%d %dx%d)", s.gdiName.c_str(), s.x, s.y, s.w, s.h);
            out.push_back(s);
        } else {
            MSD_WARN("layout: could not park %ls (%ld)", dd.DeviceName, r);
        }
    }
    if (!out.empty()) applyPending();
    return out;
}

void restoreOutputs(const std::vector<SavedOutput>& outs) {
    for (const SavedOutput& s : outs) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | (s.hz ? DM_DISPLAYFREQUENCY : 0);
        dm.dmPosition.x = s.x;
        dm.dmPosition.y = s.y;
        dm.dmPelsWidth = DWORD(s.w);
        dm.dmPelsHeight = DWORD(s.h);
        dm.dmDisplayFrequency = DWORD(s.hz);
        LONG r = ChangeDisplaySettingsExW(s.gdiName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        MSD_INFO("layout: restore %ls -> %ld", s.gdiName.c_str(), r);
    }
    if (!outs.empty()) applyPending();
}

bool placeOutput(const std::wstring& gdiName, int width, const SavedOutput* anchor) {
    int x = -width, y = 0;  // default: left of the primary, top aligned
    if (anchor) {
        x = anchor->x < 0 ? anchor->x + anchor->w - width : anchor->x;
        y = anchor->y;
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_POSITION;
    dm.dmPosition.x = x;
    dm.dmPosition.y = y;
    LONG r = ChangeDisplaySettingsExW(gdiName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) {
        MSD_WARN("layout: could not move %ls to (%d,%d) (%ld)", gdiName.c_str(), x, y, r);
        return false;
    }
    MSD_INFO("layout: %ls placed at (%d,%d)", gdiName.c_str(), x, y);
    return applyPending();
}

}  // namespace msdisp
