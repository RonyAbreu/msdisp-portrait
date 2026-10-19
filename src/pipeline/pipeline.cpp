#include "pipeline.h"

#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <vector>

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

// 30 s accumulation for the log: what the user actually experiences while things move.
struct Window30 {
    int64_t start = 0;
    int updates = 0, activeSecs = 0, secs = 0;
    uint64_t bytes = 0, maxBytes = 0;
    int64_t usbBusyUs = 0, convertUs = 0;
    std::vector<double> lat;
};

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    size_t k = std::min(v.size() - 1, size_t(p * double(v.size())));
    std::nth_element(v.begin(), v.begin() + ptrdiff_t(k), v.end());
    return v[k];
}
}  // namespace

bool Pipeline::start(const Config& cfg) {
    stop();
    cfg_ = cfg;
    // The virtual monitor is the chip mode rotated back (portrait for 90/270, landscape for 0/180), so pixels map
    // 1:1 and text stays crisp in the smaller modes (user preference over a 1080p desktop downscaled by the GPU,
    // which the converter still supports). Needs the matching Parsec custom modes: 1080x1920, 768x1366, 720x1280.
    // (Tested: overlapping capture/convert with the USB transfer gave no fps and +10 ms latency, so the loop
    // stays serialized.)
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
        // Displays first, chip last: once the chip is started it shows garbage until it receives a frame.
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
        if (!dev_->isOpen()) {
            if (!dev_->open() || !dev_->start(cfg_.mode)) {
                setState("adaptador USB não encontrado ou ocupado; tentando de novo", true);
                dev_->close();
                for (int i = 0; i < 20 && !quit_; ++i) Sleep(100);
                continue;
            }
        }
        setState("rodando");

        bool forceFull = true;
        Meter m;
        m.windowStart = nowUs();
        m.bytesAtStart = dev_->stats().bytesSent;
        int64_t lastCaptureUs = 0, lastSubmitUs = 0;
        Window30 w;
        w.start = nowUs();
        uint64_t bytesPrev = dev_->stats().bytesSent;
        const int64_t minIntervalUs = cfg_.fpsCap > 0 ? 1000000 / cfg_.fpsCap : 0;

        while (!quit_) {
            // 1. Wait until the USB link is idle so we always capture the newest frame.
            const int64_t flushStart = nowUs();
            const bool flushed = dev_->flush(2000);
            if (lastCaptureUs) w.usbBusyUs += nowUs() - flushStart;
            if (!flushed) {
                setState("transferência USB falhou; reconectando", true);
                dev_->close();
                // Restart capture too: its first frame is always full, which repaints the re-powered chip.
                cap_.reset();
                conv_.reset();
                break;
            }
            if (lastCaptureUs) {
                const double lat = double(nowUs() - lastCaptureUs) / 1000.0;
                m.latSum += lat;
                m.latN++;
                w.lat.push_back(lat);
                lastCaptureUs = 0;
                const uint64_t b = dev_->stats().bytesSent;
                w.bytes += b - bytesPrev;
                w.maxBytes = std::max<uint64_t>(w.maxBytes, b - bytesPrev);
                bytesPrev = b;
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
                const int64_t convStart = nowUs();
                bool ok = conv_->process(f.texture, cfg_.rotation, f.full ? std::vector<Rect>{} : f.dirty,
                                         forceFull, cr);
                w.convertUs += nowUs() - convStart;
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
                        // The chip rotates two frame buffers: a full repaint must reach both, now.
                        if (forceFull && dev_->flush(2000)) dev_->submit(up);
                        forceFull = false;
                        lastCaptureUs = f.timeUs;
                        lastSubmitUs = nowUs();
                        m.frames++;
                        w.updates++;
                    }
                }
                conv_->done();
                cap_->release();
                if (!dev_->isOpen()) {
                    setState("adaptador desconectado; reconectando", true);
                    dev_->close();
                    cap_.reset();
                    conv_.reset();
                    break;
                }
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
                w.secs++;
                if (m.frames > 0) w.activeSecs++;
                if (now - w.start >= 30000000) {
                    if (w.updates > 0) {
                        const double act = std::max(1, w.activeSecs);
                        double latAvg = 0;
                        for (double x : w.lat) latAvg += x;
                        if (!w.lat.empty()) latAvg /= double(w.lat.size());
                        const double wall = double(now - w.start) / 1e6;
                        MSD_INFO("stats30s: active %d/%d s | %.1f fps while active | %.0f KB/update (max %.0f) | "
                                 "%.1f MB/s while active | USB busy %.0f%% | latency avg %.1f p95 %.1f max %.1f ms | "
                                 "convert %.2f ms",
                                 w.activeSecs, w.secs, w.updates / act, double(w.bytes) / w.updates / 1024.0,
                                 double(w.maxBytes) / 1024.0, double(w.bytes) / act / 1e6,
                                 100.0 * double(w.usbBusyUs) / 1e6 / wall, latAvg, percentile(w.lat, 0.95),
                                 w.lat.empty() ? 0.0 : *std::max_element(w.lat.begin(), w.lat.end()),
                                 double(w.convertUs) / 1000.0 / w.updates);
                    }
                    w = Window30{};
                    w.start = now;
                }
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
