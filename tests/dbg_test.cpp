#include <gtest/gtest.h>
#include "test_util.h"
namespace tbox { namespace time { namespace testutil {
using namespace tbox::time;
constexpr int64_t kUtc = 1'700'000'000'000;
TimeCoreConfig sc() {
    TimeCoreConfig c;
    c.max_source_age_ms = 300000; c.max_source_error_ms = 500;
    c.step_threshold_ms = 1000; c.slew_threshold_ms = 50;
    c.alignment_tolerance_ms = 50; c.calibration_retry_limit = 5;
    c.holdover_max_ms = 60000; c.holdover_max_error_ms = 1000;
    c.drift_bound_ppm = 100; c.external_change_threshold_ms = 200;
    c.fault_probe_interval_ms = 5000; c.synchronizing_timeout_ms = 30000;
    c.source_lost_age_ms = 250000;
    return c;
}
TEST(Dbg, StepFailure) {
    Harness h(kUtc, sc());
    h.backend.ctrl.fail_step = true;
    h.inject(goodSample("ntp0", kUtc + 5000, h.boot));
    for (int i = 0; i < 8; ++i) {
        h.tick();
        printf("tick %d: state=%d\n", i, (int)h.st());
    }
}
}}}
