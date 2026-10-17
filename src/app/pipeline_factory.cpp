// Real pipeline (VDD -> DXGI capture -> GPU -> MS912x) behind the shell's IPipeline seam.
#include "../pipeline/pipeline.h"
#include "core/pipeline.h"

namespace msdisp {

namespace {
class RealPipeline final : public IPipeline {
public:
    bool start(const Config& cfg) override { return p_.start(cfg); }
    void stop() override { p_.stop(); }
    PipelineStatus status() override {
        RunStatus r = p_.status();
        PipelineStatus s;
        s.running = r.running;
        s.text = r.state;
        s.fps = r.fps;
        s.mbps = r.mbps;
        s.latencyMs = r.latencyMs;
        s.dropped = r.dropped;
        s.lastError = r.lastError;
        s.displayName = r.virtualDisplay;
        return s;
    }

private:
    Pipeline p_;
};
}  // namespace

std::unique_ptr<IPipeline> createPipeline() { return std::make_unique<RealPipeline>(); }

}  // namespace msdisp
