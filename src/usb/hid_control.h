// HID feature-report control channel of the MS912x (interface MI_00, report ID 0, 8 data bytes).
#pragma once
#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace msdisp {

class HidControl {
public:
    ~HidControl() { close(); }
    bool open(uint16_t vid, uint16_t pid, int mi);
    void close();
    bool isOpen() const { return h_ != INVALID_HANDLE_VALUE; }

    // Raw 8-byte request/response.
    bool set(const uint8_t req[8]);
    bool get(uint8_t resp[8]);

    // 0xB5: read up to 4 bytes of xdata at addr.
    bool readXdata(uint16_t addr, uint8_t* out, int count);
    bool readByte(uint16_t addr, uint8_t& out) { return readXdata(addr, &out, 1); }
    // 0xB6: write one byte of xdata.
    bool writeByte(uint16_t addr, uint8_t value);
    // 0xA6: video sub-command with 6 bytes payload.
    bool video(uint8_t subOp, const uint8_t data[6]);

    const std::wstring& path() const { return path_; }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
    std::mutex mu_;
};

}  // namespace msdisp
