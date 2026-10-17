// msdisp-portrait — seam between the app shell (track D) and the real pipeline (integrator).
// Pipeline = virtual monitor -> DXGI capture -> GPU rotate/convert -> USB.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "msdisp/config.h"

namespace msdisp {

struct PipelineStatus {
    bool running = false;
    std::string text;       // short human state, UTF-8, pt-BR ("Transmitindo", "Aguardando adaptador"...)
    double fps = 0;         // frames submitted to the device per second
    double mbps = 0;        // USB payload, megabytes per second
    double latencyMs = 0;   // capture -> USB submit
    uint64_t dropped = 0;   // frames dropped since start()
    std::string lastError;  // UTF-8, empty when healthy
    // GDI name of the monitor the pipeline captures (e.g. L"\\\\.\\DISPLAY3"). When non-empty the shell
    // moves the metrics overlay there (same as calling setOverlayMonitor()).
    std::wstring displayName;
};

// Threading contract:
// - start()/stop() are called from ONE shell worker thread (never the UI thread), never concurrently.
//   They may block for seconds (creating the virtual monitor, opening the device).
// - status() is polled from the UI thread about once per second, concurrently with start()/stop()
//   and with the pipeline's own threads, so it must be thread-safe and must not block.
// - start() on a running pipeline is not issued: the shell always calls stop() first.
// - stop() must be idempotent and must release the USB device before returning.
class IPipeline {
public:
    virtual ~IPipeline() = default;
    virtual bool start(const Config& cfg) = 0;  // false => status().lastError says why
    virtual void stop() = 0;
    virtual PipelineStatus status() = 0;
};

// Factory used by main.cpp. Defined in src/app/pipeline_factory.cpp (the only file the integrator replaces).
std::unique_ptr<IPipeline> createPipeline();

// Pretends to stream: fake metrics, no hardware. Defined in src/app/core/fake_pipeline.cpp.
// `startDelayMs` simulates a slow start(); `failStarts` makes the first N start() calls fail.
std::unique_ptr<IPipeline> createFakePipeline(int startDelayMs = 300, int failStarts = 0);

}  // namespace msdisp
