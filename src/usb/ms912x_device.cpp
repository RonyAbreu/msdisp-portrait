// MS912x (534D:6021) device: HID control + libusb0 async bulk frame transfer.
// Protocol references: MacroSilicon MS91xx Linux DRM source (GPL-2.0), rhgndf/ms912x (GPL-2.0).
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "hid_control.h"
#include "libusb0_api.h"
#include "msdisp/device.h"
#include "msdisp/log.h"

namespace msdisp {

namespace {

constexpr uint16_t kVid = 0x534D;
constexpr uint16_t kPid = 0x6021;
constexpr int kHidInterface = 0;
constexpr unsigned char kBulkEp = 0x04;
constexpr int kSlots = 3;
constexpr int kEtimedout = 116;  // libusb-win32 errno value
constexpr int kHeaderLen = 8;
constexpr uint8_t kTerminator[8] = {0xFF, 0xC0, 0, 0, 0, 0, 0, 0};

// HID video sub-ops (0xA6 xx)
constexpr uint8_t kSubInInfo = 0x01;
constexpr uint8_t kSubOutInfo = 0x02;
constexpr uint8_t kSubTransMode = 0x03;
constexpr uint8_t kSubTransfer = 0x04;
constexpr uint8_t kSubVideoEnable = 0x05;
constexpr uint8_t kSubPower = 0x07;
constexpr uint8_t kPixFmtUyvy = 0x22;
constexpr uint8_t kTransModeManualBlock = 0x03;

constexpr uint16_t kRegChipId = 0xF000;
constexpr uint16_t kRegVideoPort = 0x0031;
constexpr uint16_t kRegHpd = 0x0032;
constexpr uint16_t kRegEdid = 0xC000;

void putHeader(uint8_t* p, const Rect& r) {
    uint32_t pos = ((uint32_t(r.x) & 0xfff) << 12) | (uint32_t(r.y) & 0xfff);
    uint32_t dim = ((uint32_t(r.w) & 0xfff) << 12) | (uint32_t(r.h) & 0xfff);
    p[0] = 0xFF;
    p[1] = 0x00;
    p[2] = uint8_t(pos >> 16);
    p[3] = uint8_t(pos >> 8);
    p[4] = uint8_t(pos);
    p[5] = uint8_t(dim >> 16);
    p[6] = uint8_t(dim >> 8);
    p[7] = uint8_t(dim);
}

class Ms912xDevice final : public IDevice {
public:
    ~Ms912xDevice() override { close(); }

    bool open() override {
        close();
        if (!hid_.open(kVid, kPid, kHidInterface)) return false;
        const usb0::Api* a = usb0::api();
        if (!a) return false;
        a->find_busses();
        a->find_devices();
        usb0::usb_device* found = nullptr;
        for (usb0::usb_bus* b = a->get_busses(); b && !found; b = b->next)
            for (usb0::usb_device* d = b->devices; d; d = d->next)
                if (d->descriptor.idVendor == kVid && d->descriptor.idProduct == kPid) {
                    found = d;
                    break;
                }
        if (!found) {
            MSD_ERROR("libusb0: device %04x:%04x not found (is libusb0.sys bound to MI_03?)", kVid, kPid);
            hid_.close();
            return false;
        }
        uh_ = a->open(found);
        if (!uh_) {
            MSD_ERROR("libusb0: usb_open failed: %s", a->strerror());
            hid_.close();
            return false;
        }
        for (int attempt = 0; attempt < 2 && iface_ < 0; ++attempt) {
            if (attempt == 1) a->set_configuration(uh_, 1);
            for (int i : {3, 0})
                if (a->claim_interface(uh_, i) >= 0) {
                    iface_ = i;
                    break;
                }
        }
        if (iface_ < 0) {
            MSD_ERROR("libusb0: claim_interface failed: %s (vendor app still running?)", a->strerror());
            close();
            return false;
        }
        for (auto& s : slots_) {
            if (a->bulk_setup_async(uh_, &s.ctx, kBulkEp) < 0) {
                MSD_ERROR("libusb0: bulk_setup_async failed: %s", a->strerror());
                close();
                return false;
            }
        }
        const char* env = std::getenv("MSDISP_SINGLE_RECT");
        multiRect_ = !(env && *env == '1');
        MSD_INFO("device open: %s iface=%d hid=%ls multiRect=%d", found->filename, iface_, hid_.path().c_str(),
                 multiRect_);
        return true;
    }

    void close() override {
        const usb0::Api* a = usb0::api();
        if (uh_ && a) {
            flush(500);
            for (auto& s : slots_) {
                if (s.busy) a->cancel_async(s.ctx);
                if (s.ctx) a->free_async(&s.ctx);
                s = Slot{};
            }
            if (iface_ >= 0) a->release_interface(uh_, iface_);
            a->close(uh_);
        }
        uh_ = nullptr;
        iface_ = -1;
        hid_.close();
    }

    bool isOpen() const override { return uh_ != nullptr && hid_.isOpen(); }

    bool readInfo(DeviceInfo& out) override {
        uint8_t port = 0, hpd = 0;
        if (!hid_.readXdata(kRegChipId, out.chipId, 3)) return false;
        if (!hid_.readByte(kRegVideoPort, port) || !hid_.readByte(kRegHpd, hpd)) return false;
        out.videoPort = port;
        out.hotplug = hpd != 0;
        return true;
    }

    bool readEdid(std::vector<uint8_t>& out) override {
        out.assign(128, 0);
        if (!hid_.readXdata(kRegEdid, out.data(), 128)) return false;
        if (out[126] > 0) {  // extension block present
            out.resize(256);
            if (!hid_.readXdata(kRegEdid + 128, out.data() + 128, 128)) out.resize(128);
        }
        return true;
    }

    bool start(const Mode& m) override {
        const uint8_t zero[6] = {0};
        uint8_t d[6];
        auto vid = [&](uint8_t sub, std::initializer_list<uint8_t> bytes) {
            std::memset(d, 0, 6);
            std::copy(bytes.begin(), bytes.end(), d);
            return hid_.video(sub, d);
        };
        uint8_t dummy;
        bool ok = vid(kSubPower, {0x01, 0x02}) && hid_.video(kSubTransfer, zero) &&
                  hid_.readByte(0x0030, dummy) && hid_.readByte(0x0033, dummy) && hid_.readByte(0xC620, dummy) &&
                  vid(kSubTransMode, {kTransModeManualBlock}) &&
                  vid(kSubInInfo, {uint8_t(m.width >> 8), uint8_t(m.width), uint8_t(m.height >> 8),
                                   uint8_t(m.height), kPixFmtUyvy, 0x00}) &&
                  vid(kSubOutInfo, {m.vic, 0x01, uint8_t(m.width >> 8), uint8_t(m.width), uint8_t(m.height >> 8),
                                    uint8_t(m.height)}) &&
                  vid(kSubTransfer, {0x01}) && vid(kSubVideoEnable, {0x01});
        if (ok) {
            mode_ = m;
            MSD_INFO("device started %dx%d@%d vic=0x%02x", m.width, m.height, m.hz, m.vic);
        } else {
            MSD_ERROR("device start sequence failed");
        }
        return ok;
    }

    void stop() override {
        const uint8_t zero[6] = {0};
        flush(1000);
        hid_.video(kSubTransfer, zero);
        hid_.video(kSubVideoEnable, zero);
        hid_.video(kSubPower, zero);
    }

    bool submit(const FrameUpdate& u) override {
        if (!uh_ || u.rects.empty()) return false;
        std::vector<Rect> rects;
        rects.reserve(u.rects.size());
        Rect bbox{};
        bool first = true;
        for (Rect r : u.rects) {
            int x0 = std::clamp(r.x & ~1, 0, mode_.width), y0 = std::clamp(r.y, 0, mode_.height);
            int x1 = std::clamp((r.right() + 1) & ~1, 0, mode_.width), y1 = std::clamp(r.bottom(), 0, mode_.height);
            if (x1 <= x0 || y1 <= y0) continue;
            Rect c{x0, y0, x1 - x0, y1 - y0};
            rects.push_back(c);
            if (first) bbox = c, first = false;
            else {
                int bx1 = std::max(bbox.right(), c.right()), by1 = std::max(bbox.bottom(), c.bottom());
                bbox.x = std::min(bbox.x, c.x), bbox.y = std::min(bbox.y, c.y);
                bbox.w = bx1 - bbox.x, bbox.h = by1 - bbox.y;
            }
        }
        if (rects.empty()) return false;
        if (!multiRect_) rects.assign(1, bbox);

        std::lock_guard<std::mutex> lock(mu_);
        reapLocked(0);
        Slot* slot = nullptr;
        for (auto& s : slots_)
            if (!s.busy) {
                slot = &s;
                break;
            }
        stats_.framesSubmitted++;
        if (!slot) {
            stats_.framesDropped++;
            return false;
        }
        size_t total = sizeof(kTerminator);
        for (const Rect& r : rects) total += kHeaderLen + size_t(r.w) * 2 * r.h;
        if (slot->buf.size() < total) slot->buf.resize(total);
        uint8_t* p = slot->buf.data();
        for (const Rect& r : rects) {
            putHeader(p, r);
            p += kHeaderLen;
            const uint8_t* src = u.uyvy + size_t(r.y) * u.pitch + size_t(r.x) * 2;
            for (int row = 0; row < r.h; ++row, src += u.pitch, p += r.w * 2) std::memcpy(p, src, size_t(r.w) * 2);
        }
        std::memcpy(p, kTerminator, sizeof(kTerminator));
        const usb0::Api* a = usb0::api();
        if (a->submit_async(slot->ctx, reinterpret_cast<char*>(slot->buf.data()), int(total)) < 0) {
            MSD_ERROR("usb_submit_async failed: %s", a->strerror());
            return false;
        }
        slot->busy = true;
        slot->len = int(total);
        slot->t0 = nowUs();
        return true;
    }

    bool flush(unsigned timeoutMs) override {
        std::lock_guard<std::mutex> lock(mu_);
        return reapLocked(int(timeoutMs));
    }

    DeviceStats stats() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return stats_;
    }

private:
    struct Slot {
        std::vector<uint8_t> buf;
        void* ctx = nullptr;
        bool busy = false;
        int len = 0;
        int64_t t0 = 0;
    };

    // Collects finished transfers; returns false if any is still pending after timeout or failed.
    bool reapLocked(int timeoutMs) {
        const usb0::Api* a = usb0::api();
        bool ok = true;
        for (auto& s : slots_) {
            if (!s.busy) continue;
            int r = a->reap_async_nocancel(s.ctx, timeoutMs);
            if (r == -kEtimedout) {
                if (timeoutMs > 0) ok = false;
                continue;
            }
            s.busy = false;
            if (r < 0) {
                MSD_ERROR("bulk transfer failed (%d): %s", r, a->strerror());
                ok = false;
            } else {
                stats_.bytesSent += uint64_t(r);
                stats_.lastTransferMs = double(nowUs() - s.t0) / 1000.0;
            }
        }
        return ok;
    }

    HidControl hid_;
    usb0::usb_dev_handle* uh_ = nullptr;
    int iface_ = -1;
    Slot slots_[kSlots];
    Mode mode_;
    bool multiRect_ = true;
    DeviceStats stats_;
    mutable std::mutex mu_;
};

}  // namespace

std::unique_ptr<IDevice> createMs912xDevice() { return std::make_unique<Ms912xDevice>(); }

}  // namespace msdisp
