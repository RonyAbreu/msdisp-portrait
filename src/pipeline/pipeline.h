// Real pipeline: Parsec VDD portrait monitor -> DXGI capture -> GPU convert -> MS912x.
// Integrator-owned. Loop is serialized on purpose: wait for USB idle, then capture the newest frame,
// so no frame queues behind a slow transfer (Phase 0 finding: 3 queued full frames = ~530 ms lag)
// and no dirty area is ever lost (every converted frame is submitted).
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "msdisp/capture.h"
#include "msdisp/config.h"
#include "msdisp/device.h"
#include "msdisp/gpu.h"
#include "msdisp/vdd.h"

namespace msdisp {

struct RunStatus {
    bool running = false;
    std::string state;      // human readable (pt-BR)
    double fps = 0;         // submitted updates per second
    double mbps = 0;        // USB payload MB/s
    double latencyMs = 0;   // capture timestamp -> transfer completed (avg)
    uint64_t dropped = 0;
    std::wstring virtualDisplay;  // \\.\DISPLAYn of the portrait monitor
    std::string lastError;
};

class Pipeline {
public:
    ~Pipeline() { stop(); }
    bool start(const Config& cfg);
    void stop();
    RunStatus status();

private:
    void threadMain();
    bool setupVirtualDisplay();
    bool setupCapture();
    void setState(const std::string& s, bool error = false);

    Config cfg_;
    std::unique_ptr<IDevice> dev_;
    std::unique_ptr<IVirtualDisplay> vdd_;
    std::unique_ptr<ICapture> cap_;
    std::unique_ptr<IConverter> conv_;
    int vddIndex_ = -1;
    std::thread th_;
    std::atomic<bool> quit_{false};
    std::mutex mu_;
    RunStatus st_;
};

}  // namespace msdisp
