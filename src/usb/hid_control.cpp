#include "hid_control.h"

// clang-format off
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
// clang-format on

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <vector>

#include "msdisp/log.h"

namespace msdisp {

bool HidControl::open(uint16_t vid, uint16_t pid, int mi) {
    close();
    wchar_t needle[64];
    swprintf(needle, 64, L"vid_%04x&pid_%04x&mi_%02d", vid, pid, mi);

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return false;

    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        std::vector<uint8_t> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr)) continue;
        std::wstring p = detail->DevicePath;
        std::wstring lower = p;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return std::towlower(c); });
        if (lower.find(needle) == std::wstring::npos) continue;
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            MSD_WARN("HID open failed (%lu): %ls", GetLastError(), p.c_str());
            continue;
        }
        h_ = h;
        path_ = p;
        break;
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!isOpen()) MSD_ERROR("HID control interface %ls not found", needle);
    return isOpen();
}

void HidControl::close() {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
}

bool HidControl::set(const uint8_t req[8]) {
    uint8_t buf[9] = {0};
    std::memcpy(buf + 1, req, 8);
    if (!HidD_SetFeature(h_, buf, sizeof(buf))) {
        MSD_ERROR("HidD_SetFeature failed (%lu) req=%02x %02x %02x", GetLastError(), req[0], req[1], req[2]);
        return false;
    }
    return true;
}

bool HidControl::get(uint8_t resp[8]) {
    uint8_t buf[9] = {0};
    if (!HidD_GetFeature(h_, buf, sizeof(buf))) {
        MSD_ERROR("HidD_GetFeature failed (%lu)", GetLastError());
        return false;
    }
    std::memcpy(resp, buf + 1, 8);
    return true;
}

bool HidControl::readXdata(uint16_t addr, uint8_t* out, int count) {
    std::lock_guard<std::mutex> lock(mu_);
    while (count > 0) {
        int n = std::min(count, 4);
        uint8_t req[8] = {0xB5, static_cast<uint8_t>(addr >> 8), static_cast<uint8_t>(addr & 0xff), 0, 0, 0, 0, 0};
        uint8_t resp[8];
        if (!set(req) || !get(resp)) return false;
        std::memcpy(out, resp + 3, n);
        out += n;
        addr = static_cast<uint16_t>(addr + n);
        count -= n;
    }
    return true;
}

bool HidControl::writeByte(uint16_t addr, uint8_t value) {
    std::lock_guard<std::mutex> lock(mu_);
    uint8_t req[8] = {0xB6, static_cast<uint8_t>(addr >> 8), static_cast<uint8_t>(addr & 0xff), value, 0, 0, 0, 0};
    return set(req);
}

bool HidControl::video(uint8_t subOp, const uint8_t data[6]) {
    std::lock_guard<std::mutex> lock(mu_);
    uint8_t req[8] = {0xA6, subOp};
    std::memcpy(req + 2, data, 6);
    return set(req);
}

}  // namespace msdisp
