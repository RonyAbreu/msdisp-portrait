// msdisp-portrait — runs IPipeline start/stop on a worker thread so the tray never freezes. Track D.
#pragma once
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "pipeline.h"

namespace msdisp {

class PipelineController {
public:
    // `onTransition` is called on the worker thread after each start/stop finishes (e.g. PostMessage to the UI).
    PipelineController(std::unique_ptr<IPipeline> pipeline, std::function<void()> onTransition = {});
    ~PipelineController();
    PipelineController(const PipelineController&) = delete;
    PipelineController& operator=(const PipelineController&) = delete;

    // Requests are coalesced: only the latest one is executed. Every (re)start is stop() + start(cfg).
    void requestStart(const Config& cfg);
    void requestStop();
    // Stops the pipeline and joins the worker. Idempotent; called by the destructor.
    void shutdown();

    bool wantRunning() const;  // latest request was a start
    bool busy() const;         // a request is pending or executing
    bool lastStartFailed() const;
    bool waitIdle(int timeoutMs);  // true once no request is pending/executing
    PipelineStatus status();

private:
    void worker();

    std::unique_ptr<IPipeline> pipeline_;
    std::function<void()> onTransition_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    Config cfg_;
    bool want_ = false;
    bool quit_ = false;
    bool executing_ = false;
    bool startFailed_ = false;
    uint64_t requested_ = 0, done_ = 0;
};

}  // namespace msdisp
