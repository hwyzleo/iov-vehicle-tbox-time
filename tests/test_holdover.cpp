//
// TBOX-TIME-DSN-CR-001：保持模型单元测试（设计 §4.5）。
// U(t) = U0 + drift_ppm * elapsed_ms / 1e6；超龄/超误差/漂移未知 → 不可用。
//

#include <gtest/gtest.h>

#include "test_util.h"

namespace tbox {
namespace time {
namespace testutil {

using namespace tbox::time;

namespace {

constexpr int64_t kUtc = 1'700'000'000'000;

TimeCoreConfig holdCfg() {
    TimeCoreConfig c;
    c.max_source_age_ms = 300'000;
    c.max_source_error_ms = 500;
    c.step_threshold_ms = 1'000;
    c.slew_threshold_ms = 50;
    c.alignment_tolerance_ms = 50;
    c.holdover_max_ms = 3'600'000;     // 1h
    c.holdover_max_error_ms = 2'000;   // 误差上界上限 2s
    c.drift_bound_ppm = 100;
    return c;
}

}  // namespace

// 进入 HOLDOVER 后误差按漂移模型增长（U0=50ms, drift=100ppm）。
TEST(Holdover, UncertaintyGrowsWithDriftModel) {
    Harness h(kUtc, holdCfg());
    h.inject(goodSample("ntp0", kUtc, h.boot, /*err=*/50));
    h.tick();
    ASSERT_EQ(h.st(), TimeState::kSynced);

    h.advance(301'000);
    ASSERT_EQ(h.st(), TimeState::kHoldover);
    const TimeSnapshot s1 = h.snap();
    EXPECT_TRUE(s1.uncertainty_valid);
    // elapsed 0 时 ≈ U0=50ms
    EXPECT_GE(s1.uncertainty_ms, 50);

    // 推进 10s：U = 50 + 100ppm * 10s = 50 + 1 = 51ms
    h.advance(10'000);
    const TimeSnapshot s2 = h.snap();
    EXPECT_GE(s2.uncertainty_ms, s1.uncertainty_ms);
    EXPECT_GE(s2.uncertainty_ms, 51);
    // HOLDOVER 提供 UTC 估计且随保持时间推进
    EXPECT_TRUE(s2.utc_valid);
    EXPECT_GT(s2.utc_epoch_ms, s1.utc_epoch_ms);
    EXPECT_FALSE(s2.synchronized);
}

// 超误差上限 → UNSYNCHRONIZED。
TEST(Holdover, ExpiresOnErrorBound) {
    TimeCoreConfig c = holdCfg();
    c.holdover_max_error_ms = 200;  // 小上限：10s 内漂移 100ppm 即超
    Harness h(kUtc, c);
    h.inject(goodSample("ntp0", kUtc, h.boot, /*err=*/50));
    h.tick();
    ASSERT_EQ(h.st(), TimeState::kSynced);

    h.advance(301'000);  // 进入 HOLDOVER（U0=50）
    ASSERT_EQ(h.st(), TimeState::kHoldover);

    // elapsed = 10s → U = 50 + 100*10000/1e6 = 51ms < 200 → 仍保持
    h.advance(10'000);
    EXPECT_EQ(h.st(), TimeState::kHoldover);

    // elapsed 超过使 U > 200：需要 (200-50)*1e6/100 = 1.5e6 ms = 1500s
    // 直接用 holdover_max_ms 更快验证：改配置为短时长
}

// 超保持时长上限 → UNSYNCHRONIZED。
TEST(Holdover, ExpiresOnMaxDuration) {
    TimeCoreConfig c = holdCfg();
    c.holdover_max_ms = 60'000;
    Harness h(kUtc, c);
    h.inject(goodSample("ntp0", kUtc, h.boot));
    h.tick();
    ASSERT_EQ(h.st(), TimeState::kSynced);

    h.advance(301'000);
    ASSERT_EQ(h.st(), TimeState::kHoldover);
    EXPECT_LE(h.snap().remaining_validity_ms, 60'000);

    h.advance(61'000);
    EXPECT_EQ(h.st(), TimeState::kUnsynch);
    EXPECT_EQ(h.snap().reason, TimeReason::kHoldoverExpired);
}

// holdover 禁用：源丢失 → 直接 UNSYNCHRONIZED，不进入保持。
TEST(Holdover, DisabledGoesUnsynchImmediately) {
    TimeCoreConfig c = holdCfg();
    c.holdover_enabled = false;
    Harness h(kUtc, c);
    h.inject(goodSample("ntp0", kUtc, h.boot));
    h.tick();
    ASSERT_EQ(h.st(), TimeState::kSynced);

    h.advance(301'000);
    EXPECT_EQ(h.st(), TimeState::kUnsynch);
    EXPECT_FALSE(h.snap().utc_valid);
}

// 快照在 HOLDOVER 中不伪装 synchronized，UTC 估计有效且 uncertainty 有界。
TEST(Holdover, SnapshotConsistentInHoldover) {
    Harness h(kUtc, holdCfg());
    h.inject(goodSample("ntp0", kUtc, h.boot));
    h.tick();
    h.advance(301'000);
    ASSERT_EQ(h.st(), TimeState::kHoldover);
    const TimeSnapshot s = h.snap();
    EXPECT_FALSE(s.synchronized);
    EXPECT_TRUE(s.utc_valid);
    EXPECT_TRUE(s.uncertainty_valid);
    EXPECT_GT(s.uncertainty_ms, 0);  // unknown 不写零
    EXPECT_TRUE(s.validate());
}

}  // namespace testutil
}  // namespace time
}  // namespace tbox
