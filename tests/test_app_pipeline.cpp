// Track D: PipelineController + FakePipeline (worker thread, coalescing, failure reporting).
#include <windows.h>

#include <atomic>
#include <cstdio>

#include "../src/app/core/controller.h"

using namespace msdisp;

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                              \
        }                                                              \
    } while (0)

int main() {
    // Start, metrics, stop.
    {
        std::atomic<int> transitions{0};
        PipelineController c(createFakePipeline(50), [&] { ++transitions; });
        CHECK(!c.status().running);
        Config cfg;
        c.requestStart(cfg);
        CHECK(c.wantRunning());
        CHECK(c.waitIdle(3000));
        PipelineStatus s = c.status();
        CHECK(s.running);
        CHECK(s.fps > 0 && s.fps <= 60.5);
        CHECK(s.mbps > 0);
        CHECK(s.lastError.empty());
        CHECK(!c.lastStartFailed());
        c.requestStop();
        CHECK(c.waitIdle(3000));
        CHECK(!c.status().running);
        CHECK(transitions >= 2);
    }

    // Burst of requests is coalesced and the last config wins.
    {
        std::atomic<int> transitions{0};
        PipelineController c(createFakePipeline(100), [&] { ++transitions; });
        Config cfg;
        for (int i = 0; i < 10; ++i) {
            cfg.fpsCap = 10 + i;
            c.requestStart(cfg);
        }
        CHECK(c.waitIdle(5000));
        CHECK(c.status().running);
        CHECK(transitions < 10);
        CHECK(c.status().fps <= 19.5);  // fpsCap = 19
    }

    // Start failure is reported, next start succeeds.
    {
        PipelineController c(createFakePipeline(0, 1));
        c.requestStart(Config{});
        CHECK(c.waitIdle(3000));
        CHECK(c.lastStartFailed());
        CHECK(!c.status().running);
        CHECK(!c.status().lastError.empty());
        c.requestStart(Config{});
        CHECK(c.waitIdle(3000));
        CHECK(!c.lastStartFailed());
        CHECK(c.status().running);
    }

    // Shutdown stops a running pipeline and is idempotent; requests after it are ignored.
    {
        PipelineController c(createFakePipeline(0));
        c.requestStart(Config{});
        CHECK(c.waitIdle(3000));
        c.shutdown();
        CHECK(!c.status().running);
        c.requestStart(Config{});
        c.shutdown();
        CHECK(!c.status().running);
    }

    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_app_pipeline: OK\n");
    return 0;
}
