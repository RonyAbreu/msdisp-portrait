#include "pipeline.h"

#include <windows.h>
#include <timeapi.h>

#include <algorithm>

#include "msdisp/log.h"

namespace msdisp {

namespace {
// Rolling averages over ~1 s windows.
struct Meter {
    int64_t windowStart = 0;
    int frames = 0;
    uint64_t bytesAtStart = 0;
    double latSum = 0;
    int latN = 0;
};
}  // namespace

bool Pipeline::start(const Config& cfg) {
    stop();
    cfg_ = cfg;
    // The virtual monitor must be the chip mode rotated back: portrait for 90/270, landscape for 0/180.
    const bool swap = cfg.rotation == Rotation::CW90 || cfg.rotation == Rotation::CW270;
    cfg_.virtualWidth = swap ? cfg.mode.height : cfg.mode.width;
    cfg_.virtualHeight = swap ? cfg.mode.width : cfg.mode.height;
    quit_ = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        st_ = RunStatus{};
        st_.running = true;
        st_.state = "iniciando";
    }
    th_ = std::thread([this] { threadMain(); });
    return true;
}

void Pipeline::stop() {
    quit_ = true;
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> lock(mu_);
    st_.running = false;
    st_.state = "parado";
}

RunStatus Pipeline::status() {
    std::lock_guard<std::mutex> lock(mu_);
    return st_;
}

void Pipeline::setState(const std::string& s, bool error) {
    std::lock_guard<std::mutex> lock(mu_);
    st_.state = s;
    if (error) st_.lastError = s;
    (error ? MSD_ERROR("pipeline: %s", s.c_str()) : MSD_INFO("pipeline: %s", s.c_str()));
}

bool Pipeline::setupVirtualDisplay() {
    vdd_ = createParsecVdd();
    if (!vdd_ || !vdd_->open()) {
        setState("Parsec Virtual Display indisponível", true);
        return false;
    }
    vddIndex_ = vdd_->add();
    if (vddIndex_ < 0) {
        setState("falha ao criar monitor virtual", true);
        return false;
    }
    std::wstring gdi;
    for (int i = 0; i < 50 && gdi.empty() && !quit_; ++i) {
        gdi = vdd_->gdiName(vddIndex_);
        if (gdi.empty()) Sleep(100);
    }
    if (gdi.empty()) {
        setState("monitor virtual não apareceu no Windows", true);
        return false;
    }
    if (!setDisplayMode(gdi, cfg_.virtualWidth, cfg_.virtualHeight, 60))
        MSD_WARN("could not set %dx%d on %ls; using current mode", cfg_.virtualWidth, cfg_.virtualHeight, gdi.c_str());
    parked_ = detachVendorOutputs();
    placeOutput(gdi, cfg_.virtualWidth, parked_.empty() ? nullptr : &parked_[0]);
    std::lock_guard<std::mutex> lock(mu_);
    st_.virtualDisplay = gdi;
    return true;
}

bool Pipeline::setupCapture() {
    std::wstring gdi;
    {
        std::lock_guard<std::mutex> lock(mu_);
        gdi = st_.virtualDisplay;
    }
    cap_ = createDxgiCapture();
    conv_ = createD3D11Converter();
    if (!cap_ || !conv_ || !cap_->init(gdi)) {
        setState("falha ao iniciar captura", true);
        return false;
    }
    if (!conv_->init(cap_->device(), cap_->context(), cfg_.mode.width, cfg_.mode.height, cfg_.tileW, cfg_.tileH)) {
        setState("falha ao iniciar conversor GPU", true);
        return false;
    }
    return true;
}

void Pipeline::threadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    timeBeginPeriod(1);  // Sleep() granularity 1 ms for frame pacing
    dev_ = createMs912xDevice();
    while (!quit_) {
        // (Re)connect device + displays.
        if (!dev_->isOpen()) {
            if (!dev_->open() || !dev_->start(cfg_.mode)) {
                setState("adaptador USB não encontrado ou ocupado; tentando de novo", true);
                dev_->close();
                for (int i = 0; i < 20 && !quit_; ++i) Sleep(100);
                continue;
            }
        }
        if (!vdd_ && !setupVirtualDisplay()) {
            vdd_.reset();
            for (int i = 0; i < 30 && !quit_; ++i) Sleep(100);
            continue;
        }
        if (!cap_ && !setupCapture()) {
            cap_.reset();
            conv_.reset();
            for (int i = 0; i < 10 && !quit_; ++i) Sleep(100);
            continue;
        }
        setState("rodando");

        bool forceFull = true;
        Meter m;
        m.windowStart = nowUs();
        m.bytesAtStart = dev_->stats().bytesSent;
        int64_t lastCaptureUs = 0, lastSubmitUs = 0;
        int logTick = 0;
        const int64_t minIntervalUs = cfg_.fpsCap > 0 ? 1000000 / cfg_.fpsCap : 0;

        while (!quit_) {
            // 1. Wait until the USB link is idle so we always capture the newest frame.
            if (!dev_->flush(2000)) {
                setState("transferência USB falhou; reconectando", true);
                dev_->close();
                break;
            }
            if (lastCaptureUs) {
                m.latSum += double(nowUs() - lastCaptureUs) / 1000.0;
                m.latN++;
                lastCaptureUs = 0;
            }
            if (minIntervalUs) {
                int64_t wait = lastSubmitUs + minIntervalUs - nowUs();
                if (wait > 1000) Sleep(DWORD(wait / 1000));
            }

            // 2. Capture.
            CapturedFrame f;
            AcquireResult r = cap_->acquire(f, 50);
            if (r == AcquireResult::Timeout) goto meter;
            if (r != AcquireResult::Frame) {
                setState("captura perdida; reiniciando captura", r == AcquireResult::Error);
                cap_.reset();
                conv_.reset();
                break;
            }

            // 3. Convert + 4. submit.
            {
                ConvertResult cr;
                bool ok = conv_->process(f.texture, cfg_.rotation, f.full ? std::vector<Rect>{} : f.dirty,
                                         forceFull, cr);
                if (!ok) {
                    cap_->release();
                    setState("tamanho do monitor virtual não bate com a rotação/modo", true);
                    for (int i = 0; i < 20 && !quit_; ++i) Sleep(100);
                    cap_.reset();
                    conv_.reset();
                    break;
                }
                if (!cr.dirty.empty()) {
                    FrameUpdate up{cr.uyvy, cr.pitch, cr.dirty};
                    if (dev_->submit(up)) {
                        forceFull = false;
                        lastCaptureUs = f.timeUs;
                        lastSubmitUs = nowUs();
                        m.frames++;
                    }
                }
                conv_->done();
                cap_->release();
            }

        meter:
            int64_t now = nowUs();
            if (now - m.windowStart >= 1000000) {
                DeviceStats ds = dev_->stats();
                double secs = double(now - m.windowStart) / 1e6;
                std::lock_guard<std::mutex> lock(mu_);
                st_.fps = m.frames / secs;
                st_.mbps = double(ds.bytesSent - m.bytesAtStart) / secs / 1e6;
                st_.latencyMs = m.latN ? m.latSum / m.latN : 0;
                st_.dropped = ds.framesDropped;
                if (++logTick % 30 == 0)
                    MSD_INFO("stats: %.1f fps, %.2f MB/s, latency %.1f ms, last transfer %.1f ms", st_.fps, st_.mbps,
                             st_.latencyMs, ds.lastTransferMs);
                m = Meter{};
                m.windowStart = now;
                m.bytesAtStart = ds.bytesSent;
            }
        }
    }

    // Teardown in reverse order.
    conv_.reset();
    if (cap_) cap_->shutdown();
    cap_.reset();
    if (vdd_) {
        if (vddIndex_ >= 0) vdd_->remove(vddIndex_);
        vdd_->close();
    }
    vdd_.reset();
    vddIndex_ = -1;
    restoreOutputs(parked_);
    parked_.clear();
    if (dev_ && dev_->isOpen()) {
        dev_->stop();
        dev_->close();
    }
    dev_.reset();
    timeEndPeriod(1);
}

}  // namespace msdisp
