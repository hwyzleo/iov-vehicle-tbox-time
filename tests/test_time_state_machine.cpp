//
// TBOX-TIME-DSN-CR-001：状态机单元测试。
// 覆盖 AT-01（无源冷启动）、AT-02（step/slew/写失败/权限）、AT-05（保持）、
// AT-06（回拨/大偏差/旧 generation）。
//

#include <gtest/gtest.h>

#include "test_util.h"

namespace tbox {
namespace time {
namespace testutil {

using namespace tbox::time;

namespace {

constexpr int64_t kUtc = 1'700'000'000'000;  // 2023-11-14T22:13:20Z

TimeCoreConfig smallCfg() {
    TimeCoreConfig c;
    c.max_source_age_ms = 300'000;
    c.max_source_error_ms = 500;
    c.step_threshold_ms = 1'000;
    c.slew_threshold_ms = 50;
    c.alignment_tolerance_ms = 50;
    c.calibration_retry_limit = 5;
    c.holdover_max_ms = 60'000;      // 测试用小值
    c.holdover_max_error_ms = 1'000;
    c.drift_bound_ppm = 100;
    c.external_change_threshold_ms = 200;
    c.fault_probe_interval_ms = 5'000;
    c.synchronizing_timeout_ms = 30'000;
    c.source_lost_age_ms = 250'000;
    return c;
}

}  // namespace

// AT-01：无源冷启动 → 明确不可用，可查询，不伪装真实时间。
TEST(TimeStateMachine, NoSourceColdStartUnavailable) {
    Harness h(kUtc, smallCfg());
    h.tick();
    const TimeSnapshot s = h.snap();
    EXPECT_EQ(s.state, TimeState::kUnsynch);
    EXPECT_FALSE(s.synchronized);
    EXPECT_FALSE(s.utc_valid);
    EXPECT_EQ(s.utc_epoch_ms, 0);
    EXPECT_FALSE(s.uncertainty_valid);
    EXPECT_TRUE(s.validate());
}

// AT-02：首次同步（小偏差，offset≈0）→ 读回验证 → SYNCHRONIZED。
TEST(TimeStateMachine, FirstSyncSmallOffset) {
    Harness h(kUtc, smallCfg());
    h.inject(goodSample("ntp0", kUtc, h.boot));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);
    const TimeSnapshot s = h.snap();
    EXPECT_TRUE(s.synchronized);
    EXPECT_TRUE(s.utc_valid);
    EXPECT_EQ(s.utc_epoch_ms, kUtc);
    EXPECT_TRUE(s.system_clock_aligned);
    EXPECT_EQ(s.auth_status, AuthStatus::kUnverified);  // 普通 NTP 未认证
    EXPECT_FALSE(s.auth_verified);
    // 首次同步事件链：UNSYNCHRONIZED → SYNCHRONIZING → SYNCHRONIZED
    ASSERT_GE(h.events.size(), 2u);
    const auto& last = h.events.back();
    EXPECT_EQ(last.new_state, TimeState::kSynced);
    EXPECT_EQ(last.reason, TimeReason::kAlignmentVerified);
    EXPECT_EQ(last.old_state, TimeState::kSyncing);
}

// AT-02：大偏差（5000ms > step_threshold）→ step 执行。
TEST(TimeStateMachine, LargeOffsetUsesStep) {
    Harness h(kUtc, smallCfg());
    h.inject(goodSample("ntp0", kUtc + 5'000, h.boot));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);
    EXPECT_EQ(h.backend.wallClockMs(), kUtc + 5'000);  // step 生效
}

// AT-02：中等偏差（200ms：>slew 50，<step 1000）→ slew 路径。
TEST(TimeStateMachine, MediumOffsetUsesSlew) {
    Harness h(kUtc, smallCfg());
    h.inject(goodSample("ntp0", kUtc + 200, h.boot));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);
    EXPECT_EQ(h.backend.wallClockMs(), kUtc + 200);
}

// AT-02：step 写失败 → 校准重试 → FAULT（权限/后端失败不得报告成功）。
// 用大偏差样本触发 step 路径（小偏差仅读回验证，不写钟）。
TEST(TimeStateMachine, StepFailureLeadsToFault) {
    Harness h(kUtc, smallCfg());
    h.backend.ctrl.fail_step = true;
    h.inject(goodSample("ntp0", kUtc + 5'000, h.boot));  // 触发 step
    for (int i = 0; i < 8; ++i) h.tick();
    EXPECT_EQ(h.st(), TimeState::kFault);
    EXPECT_FALSE(h.snap().synchronized);
}

// 竞争写入者：检测到 chronyd/ntpd 等 → FAULT（唯一写入方约束）。
TEST(TimeStateMachine, CompetingWriterLeadsToFault) {
    Harness h(kUtc, smallCfg());
    h.backend.ctrl.competing_writer = true;
    h.inject(goodSample("ntp0", kUtc, h.boot));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kFault);
    EXPECT_EQ(h.snap().reason, TimeReason::kCompetingWriter);
    EXPECT_FALSE(h.snap().synchronized);
}

// FAULT 恢复：故障消除 → 重新获取与校验，不直接恢复同步。
TEST(TimeStateMachine, FaultRecoveryReacquires) {
    Harness h(kUtc, smallCfg());
    h.backend.ctrl.fail_step = true;
    h.inject(goodSample("ntp0", kUtc + 5'000, h.boot));
    for (int i = 0; i < 8; ++i) h.tick();
    ASSERT_EQ(h.st(), TimeState::kFault);

    h.backend.ctrl.fail_step = false;
    h.advance(6'000);  // 超过 fault_probe_interval
    h.inject(goodSample("ntp0", kUtc + 5'000, h.boot));
    h.tick();  // probe 恢复 → 重新获取并校准（同一 tick 内完成）
    EXPECT_EQ(h.st(), TimeState::kSynced);
    EXPECT_EQ(h.backend.wallClockMs(), kUtc + 5'000);
}

// AT-06：SYNCHRONIZED 后出现大偏差样本 → 先撤销（回拨事件）再受控重同步。
TEST(TimeStateMachine, RollbackRevokesBeforeResync) {
    Harness h(kUtc, smallCfg());
    h.inject(goodSample("ntp0", kUtc, h.boot));
    h.tick();
    ASSERT_EQ(h.st(), TimeState::kSynced);

    const size_t ev_before = h.events.size();
    // 新样本偏差 10000ms（回拨/大幅偏移）
    h.inject(goodSample("ntp0", kUtc - 10'000, h.boot + 1'000));
    h.advance(1'000);
    h.tick();  // monitor：撤销同步（RollbackDetected）
    h.tick();  // reevaluate：新样本 → SYNCHRONIZING
    h.tick();  // calibrate：step → SYNCHRONIZED

    // 应经历撤销事件（SYNCHRONIZED → UNSYNCHRONIZED, RollbackDetected）
    bool saw_revoke = false;
    for (size_t i = ev_before; i < h.events.size(); ++i) {
        if (h.events[i].old_state == TimeState::kSynced &&
            h.events[i].new_state == TimeState::kUnsynch &&
            h.events[i].reason == TimeReason::kRollbackDetected) {
            saw_revoke = true;
        }
    }
    EXPECT_TRUE(saw_revoke);
    // 最终重同步成功（墙钟被 step 到新目标）
    EXPECT_EQ(h.st(), TimeState::kSynced);
    EXPECT_EQ(h.backend.wallClockMs(), kUtc - 10'000);
}

// AT-06：外部改时（墙钟被外部改动，单调未变）→ 撤销。
TEST(TimeStateMachine, ExternalClockChangeRevokes) {
    Harness h(kUtc, smallCfg());
    h.inject(goodSample("ntp0", kUtc, h.boot));
    h.tick();
    ASSERT_EQ(h.st(), TimeState::kSynced);

    // 外部把墙钟拨快 10 秒（单调钟不动）
    h.backend.stepTo(kUtc + 10'000);
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kUnsynch);
    EXPECT_EQ(h.snap().reason, TimeReason::kRollbackDetected);
    EXPECT_FALSE(h.snap().synchronized);
}

// AT-06：旧 generation 回调不能覆盖当前来源。
TEST(TimeStateMachine, StaleGenerationRejected) {
    Harness h(kUtc, smallCfg());
    // 新代次样本
    h.inject(goodSample("ntp0", kUtc, h.boot, 50, AuthStatus::kUnverified, 1, 5));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);

    // 旧代次（gen=2）迟到的回调：UTC 完全不同，若被接受将破坏同步
    h.inject(goodSample("ntp0", kUtc - 99'000, h.boot, 50,
                        AuthStatus::kUnverified, 1, 2));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);  // 状态不受旧回调影响
    EXPECT_EQ(h.snap().utc_epoch_ms, kUtc);
}

// AT-05：源丢失 → HOLDOVER；超限 → UNSYNCHRONIZED。
TEST(TimeStateMachine, HoldoverEnterAndExpire) {
    TimeCoreConfig c = smallCfg();
    c.holdover_max_ms = 60'000;
    Harness h(kUtc, c);
    h.inject(goodSample("ntp0", kUtc, h.boot));
    h.tick();
    ASSERT_EQ(h.st(), TimeState::kSynced);

    // 样本超龄（> max_source_age 300s）→ HOLDOVER
    h.advance(301'000);
    EXPECT_EQ(h.st(), TimeState::kHoldover);
    const TimeSnapshot ho = h.snap();
    EXPECT_TRUE(ho.utc_valid);                       // 保持期间提供估计
    EXPECT_TRUE(ho.uncertainty_valid);
    EXPECT_FALSE(ho.synchronized);                   // HOLDOVER 不伪装为同步
    EXPECT_GE(ho.uncertainty_ms, 50);                // 误差以 U0=50 起步并随保持增长
    EXPECT_LE(ho.remaining_validity_ms, 60'000);

    // 超过 holdover_max → UNSYNCHRONIZED
    h.advance(61'000);
    EXPECT_EQ(h.st(), TimeState::kUnsynch);
    EXPECT_EQ(h.snap().reason, TimeReason::kHoldoverExpired);
    EXPECT_FALSE(h.snap().utc_valid);
}

// HOLDOVER 恢复：新合格候选 → 重同步。
TEST(TimeStateMachine, HoldoverRecoveryResyncs) {
    TimeCoreConfig c = smallCfg();
    c.holdover_max_ms = 3'600'000;
    Harness h(kUtc, c);
    h.inject(goodSample("ntp0", kUtc, h.boot));
    h.tick();
    ASSERT_EQ(h.st(), TimeState::kSynced);
    h.advance(301'000);
    ASSERT_EQ(h.st(), TimeState::kHoldover);

    // 恢复：新样本
    h.inject(goodSample("ntp0", kUtc + 10'000, h.boot));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSyncing);
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);
}

// 事件：状态 sequence 只在事实变化时递增；查询不递增。
TEST(TimeStateMachine, SequenceIncrementsOnFactsOnly) {
    Harness h(kUtc, smallCfg());
    h.inject(goodSample("ntp0", kUtc, h.boot));
    h.tick();
    const uint64_t seq_after_sync = h.core->eventSequence();
    EXPECT_GT(seq_after_sync, 0);
    // 多次刷新样本（UTC 随墙钟同步推进，偏差≈0）不改变事实 → sequence 不变
    for (int i = 1; i <= 5; ++i) {
        h.inject(goodSample("ntp0", kUtc + i * 1000, h.boot + (i - 1) * 1000));
        h.advance(1'000);
        h.tick();
        h.snap();
    }
    EXPECT_EQ(h.core->eventSequence(), seq_after_sync);
}

}  // namespace testutil
}  // namespace time
}  // namespace tbox
