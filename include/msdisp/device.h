// msdisp-portrait — MS912x USB display device. FROZEN CONTRACT.
// Owner: track A (src/usb). Control via HID feature reports (MI_00), pixels via libusb0 bulk EP 0x04 (MI_03).
#pragma once
#include <memory>
#include <vector>

#include "msdisp/common.h"

namespace msdisp {

struct DeviceInfo {
    uint8_t chipId[3] = {0, 0, 0};  // read at 0xF000 for MS912x
    int videoPort = -1;             // reg 0x31 (5 = HDMI)
    bool hotplug = false;           // reg 0x32 != 0
};

// One update: UYVY pixels (2 bytes/pixel) for a full output-sized frame buffer, plus the
// rectangles that changed. Rect x and w must be even (UYVY pairs). The device copies what it
// needs before submit() returns, so the caller may reuse the buffer immediately.
struct FrameUpdate {
    const uint8_t* uyvy = nullptr;  // top-left of the full frame
    int pitch = 0;                  // bytes per row
    std::vector<Rect> rects;        // in output coordinates; empty = nothing to send
};

struct DeviceStats {
    uint64_t framesSubmitted = 0;
    uint64_t framesDropped = 0;   // submit() found all transfer slots busy
    uint64_t bytesSent = 0;
    double lastTransferMs = 0;    // duration of the last completed bulk transfer
};

class IDevice {
public:
    virtual ~IDevice() = default;
    virtual bool open() = 0;   // finds 534D:6021, opens HID + libusb0 interface
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool readInfo(DeviceInfo& out) = 0;
    virtual bool readEdid(std::vector<uint8_t>& out) = 0;  // 128 or 256 bytes
    virtual bool start(const Mode& mode) = 0;  // power on + configure mode + enable transfer
    virtual void stop() = 0;                   // disable + power off
    // Non-blocking-ish: queues the update on a free transfer slot; returns false if dropped.
    virtual bool submit(const FrameUpdate& update) = 0;
    // Blocks until all in-flight transfers finished (or timeout). Returns false on timeout/error.
    virtual bool flush(unsigned timeoutMs) = 0;
    virtual DeviceStats stats() const = 0;
};

std::unique_ptr<IDevice> createMs912xDevice();

}  // namespace msdisp
