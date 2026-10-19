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
constexpr int kSlots = 1;
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
        // Phase 2 finding: the MS912x applies only one rectangle per bulk transfer reliably, and it rotates
        // between 2 frame buffers per transfer. Defaults follow the vendor driver: bounding box + 2 buffers + ZLP.
        const char* env = std::getenv("MSDISP_MULTI_RECT");
        multiRect_ = env && *env == '1';
        env = std::getenv("MSDISP_ZLP");
        zlp_ = !(env && *env == '0');
        env = std::getenv("MSDISP_TRANSMODE");
        transMode_ = env ? uint8_t(std::clamp(atoi(env), 0, 5)) : kTransModeManualBlock;
        env = std::getenv("MSDISP_NBUF");
        nbuf_ = env ? std::clamp(atoi(env), 1, kMaxBuf) : 2;
        env = std::getenv("MSDISP_DOUBLE");
        if (env && *env == '0') nbuf_ = 1;
        MSD_INFO("device open: %s iface=%d hid=%ls multiRect=%d transMode=%d nbuf=%d", found->filename, iface_,
                 hid_.path().c_str(), multiRect_, transMode_, nbuf_);
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
        broken_ = false;
        hid_.close();
    }

    // A failed transfer (unplug/replug invalidates the handle) makes the device report closed so the owner
    // reconnects.
    bool isOpen() const override { return uh_ != nullptr && hid_.isOpen() && !broken_; }

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
                  vid(kSubTransMode, {transMode_}) &&
                  vid(kSubInInfo, {uint8_t(m.width >> 8), uint8_t(m.width), uint8_t(m.height >> 8),
                                   uint8_t(m.height), kPixFmtUyvy, 0x00}) &&
                  vid(kSubOutInfo, {m.vic, 0x01, uint8_t(m.width >> 8), uint8_t(m.width), uint8_t(m.height >> 8),
                                    uint8_t(m.height)}) &&
                  vid(kSubTransfer, {0x01}) && vid(kSubVideoEnable, {0x01});
        if (ok) {
            mode_ = m;
            cur_ = 0;  // which physical buffer comes first doesn't matter: every buffer gets all damage
            for (auto& p : pending_) p.reset(m.width, m.height);
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
        std::lock_guard<std::mutex> lock(mu_);
        // One transfer at a time: the chip double-buffers and flips on each transfer end (ZLP), so ordering
        // data -> ZLP -> next data must be strict.
        if (!reapLocked(2000)) return false;
        for (const Rect& r : u.rects)
            for (int b = 0; b < nbuf_; ++b) pending_[b].mark(r);
        std::vector<Rect> rects = pending_[cur_].extract(multiRect_ ? kMaxRectsPerTransfer : 1);
        stats_.framesSubmitted++;
        if (rects.empty()) return true;

        Slot& slot = slots_[0];
        size_t total = sizeof(kTerminator);
        for (const Rect& r : rects) total += kHeaderLen + size_t(r.w) * 2 * r.h;
        if (slot.buf.size() < total) slot.buf.resize(total);
        uint8_t* p = slot.buf.data();
        for (const Rect& r : rects) {
            putHeader(p, r);
            p += kHeaderLen;
            const uint8_t* src = u.uyvy + size_t(r.y) * u.pitch + size_t(r.x) * 2;
            for (int row = 0; row < r.h; ++row, src += u.pitch, p += r.w * 2) std::memcpy(p, src, size_t(r.w) * 2);
        }
        std::memcpy(p, kTerminator, sizeof(kTerminator));
        const usb0::Api* a = usb0::api();
        if (a->submit_async(slot.ctx, reinterpret_cast<char*>(slot.buf.data()), int(total)) < 0) {
            MSD_ERROR("usb_submit_async failed: %s", a->strerror());
            broken_ = true;
            return false;
        }
        slot.busy = true;
        slot.len = int(total);
        slot.t0 = nowUs();
        pending_[cur_].clear();
        cur_ = (cur_ + 1) % nbuf_;
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
    static constexpr int kMaxRectsPerTransfer = 32;
    static constexpr int kMaxBuf = 4;
    // Grid granularity matches the official driver's alignment (x/w multiple of 4, y/h multiple of 2).
    static constexpr int kCellW = 16, kCellH = 2;

    // Per-buffer accumulated damage on a coarse grid.
    struct Damage {
        int cols = 0, rows = 0, w = 0, h = 0;
        std::vector<uint8_t> cells;
        bool any = false;
        void reset(int width, int height) {
            w = width, h = height;
            cols = (width + kCellW - 1) / kCellW, rows = (height + kCellH - 1) / kCellH;
            cells.assign(size_t(cols) * rows, 0);
            any = false;
        }
        void clear() {
            if (any) std::fill(cells.begin(), cells.end(), 0);
            any = false;
        }
        void mark(const Rect& r) {
            int c0 = std::clamp(r.x / kCellW, 0, cols), c1 = std::clamp((r.right() + kCellW - 1) / kCellW, 0, cols);
            int r0 = std::clamp(r.y / kCellH, 0, rows), r1 = std::clamp((r.bottom() + kCellH - 1) / kCellH, 0, rows);
            for (int y = r0; y < r1; ++y) std::fill(&cells[size_t(y) * cols + c0], &cells[size_t(y) * cols + c1], 1);
            if (c1 > c0 && r1 > r0) any = true;
        }
        // Horizontal runs per grid row, merged vertically when the span repeats; falls back to the bounding box
        // when that would exceed maxRects.
        std::vector<Rect> extract(int maxRects) const {
            std::vector<Rect> out, open, next;
            if (!any) return out;
            Rect bb{w, h, 0, 0};
            int bx1 = 0, by1 = 0;
            for (int y = 0; y < rows; ++y) {
                next.clear();
                const uint8_t* row = &cells[size_t(y) * cols];
                size_t oi = 0;
                for (int x = 0; x < cols;) {
                    if (!row[x]) { ++x; continue; }
                    int x1 = x;
                    while (x1 < cols && row[x1]) ++x1;
                    Rect r{x * kCellW, y * kCellH, std::min(x1 * kCellW, w) - x * kCellW,
                           std::min((y + 1) * kCellH, h) - y * kCellH};
                    bb.x = std::min(bb.x, r.x), bb.y = std::min(bb.y, r.y);
                    bx1 = std::max(bx1, r.right()), by1 = std::max(by1, r.bottom());
                    x = x1;
                    while (oi < open.size() && open[oi].x < r.x) out.push_back(open[oi++]);
                    if (oi < open.size() && open[oi].x == r.x && open[oi].w == r.w) {
                        Rect m = open[oi++];
                        m.h = r.bottom() - m.y;
                        next.push_back(m);
                    } else {
                        next.push_back(r);
                    }
                }
                while (oi < open.size()) out.push_back(open[oi++]);
                open.swap(next);
            }
            out.insert(out.end(), open.begin(), open.end());
            if (int(out.size()) > maxRects) out.assign(1, Rect{bb.x, bb.y, bx1 - bb.x, by1 - bb.y});
            return out;
        }
    };

    struct Slot {
        std::vector<uint8_t> buf;
        void* ctx = nullptr;
        bool busy = false;
        int len = 0;
        int64_t t0 = 0;
    };

    // Waits for the in-flight transfer, then sends the zero-length packet that ends it (the chip flips buffers
    // on it). Returns false on timeout or failure.
    bool reapLocked(int timeoutMs) {
        const usb0::Api* a = usb0::api();
        Slot& s = slots_[0];
        if (!s.busy) return true;
        int r = a->reap_async_nocancel(s.ctx, timeoutMs);
        if (r == -kEtimedout) return timeoutMs == 0;
        s.busy = false;
        if (r < 0) {
            MSD_ERROR("bulk transfer failed (%d): %s", r, a->strerror());
            broken_ = true;
            return false;
        }
        if (zlp_) {
            char dummy = 0;
            if (a->bulk_write(uh_, kBulkEp, &dummy, 0, 1000) < 0) {
                MSD_WARN("zero-length packet not supported (%s); continuing without", a->strerror());
                zlp_ = false;
            }
        }
        stats_.bytesSent += uint64_t(r);
        stats_.lastTransferMs = double(nowUs() - s.t0) / 1000.0;
        return true;
    }

    Damage pending_[kMaxBuf];
    int cur_ = 0;
    bool zlp_ = true;
    bool broken_ = false;
    uint8_t transMode_ = kTransModeManualBlock;  // 0 frame, 3 manual block, 4 bypass frame, 5 bypass manual block
    int nbuf_ = 2;  // chip frame buffers in rotation: damage is replayed into each of them
    HidControl hid_;
    usb0::usb_dev_handle* uh_ = nullptr;
    int iface_ = -1;
    Slot slots_[kSlots];
    Mode mode_;
    bool multiRect_ = false;
    DeviceStats stats_;
    mutable std::mutex mu_;
};

}  // namespace

std::unique_ptr<IDevice> createMs912xDevice() { return std::make_unique<Ms912xDevice>(); }

}  // namespace msdisp
