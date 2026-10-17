#include "controller.h"

#include <chrono>

#include "msdisp/log.h"

namespace msdisp {

PipelineController::PipelineController(std::unique_ptr<IPipeline> pipeline, std::function<void()> onTransition)
    : pipeline_(std::move(pipeline)), onTransition_(std::move(onTransition)) {
    thread_ = std::thread([this] { worker(); });
}

PipelineController::~PipelineController() { shutdown(); }

void PipelineController::requestStart(const Config& cfg) {
    std::lock_guard<std::mutex> lock(mu_);
    if (quit_) return;
    cfg_ = cfg;
    want_ = true;
    ++requested_;
    cv_.notify_all();
}

void PipelineController::requestStop() {
    std::lock_guard<std::mutex> lock(mu_);
    if (quit_) return;
    want_ = false;
    ++requested_;
    cv_.notify_all();
}

void PipelineController::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        quit_ = true;
        cv_.notify_all();
    }
    if (thread_.joinable()) thread_.join();
}

bool PipelineController::wantRunning() const {
    std::lock_guard<std::mutex> lock(mu_);
    return want_;
}

bool PipelineController::busy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return executing_ || requested_ != done_;
}

bool PipelineController::lastStartFailed() const {
    std::lock_guard<std::mutex> lock(mu_);
    return startFailed_;
}

bool PipelineController::waitIdle(int timeoutMs) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                        [this] { return quit_ || (!executing_ && requested_ == done_); });
}

PipelineStatus PipelineController::status() { return pipeline_->status(); }

void PipelineController::worker() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        cv_.wait(lock, [this] { return quit_ || requested_ != done_; });
        if (quit_) break;
        uint64_t gen = requested_;
        bool want = want_;
        Config cfg = cfg_;
        executing_ = true;
        lock.unlock();

        pipeline_->stop();
        bool failed = false;
        if (want) {
            failed = !pipeline_->start(cfg);
            if (failed) MSD_WARN("pipeline: start failed: %s", pipeline_->status().lastError.c_str());
        }

        lock.lock();
        executing_ = false;
        startFailed_ = failed;
        done_ = gen;
        cv_.notify_all();
        if (onTransition_) {
            lock.unlock();
            onTransition_();
            lock.lock();
        }
    }
    executing_ = true;
    lock.unlock();
    pipeline_->stop();
    lock.lock();
    executing_ = false;
    cv_.notify_all();
}

}  // namespace msdisp
