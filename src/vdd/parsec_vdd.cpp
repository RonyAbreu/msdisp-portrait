// Parsec Virtual Display Driver control. Protocol from nomi-san/parsec-vdd (MIT), core/parsec-vdd.h:
// open the adapter's device interface, then overlapped DeviceIoControl ADD / REMOVE / UPDATE / VERSION with a
// 32-byte input buffer and a DWORD output. UPDATE must be repeated (< 1 s) or the driver unplugs the displays.
#include "msdisp/vdd.h"

// clang-format off
#include <windows.h>
#include <setupapi.h>
// clang-format on

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "msdisp/log.h"
#include "vdd_internal.h"

namespace msdisp {

namespace {

// {00b41627-04c4-429e-a26e-0265cf50c8fa} — "Parsec Virtual Display Adapter" device interface.
const GUID kVddAdapterGuid = {0x00b41627, 0x04c4, 0x429e, {0xa2, 0x6e, 0x02, 0x65, 0xcf, 0x50, 0xc8, 0xfa}};
constexpr wchar_t kVddMonitorId[] = L"psccdd0";  // lower-case; appears in the monitor device path
constexpr auto kKeepAlivePeriod = std::chrono::milliseconds(100);
constexpr DWORD kIoctlTimeoutMs = 5000;

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return std::towlower(c); });
    return s;
}

class ParsecVdd final : public IVirtualDisplay {
public:
    ~ParsecVdd() override { close(); }

    bool open() override {
        if (h_ != INVALID_HANDLE_VALUE) return true;
        HDEVINFO set = SetupDiGetClassDevsW(&kVddAdapterGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) return false;
        SP_DEVICE_INTERFACE_DATA ifd{};
        ifd.cbSize = sizeof(ifd);
        for (DWORD i = 0;
             h_ == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &kVddAdapterGuid, i, &ifd); ++i) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
            std::vector<uint8_t> buf(need);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr)) continue;
            HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED |
                                       FILE_FLAG_WRITE_THROUGH,
                                   nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                MSD_WARN("Parsec VDD open failed (%lu): %ls", GetLastError(), detail->DevicePath);
                continue;
            }
            h_ = h;
        }
        SetupDiDestroyDeviceInfoList(set);
        if (h_ == INVALID_HANDLE_VALUE) {
            MSD_ERROR("Parsec Virtual Display Adapter not found (driver not installed or disabled)");
            return false;
        }
        long ver = ioctl(vdd::kIoctlVersion, nullptr, 0);
        MSD_INFO("Parsec VDD opened, driver version 0.%ld", ver);
        return true;
    }

    void close() override {
        if (h_ == INVALID_HANDLE_VALUE) return;
        std::vector<int> left;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            for (auto& [idx, d] : displays_) left.push_back(idx);
        }
        for (int idx : left) remove(idx);
        stopKeepAlive();
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }

    int add() override {
        if (h_ == INVALID_HANDLE_VALUE) return -1;
        Display d;
        for (auto& t : vdd::listParsecTargets(true)) d.before.insert(lower(t.monitorPath));
        long idx = ioctl(vdd::kIoctlAdd, nullptr, 0);
        ioctl(vdd::kIoctlUpdate, nullptr, 0);
        if (idx < 0) {
            MSD_ERROR("Parsec VDD add failed");
            return -1;
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            displays_[int(idx)] = std::move(d);
        }
        startKeepAlive();
        MSD_INFO("Parsec VDD display %ld added", idx);
        return int(idx);
    }

    bool remove(int index) override {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (!displays_.erase(index)) return false;
        }
        // 16-bit big-endian index.
        uint16_t be = uint16_t(((index & 0xFF) << 8) | ((index >> 8) & 0xFF));
        bool ok = ioctl(vdd::kIoctlRemove, &be, sizeof(be)) >= 0;
        ioctl(vdd::kIoctlUpdate, nullptr, 0);
        bool empty;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            empty = displays_.empty();
        }
        if (empty) stopKeepAlive();
        MSD_INFO("Parsec VDD display %d removed%s", index, ok ? "" : " (ioctl failed)");
        return ok;
    }

    std::wstring gdiName(int index) override {
        std::lock_guard<std::mutex> lock(stateMutex_);
        auto it = displays_.find(index);
        if (it == displays_.end()) return {};
        Display& d = it->second;
        auto targets = vdd::listParsecTargets(true);
        if (d.monitorPath.empty()) {
            // Claim the first active Parsec display that appeared after our add() and is not claimed by another index.
            for (auto& t : targets) {
                std::wstring p = lower(t.monitorPath);
                if (d.before.count(p)) continue;
                bool claimed = false;
                for (auto& [i, o] : displays_) claimed |= (i != index && o.monitorPath == p);
                if (claimed) continue;
                d.monitorPath = p;
                break;
            }
        }
        for (auto& t : targets)
            if (!d.monitorPath.empty() && lower(t.monitorPath) == d.monitorPath) return t.gdiName;
        return {};
    }

private:
    struct Display {
        std::set<std::wstring> before;  // active Parsec monitor paths before add()
        std::wstring monitorPath;       // claimed monitor path (lower-case), once seen
    };

    // Returns the driver's DWORD output, or -1 on failure.
    long ioctl(uint32_t code, const void* data, size_t size) {
        std::lock_guard<std::mutex> lock(ioMutex_);
        uint8_t in[32] = {0};
        if (data && size) std::memcpy(in, data, std::min(size, sizeof(in)));
        DWORD out = 0, got = 0;
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return -1;
        long result = -1;
        if (DeviceIoControl(h_, code, in, sizeof(in), &out, sizeof(out), nullptr, &ov) ||
            GetLastError() == ERROR_IO_PENDING) {
            if (GetOverlappedResultEx(h_, &ov, &got, kIoctlTimeoutMs, FALSE)) {
                result = long(out);
            } else {
                MSD_ERROR("Parsec VDD ioctl 0x%08x failed (%lu)", unsigned(code), GetLastError());
                CancelIoEx(h_, &ov);
                GetOverlappedResult(h_, &ov, &got, TRUE);
            }
        } else {
            MSD_ERROR("Parsec VDD DeviceIoControl 0x%08x failed (%lu)", unsigned(code), GetLastError());
        }
        CloseHandle(ov.hEvent);
        return result;
    }

    void startKeepAlive() {
        if (keepAlive_.joinable()) return;
        stop_ = false;
        keepAlive_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(kaMutex_);
            while (!stop_) {
                lock.unlock();
                ioctl(vdd::kIoctlUpdate, nullptr, 0);
                lock.lock();
                kaCv_.wait_for(lock, kKeepAlivePeriod, [this] { return stop_; });
            }
        });
    }

    void stopKeepAlive() {
        if (!keepAlive_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(kaMutex_);
            stop_ = true;
        }
        kaCv_.notify_all();
        keepAlive_.join();
    }

    HANDLE h_ = INVALID_HANDLE_VALUE;
    std::mutex ioMutex_;
    std::mutex stateMutex_;
    std::map<int, Display> displays_;
    std::thread keepAlive_;
    std::mutex kaMutex_;
    std::condition_variable kaCv_;
    bool stop_ = false;
};

}  // namespace

namespace vdd {

std::vector<ParsecTarget> listParsecTargets(bool activeOnly) {
    std::vector<ParsecTarget> result;
    UINT32 flags = activeOnly ? QDC_ONLY_ACTIVE_PATHS : QDC_ALL_PATHS;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG rc;
    do {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(flags, &np, &nm) != ERROR_SUCCESS) return result;
        paths.resize(np);
        modes.resize(nm);
        rc = QueryDisplayConfig(flags, &np, paths.data(), &nm, modes.data(), nullptr);
        paths.resize(np);
    } while (rc == ERROR_INSUFFICIENT_BUFFER);
    if (rc != ERROR_SUCCESS) return result;

    std::set<std::pair<uint64_t, uint32_t>> seen;
    for (auto& p : paths) {
        if (!p.targetInfo.targetAvailable) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = p.targetInfo.adapterId;
        tn.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) continue;
        std::wstring path = tn.monitorDevicePath;
        if (lower(path).find(kVddMonitorId) == std::wstring::npos) continue;
        bool active = (p.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        uint64_t luid = (uint64_t(uint32_t(p.targetInfo.adapterId.HighPart)) << 32) | p.targetInfo.adapterId.LowPart;
        // With QDC_ALL_PATHS every target appears once per possible source; keep one entry, preferring active.
        if (!active && !seen.insert({luid, p.targetInfo.id}).second) continue;
        ParsecTarget t;
        t.monitorPath = path;
        t.targetId = p.targetInfo.id;
        t.connector = tn.connectorInstance;
        t.active = active;
        if (active) {
            seen.insert({luid, p.targetInfo.id});
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
            sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sn.header.size = sizeof(sn);
            sn.header.adapterId = p.sourceInfo.adapterId;
            sn.header.id = p.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS) t.gdiName = sn.viewGdiDeviceName;
        }
        result.push_back(std::move(t));
    }
    // Drop inactive duplicates of targets that are active.
    std::vector<ParsecTarget> out;
    for (auto& t : result) {
        bool dup = !t.active && std::any_of(result.begin(), result.end(), [&](const ParsecTarget& o) {
            return o.active && o.targetId == t.targetId && o.monitorPath == t.monitorPath;
        });
        if (!dup) out.push_back(std::move(t));
    }
    return out;
}

}  // namespace vdd

std::unique_ptr<IVirtualDisplay> createParsecVdd() { return std::make_unique<ParsecVdd>(); }

bool parsecVddSetCustomMode(int slot, int width, int height, int hz) {
    if (slot < 0 || slot > 9 || width <= 0 || height <= 0 || hz <= 0) return false;
    wchar_t key[64];
    swprintf(key, 64, L"SOFTWARE\\Parsec\\vdd\\%d", slot);
    HKEY h = nullptr;
    LSTATUS rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &h,
                                 nullptr);
    if (rc != ERROR_SUCCESS) {
        if (rc == ERROR_ACCESS_DENIED)
            MSD_WARN("parsecVddSetCustomMode: HKLM\\%ls needs administrator rights", key);
        else
            MSD_ERROR("parsecVddSetCustomMode: RegCreateKeyEx failed (%ld)", long(rc));
        return false;
    }
    const DWORD vals[3] = {DWORD(width), DWORD(height), DWORD(hz)};
    const wchar_t* names[3] = {L"width", L"height", L"hz"};
    bool ok = true;
    for (int i = 0; i < 3; ++i)
        ok &= RegSetValueExW(h, names[i], 0, REG_DWORD, reinterpret_cast<const BYTE*>(&vals[i]), sizeof(DWORD)) ==
              ERROR_SUCCESS;
    RegCloseKey(h);
    if (ok) MSD_INFO("Parsec VDD custom mode slot %d = %dx%d@%d", slot, width, height, hz);
    return ok;
}

bool setDisplayMode(const std::wstring& gdiName, int width, int height, int hz) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    bool found = false;
    for (DWORD i = 0; EnumDisplaySettingsExW(gdiName.c_str(), i, &dm, 0); ++i) {
        if (int(dm.dmPelsWidth) == width && int(dm.dmPelsHeight) == height &&
            (hz <= 0 || int(dm.dmDisplayFrequency) == hz) && dm.dmDisplayOrientation == DMDO_DEFAULT) {
            found = true;
            break;
        }
    }
    if (!found) {
        MSD_WARN("setDisplayMode: %ls has no native mode %dx%d@%d", gdiName.c_str(), width, height, hz);
        return false;
    }
    DEVMODEW cur{};
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsExW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &cur, 0)) return false;
    cur.dmPelsWidth = dm.dmPelsWidth;
    cur.dmPelsHeight = dm.dmPelsHeight;
    cur.dmDisplayFrequency = dm.dmDisplayFrequency;
    cur.dmDisplayOrientation = DMDO_DEFAULT;
    cur.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_DISPLAYORIENTATION;
    LONG rc = ChangeDisplaySettingsExW(gdiName.c_str(), &cur, nullptr, CDS_UPDATEREGISTRY, nullptr);
    if (rc != DISP_CHANGE_SUCCESSFUL) {
        MSD_ERROR("setDisplayMode: ChangeDisplaySettingsEx(%ls, %dx%d@%d) = %ld", gdiName.c_str(), width, height,
                  int(dm.dmDisplayFrequency), long(rc));
        return false;
    }
    MSD_INFO("setDisplayMode: %ls -> %dx%d@%d", gdiName.c_str(), width, height, int(dm.dmDisplayFrequency));
    return true;
}

}  // namespace msdisp
