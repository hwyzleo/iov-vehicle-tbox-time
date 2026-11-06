//
// TBOX-TIME-DSN-CR-002：system-observe 观测形态状态机（设计 §4.1）。
// 覆盖 AT-02-O/AT-05/AT-06/AT-13：
//   - 观测事实驱动：稳定窗口 → kSynced(kObservedSynchronized)，零写钟调用；
//   - STA_UNSYNC set / 证据丢失 → HOLDOVER 或 UNSYNCHRONIZED（kObserverEvidenceUnavailable）；
//   - 观测接口故障 → FAULT(kObserverInterfaceUnavailable)，恢复先 kSyncing 不直跳 kSynced；
//   - 外部改时/回拨 → 旧锚点失效 + kSyncing(kRollbackDetected) → 无新证据 kUnsynch；
//   - 快照 freshness/uncertainty 在查询时按公式增长。
//

#include <gtest/gtest.h>

#include "test_util.h"

namespace tbox {
namespace time {
namespace testutil {

using namespace tbox::time;

namespace {

constexpr int64_t kUtc = 1'700'000'000'000;  // 2023-11-14T22:13:20Z

TimeCoreConfig observeCfg() {
    TimeCoreConfig c;
    c.observe_mode = true;               // system-observe 形态
    c.max_source_age_ms = 300'000;
    c.max_source_error_ms = 500;
    c.stability_window_syncs = 3;        // 稳定窗口（连续合格观测次数）
    c.holdover_max_ms = 60'000;
    c.holdover_max_error_ms = 1'000;
    c.drift_bound_ppm = 100;
    c.external_change_threshold_ms = 200;
    c.synchronizing_timeout_ms = 30'000;
    c.source_lost_age_ms = 250'000;
    return c;
}

/// 合格观测样本（源 system-timesyncd，观测事实字段）。
SourceSample obsSample(int64_t boot, int64_t age0, bool sta_unsync = false,
                       int epoch = 1, uint32_t gen = 1,
                       int64_t maxerror_us = 200'500,
                       int64_t root_dist_us = 71'321,
                       int64_t tol_ppm = 500) {
    SourceSample s;
    s.kind = SourceKind::kNtpNetwork;
    s.source_id = "system-timesyncd";
    s.source_epoch = epoch;
    s.adapter_generation = gen;
    s.utc_valid = true;
    s.utc_epoch_ms = kUtc;
    s.age_valid = true;
    s.sampled_boot_ms = boot;
    s.age_ms = age0;
    s.error_bounded = true;
    s.error_upper_ms = 201;
    s.drift_bounded = true;
    s.drift_upper_ppm = tol_ppm;
    s.auth_status = AuthStatus::kUnverified;
    s.healthy = true;
    s.health = SourceHealth::kOk;
    s.observed_sample = true;
    s.observed_sta_unsync = sta_unsync;
    s.observed_age0_ms = age0;
    s.observed_boot_ms = boot;
    s.observed_kernel_maxerror_us = maxerror_us;
    s.observed_root_distance_us = root_dist_us;
    s.observed_kernel_tolerance_ppm = tol_ppm;
    s.observed_alignment_error_ms = 125;
    return s;
}

/// 观测接口故障样本（adjtimex 失败/缺 interface/契约非法）。
SourceSample obsInterfaceFault(uint32_t gen = 1,
                               const std::string& detail = "adjtimex failed") {
    SourceSample s;
    s.kind = SourceKind::kNtpNetwork;
    s.source_id = "system-timesyncd";
    s.adapter_generation = gen;
    s.healthy = false;
    s.health = SourceHealth::kObserverInterfaceUnavailable;
    s.health_detail = detail;
    s.observed_sample = true;
    return s;
}

Harness makeHarness() {
    Harness h(kUtc, observeCfg(), /*with_default_ntp=*/false);
    h.registerSource("system-timesyncd", 10);
    return h;
}

}  // namespace

// AT-02-O：观测同步建立需要稳定窗口；达到后 kSynced(kObservedSynchronized)，
// 无任何写钟调用（后端 Mock 墙钟不被 step/slew 修改）。
TEST(ObserveStateMachine, FirstSyncRequiresStableWindow) {
    Harness h = makeHarness();
    const int64_t b0 = h.boot;

    h.inject(obsSample(b0, 500));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSyncing);
    EXPECT_EQ(h.snap().reason, TimeReason::kQualifiedCandidate);

    h.inject(obsSample(b0, 500));  // 稳定窗口计数 2
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSyncing);

    h.inject(obsSample(b0, 500));  // 稳定窗口计数 3
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);

    const TimeSnapshot s = h.snap();
    EXPECT_TRUE(s.synchronized);
    EXPECT_EQ(s.reason, TimeReason::kObservedSynchronized);
    EXPECT_EQ(s.source_id, "system-timesyncd");
    EXPECT_EQ(s.source_kind, SourceKind::kNtpNetwork);
    EXPECT_TRUE(s.system_clock_aligned);
    EXPECT_EQ(s.alignment_error_ms, 125);  // ceil(abs(offset)/1000)，观测偏差
    EXPECT_EQ(s.auth_status, AuthStatus::kUnverified);  // 普通 NTP 未认证
    // 墙钟未被 TIME 修改（观测形态不写钟）
    EXPECT_EQ(h.backend.wallClockMs(), kUtc);
    // 事件链：UNSYNCHRONIZED → SYNCHRONIZING → SYNCHRONIZED（kObservedSynchronized）
    ASSERT_GE(h.events.size(), 2u);
    EXPECT_EQ(h.events.back().new_state, TimeState::kSynced);
    EXPECT_EQ(h.events.back().reason, TimeReason::kObservedSynchronized);
}

// AT-14：快照 freshness/uncertainty 在回复生成时按公式增长（真实测量年龄）。
TEST(ObserveStateMachine, SnapshotQualityGrowsWithBoottime) {
    Harness h = makeHarness();
    const int64_t b0 = h.boot;
    for (int i = 0; i < 3; ++i) h.inject(obsSample(b0, 500));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);

    h.advance(60'000);  // BOOTTIME +60s（墙钟同步流逝）
    const TimeSnapshot s = h.snap();
    // freshness = age0 + (now - B0) = 500 + 60000
    EXPECT_EQ(s.freshness_age_ms, 60'500);
    // uncertainty = ceil(max(200500+500*60000/1000, 71321+500*60500/1000)/1000)
    EXPECT_EQ(s.uncertainty_ms, 231);
    EXPECT_EQ(s.remaining_validity_ms, 300'000 - 60'500);
    EXPECT_TRUE(s.utc_valid);
    EXPECT_TRUE(s.uncertainty_valid);
    EXPECT_TRUE(s.validate());
}

// AT-05：STA_UNSYNC set（内核未纪律）→ 非合格候选 → HOLDOVER（保持模型）。
TEST(ObserveStateMachine, StaUnsyncEntersHoldover) {
    Harness h = makeHarness();
    const int64_t b0 = h.boot;
    for (int i = 0; i < 3; ++i) h.inject(obsSample(b0, 500));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);

    h.inject(obsSample(b0, 500, /*sta_unsync=*/true));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kHoldover);
    const TimeSnapshot s = h.snap();
    EXPECT_FALSE(s.synchronized);  // HOLDOVER 不伪装当前同步成功
    EXPECT_TRUE(s.utc_valid);      // 保持模型仍提供 UTC 估计（误差增长）
}

// AT-05：证据丢失（D-Bus absent/报文缺失）且无保持依据 → UNSYNCHRONIZED，
// reason = kObserverEvidenceUnavailable。
TEST(ObserveStateMachine, EvidenceUnavailableWhileSyncing) {
    Harness h = makeHarness();
    h.inject(obsSample(h.boot, 500));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSyncing);

    h.inject(failSample("system-timesyncd", SourceHealth::kUnreachable,
                        "timesync1 service absent"));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kUnsynch);
    EXPECT_EQ(h.snap().reason, TimeReason::kObserverEvidenceUnavailable);
}

// AT-13：观测接口故障（adjtimex 失败/契约非法）→ FAULT(kObserverInterfaceUnavailable)；
// 恢复由新的合格观测样本驱动，先 kSyncing 不直接 kSynced。
TEST(ObserveStateMachine, InterfaceFaultEntersFaultAndRecoversViaSyncing) {
    Harness h = makeHarness();
    for (int i = 0; i < 3; ++i) h.inject(obsSample(h.boot, 500));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);

    h.inject(obsInterfaceFault(2, "adjtimex(modes=0) failed"));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kFault);
    EXPECT_EQ(h.snap().reason, TimeReason::kObserverInterfaceUnavailable);
    EXPECT_FALSE(h.snap().utc_valid);

    // 接口恢复：合格样本 → 先 kSyncing，稳定窗口后才 kSynced
    h.inject(obsSample(h.boot, 500, false, 1, 3));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSyncing);
    EXPECT_EQ(h.snap().reason, TimeReason::kQualifiedCandidate);

    h.inject(obsSample(h.boot, 500, false, 1, 4));
    h.inject(obsSample(h.boot, 500, false, 1, 5));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);
    EXPECT_EQ(h.snap().reason, TimeReason::kObservedSynchronized);
}

// AT-06：外部改时/回拨（timesyncd step）→ 撤销旧锚点 + kSyncing(kRollbackDetected)；
// 无新证据 → kUnsynch(kObserverEvidenceUnavailable)；新测量后重新同步。
TEST(ObserveStateMachine, RollbackInvalidatesOldEvidenceThenResyncs) {
    Harness h = makeHarness();
    const int64_t b0 = h.boot;
    for (int i = 0; i < 3; ++i) h.inject(obsSample(b0, 500));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);

    // 外部改时：墙钟回拨/跳变 5s（BOOTTIME 不推进，模拟 timesyncd step）
    h.backend.advanceWall(-5'000);
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSyncing);
    EXPECT_EQ(h.snap().reason, TimeReason::kRollbackDetected);
    // 旧样本与锚点已失效：无新证据 → 下一 tick kUnsynch
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kUnsynch);
    EXPECT_EQ(h.snap().reason, TimeReason::kObserverEvidenceUnavailable);

    // 新外部测量证据（新 DestinationTimestamp，锚点重置）→ 重新同步
    h.inject(obsSample(h.boot, 500, false, 1, 2));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSyncing);
    h.inject(obsSample(h.boot, 500, false, 1, 3));
    h.inject(obsSample(h.boot, 500, false, 1, 4));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);
}

// AT-05：保持超限 → UNSYNCHRONIZED(kHoldoverExpired)。
TEST(ObserveStateMachine, HoldoverExpires) {
    Harness h = makeHarness();
    const int64_t b0 = h.boot;
    for (int i = 0; i < 3; ++i) h.inject(obsSample(b0, 500));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);

    h.inject(obsSample(b0, 500, /*sta_unsync=*/true));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kHoldover);

    h.advance(60'001);  // 超过 holdover_max_ms=60000
    EXPECT_EQ(h.st(), TimeState::kUnsynch);
    EXPECT_EQ(h.snap().reason, TimeReason::kHoldoverExpired);
}

// 观测形态 kSyncing 超时（无稳定窗口）→ UNSYNCHRONIZED，不伪装校时中。
TEST(ObserveStateMachine, SyncingTimeout) {
    Harness h = makeHarness();
    h.inject(obsSample(h.boot, 500));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSyncing);
    h.advance(30'001);  // 超过 synchronizing_timeout_ms=30000
    EXPECT_EQ(h.st(), TimeState::kUnsynch);
    EXPECT_EQ(h.snap().reason, TimeReason::kNoSourceAvailable);
}

// 观测形态超龄：真实测量年龄超 max_source_age → 撤销合格候选。
TEST(ObserveStateMachine, StaleMeasurementRevoked) {
    Harness h = makeHarness();
    const int64_t b0 = h.boot;
    for (int i = 0; i < 3; ++i) h.inject(obsSample(b0, 500));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);

    h.advance(300'001);  // freshness = 500 + 300001 > max_source_age_ms=300000
    // 墙钟同步流逝（无外部改时）；候选因真实测量年龄超限失效 → HOLDOVER/UNSYNCH
    const TimeState st = h.st();
    EXPECT_TRUE(st == TimeState::kHoldover || st == TimeState::kUnsynch);
    EXPECT_FALSE(h.snap().synchronized);
}

}  // namespace testutil
}  // namespace time
}  // namespace tbox
