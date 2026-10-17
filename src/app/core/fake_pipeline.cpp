// FakePipeline: no hardware, plausible metrics. Lets the shell be run and tested before integration.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <mutex>

#include "msdisp/log.h"
#include "pipeline.h"

namespace msdisp {

namespace {

class FakePipeline final : public IPipeline {
public:
    FakePipeline(int startDelayMs, int failStarts) : startDelayMs_(startDelayMs), failStarts_(failStarts) {}

    bool start(const Config& cfg) override {
        if (startDelayMs_ > 0) Sleep(DWORD(startDelayMs_));
        std::lock_guard<std::mutex> lock(mu_);
        if (failStarts_ > 0) {
            --failStarts_;
            running_ = false;
            lastError_ = "adaptador não encontrado (simulado)";
            MSD_WARN("fake pipeline: simulated start failure");
            return false;
        }
        cfg_ = cfg;
        running_ = true;
        lastError_.clear();
        startUs_ = nowUs();
        MSD_INFO("fake pipeline: start %dx%d@%d rot=%d virtual=%dx%d", cfg.mode.width, cfg.mode.height, cfg.mode.hz,
                 int(cfg.rotation), cfg.virtualWidth, cfg.virtualHeight);
        return true;
    }

    void stop() override {
        std::lock_guard<std::mutex> lock(mu_);
        if (running_) MSD_INFO("fake pipeline: stop");
        running_ = false;
    }

    PipelineStatus status() override {
        std::lock_guard<std::mutex> lock(mu_);
        PipelineStatus s;
        s.running = running_;
        s.lastError = lastError_;
        if (!running_) {
            s.text = lastError_.empty() ? "Parado" : "Erro";
            return s;
        }
        double t = double(nowUs() - startUs_) / 1e6;
        double cap = cfg_.fpsCap < cfg_.mode.hz ? cfg_.fpsCap : cfg_.mode.hz;
        s.fps = cap * (0.95 + 0.04 * std::sin(t * 0.7));
        // UYVY = 2 bytes/pixel; assume ~25% of the frame changes on average.
        s.mbps = s.fps * cfg_.mode.width * cfg_.mode.height * 2.0 * 0.25 / 1e6;
        if (s.mbps > 38) s.mbps = 38 + std::sin(t) * 0.5;  // USB 2.0 practical ceiling
        s.latencyMs = 14 + 3 * std::sin(t * 1.3);
        s.dropped = uint64_t(t / 7);
        s.text = "Transmitindo (simulado)";
        return s;
    }

private:
    std::mutex mu_;
    Config cfg_;
    bool running_ = false;
    std::string lastError_;
    int64_t startUs_ = 0;
    int startDelayMs_;
    int failStarts_;
};

}  // namespace

std::unique_ptr<IPipeline> createFakePipeline(int startDelayMs, int failStarts) {
    return std::make_unique<FakePipeline>(startDelayMs, failStarts);
}

}  // namespace msdisp
