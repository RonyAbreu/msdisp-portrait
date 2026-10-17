// portrait-run: headless runner for the real pipeline (integration testing).
//   portrait-run [--rot 0|90|180|270] [--720] [--sec N]
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "../../src/pipeline/pipeline.h"
#include "msdisp/log.h"

using namespace msdisp;

static std::atomic<bool> g_quit{false};
static BOOL WINAPI onCtrl(DWORD) {
    g_quit = true;
    return TRUE;
}

int main(int argc, char** argv) {
    Config cfg;
    int sec = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--rot") && i + 1 < argc) cfg.rotation = Rotation(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--720")) cfg.mode = Mode{1280, 720, 60, 0x4f};
        else if (!strcmp(argv[i], "--sec") && i + 1 < argc) sec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--debug")) logSetLevel(LogLevel::Debug);
    }
    if (cfg.mode.width == 1280) cfg.virtualWidth = 720, cfg.virtualHeight = 1280;
    SetConsoleCtrlHandler(onCtrl, TRUE);
    Pipeline p;
    p.start(cfg);
    int64_t end = sec > 0 ? nowUs() + int64_t(sec) * 1000000 : 0;
    while (!g_quit && (!end || nowUs() < end)) {
        Sleep(1000);
        RunStatus s = p.status();
        printf("[%ls] %s | %.1f fps %.1f MB/s lat %.1f ms dropped %llu\n", s.virtualDisplay.c_str(), s.state.c_str(),
               s.fps, s.mbps, s.latencyMs, (unsigned long long)s.dropped);
        fflush(stdout);
    }
    p.stop();
    return 0;
}
