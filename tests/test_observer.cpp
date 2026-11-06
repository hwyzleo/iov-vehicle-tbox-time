//
// TBOX-TIME-DSN-CR-002：system-observe 观测公式与样本构造（设计 §3/§5）。
// 覆盖 AT-13/AT-14：NTPMessage 必需项校验、一致性边界、保守不确定度与
// 真实测量新鲜度公式（golden）、重复报文不重置锚点、溢出/单位/符号检查。
//

#include <gtest/gtest.h>

#include "backends/observer_math.h"
#include "backends/system_observer_backend.h"

namespace tbox {
namespace time {

using namespace tbox::time;

namespace {

// 基准事实：对齐 Orin 实测示例（maxerror=200.5ms，RootDelay/2+RootDispersion+
// Jitter≈71.8ms，tolerance=500ppm）。R0 = D + 500ms（age0=500ms）。
ObserverFacts goodFacts() {
    ObserverFacts f;
    f.dbus_valid = true;
    f.server_name = "1.pool.ntp.org";
    f.server_address = "119.28.183.184";
    f.dbus_owner = ":1.42";
    f.packet_count = 7;
    f.leap = 0;
    f.version = 4;
    f.mode = 4;
    f.stratum = 2;
    f.root_delay_us = 595;
    f.root_dispersion_us = 44204;
    f.jitter_us = 26820;
    f.have_originate = true;
    f.originate_ts_us = 1'700'000'000'000'000;
    f.have_receive = true;
    f.receive_ts_us = 1'700'000'000'001'000;
    f.have_transmit = true;
    f.transmit_ts_us = 1'700'000'000'002'000;
    f.have_destination = true;
    f.destination_ts_us = 1'700'000'000'005'000;  // D = 2023-11-14T22:13:20.005Z
    f.adjtimex_valid = true;
    f.sta_unsync = false;
    f.maxerror_us = 200'500;
    f.offset_us = -124'320;
    f.tolerance_raw = 32'768'000;  // 500ppm（内核刻度 ppm*2^16）
    f.clock_valid = true;
    f.realtime_ms = 1'700'000'000'505;  // R0 = D + 500ms
    f.boot_ms = 1'000'000'000;          // B0
    f.consistent = true;
    return f;
}

}  // namespace

// ---- 纯公式（设计 §5）----

// tolerance 换算：内核刻度 32768000 = 500ppm（ppm * 2^16）。
TEST(ObserverMath, TolerancePpmConversion) {
    EXPECT_EQ(observerTolerancePpm(32'768'000), 500);
    EXPECT_EQ(observerTolerancePpm(0), 0);
    EXPECT_EQ(observerTolerancePpm(65'536), 1);
    EXPECT_EQ(observerTolerancePpm(-1), -1);            // 有符号非法
    EXPECT_EQ(observerTolerancePpm(INT64_MAX), -1);     // 换算溢出（>1e6 ppm）
}

// 根距离：RootDelay/2 + RootDispersion + Jitter。
TEST(ObserverMath, RootDistance) {
    EXPECT_EQ(observerRootDistanceUs(595, 44204, 26820), 71'321);
    EXPECT_EQ(observerRootDistanceUs(0, 0, 0), 0);
    EXPECT_EQ(observerRootDistanceUs(-1, 0, 0), -1);        // 非法
    EXPECT_EQ(observerRootDistanceUs(INT64_MAX, 1, INT64_MAX), -1);  // 溢出
}

// 真实测量新鲜度：age0 + (now_boot - B0)，仅 BOOTTIME 推进计龄。
TEST(ObserverMath, FreshnessGrowsWithBoottimeOnly) {
    EXPECT_EQ(observerFreshnessMs(500, 1'000'000'000, 1'000'000'000), 500);
    EXPECT_EQ(observerFreshnessMs(500, 1'000'000'000, 1'000'060'000), 60'500);
    // 墙钟跳变不影响 BOOTTIME 计龄（外部改时后旧锚点另行失效）
    EXPECT_EQ(observerFreshnessMs(500, 1'000'000'000, 1'000'000'000 + 3'600'000),
              3'600'500);
    EXPECT_EQ(observerFreshnessMs(-1, 1'000'000'000, 1'000'000'000), -1);  // age0<0 非法
    EXPECT_EQ(observerFreshnessMs(500, 1'000'000'000, 999'999'000), -1);   // now < B0 非法
    EXPECT_EQ(observerFreshnessMs(INT64_MAX, 0, 10), -1);                  // 溢出
}

// 保守不确定度 golden：max(maxerror_term, ntp_term) 向上取整。
//   maxerror_term = 200500 + 500*Δ/1000；ntp_term = 71321 + 500*age/1000
TEST(ObserverMath, UncertaintyGolden) {
    // 提交时刻 Δ=0, age=500ms：ntp_term=71321+250=71571 < maxerror_term=200500
    EXPECT_EQ(observerUncertaintyMs(500, 1'000'000'000, 1'000'000'000,
                                    200'500, 71'321, 500),
              201);  // ceil(200500/1000)
    // 60s 后：Δ=60000 → maxerror_term=230500；age=60500 → ntp_term=101571
    EXPECT_EQ(observerUncertaintyMs(500, 1'000'000'000, 1'000'060'000,
                                    200'500, 71'321, 500),
              231);
    // 误差取较大值：根距离项更大时以根距离为准
    EXPECT_EQ(observerUncertaintyMs(500, 1'000'000'000, 1'000'060'000,
                                    10'000, 1'000'000, 500),
              1'031);  // ceil((1000000 + 500*60500/1000)/1000)=ceil(1030250/1000)
    // 无可靠 tolerance → unknown（-1），不改用零或实测平均 offset
    EXPECT_EQ(observerUncertaintyMs(500, 1'000'000'000, 1'000'060'000,
                                    200'500, 71'321, -1),
              -1);
}

// 观测对齐误差：ceil(abs(offset)/1000)，不写钟也不称读回验证。
TEST(ObserverMath, AlignmentErrorCeilAbs) {
    EXPECT_EQ(observerAlignmentErrorMs(-124'320), 125);
    EXPECT_EQ(observerAlignmentErrorMs(0), 0);
    EXPECT_EQ(observerAlignmentErrorMs(124'320), 125);
    EXPECT_EQ(observerAlignmentErrorMs(1), 1);
}

// ---- buildObserverSample（设计 §3.1/§3.2）----

// 完整合法事实 → 合格观测样本（字段映射正确）。
TEST(ObserverBuilder, ValidFactsProduceSample) {
    SourceSample s;
    std::string err;
    ASSERT_TRUE(buildObserverSample(goodFacts(), 100, &s, &err)) << err;
    EXPECT_EQ(s.source_id, "system-timesyncd");
    EXPECT_EQ(s.kind, SourceKind::kNtpNetwork);
    EXPECT_TRUE(s.utc_valid);
    EXPECT_EQ(s.utc_epoch_ms, 1'700'000'000'505);
    EXPECT_TRUE(s.age_valid);
    EXPECT_EQ(s.age_ms, 500);  // 提交时真实年龄 = age0
    EXPECT_TRUE(s.error_bounded);
    EXPECT_EQ(s.error_upper_ms, 201);
    EXPECT_TRUE(s.drift_bounded);
    EXPECT_EQ(s.drift_upper_ppm, 500);  // 优先 tolerance 换算
    EXPECT_EQ(s.auth_status, AuthStatus::kUnverified);
    EXPECT_TRUE(s.healthy);
    EXPECT_TRUE(s.observed_sample);
    EXPECT_FALSE(s.observed_sta_unsync);
    EXPECT_EQ(s.observed_age0_ms, 500);
    EXPECT_EQ(s.observed_boot_ms, 1'000'000'000);
    EXPECT_EQ(s.observed_kernel_maxerror_us, 200'500);
    EXPECT_EQ(s.observed_root_distance_us, 71'321);
    EXPECT_EQ(s.observed_kernel_tolerance_ppm, 500);
    EXPECT_EQ(s.observed_alignment_error_ms, 125);
    EXPECT_TRUE(s.validate());
}

// 重复读取相同 DestinationTimestamp：锚点不重置（age0/B0 保持首次观测值）。
TEST(ObserverBuilder, RepeatedSameDestinationKeepsAnchor) {
    // 首次观测：R0=D+500ms，B0=1000000000
    ObserverFacts f = goodFacts();
    SourceSample s1;
    std::string err;
    ASSERT_TRUE(buildObserverSample(f, 100, &s1, &err)) << err;

    // 同一报文重复读取（墙钟与 BOOTTIME 都推进了 30s）：age0/B0 不得重置
    f.realtime_ms = 1'700'000'000'505 + 30'000;
    f.boot_ms = 1'000'000'000 + 30'000;
    f.anchor_valid = true;
    f.anchor_age0_ms = 500;         // 首次观测锚点（source 维护）
    f.anchor_boot_ms = 1'000'000'000;
    SourceSample s2;
    ASSERT_TRUE(buildObserverSample(f, 100, &s2, &err)) << err;
    EXPECT_EQ(s2.observed_age0_ms, 500);       // 不因重复读取重置
    EXPECT_EQ(s2.observed_boot_ms, 1'000'000'000);
    EXPECT_EQ(s2.age_ms, 30'500);              // 真实年龄随 BOOTTIME 推进
}

// 新 DestinationTimestamp → 新锚点（age0 随新报文重新计算）。
TEST(ObserverBuilder, NewDestinationEstablishesNewAnchor) {
    ObserverFacts f = goodFacts();
    // 新测量（2s 后），R0 = D + 505ms：无锚点时按本次边界计算 age0 = 505
    f.destination_ts_us = 1'700'000'000'005'000 + 2'000'000;  // = 1.700000002005e15 usec
    f.realtime_ms = 1'700'000'000'505 + 2'005;                // R0 = 1'700'000'002'510
    SourceSample s;
    std::string err;
    ASSERT_TRUE(buildObserverSample(f, 100, &s, &err)) << err;
    EXPECT_EQ(s.observed_age0_ms, 505);
}

// NTPMessage 必需项校验：Leap=3 / Stratum 0|16 / Mode 非 server / 缺时间戳 /
// Ignored=yes / DestinationTimestamp 为零/越界 → 不得产生合格样本。
TEST(ObserverBuilder, RejectsInvalidNtpMessage) {
    auto bad = [&](ObserverFacts f) {
        SourceSample s;
        std::string err;
        return !buildObserverSample(f, 100, &s, &err);
    };
    {
        ObserverFacts f = goodFacts();
        f.leap = 3;
        EXPECT_TRUE(bad(f));
    }
    {
        ObserverFacts f = goodFacts();
        f.stratum = 0;
        EXPECT_TRUE(bad(f));
        f.stratum = 16;
        EXPECT_TRUE(bad(f));
    }
    {
        ObserverFacts f = goodFacts();
        f.mode = 3;  // client 报文非预期
        EXPECT_TRUE(bad(f));
    }
    {
        ObserverFacts f = goodFacts();
        f.have_destination = false;
        EXPECT_TRUE(bad(f));
        f.have_destination = true;
        f.have_transmit = false;
        EXPECT_TRUE(bad(f));
    }
    {
        ObserverFacts f = goodFacts();
        f.ignored = true;
        EXPECT_TRUE(bad(f));
    }
    {
        ObserverFacts f = goodFacts();
        f.destination_ts_us = 0;
        EXPECT_TRUE(bad(f));
    }
    {
        ObserverFacts f = goodFacts();
        f.destination_ts_us = 1;  // 1970 → 范围外
        EXPECT_TRUE(bad(f));
    }
    {
        ObserverFacts f = goodFacts();
        f.destination_ts_us = 4'000'000'000'000'000;  // 超出 2100
        EXPECT_TRUE(bad(f));
    }
}

// 一致性边界：前后证据不一致 → 丢弃（不提交混合样本）。
TEST(ObserverBuilder, RejectsInconsistentBoundary) {
    ObserverFacts f = goodFacts();
    f.consistent = false;
    SourceSample s;
    std::string err;
    EXPECT_FALSE(buildObserverSample(f, 100, &s, &err));
    EXPECT_NE(err.find("inconsistent"), std::string::npos);
}

// 不完整事实（缺 adjtimex / 缺时钟读数 / 缺 D-Bus）→ 拒绝。
TEST(ObserverBuilder, RejectsIncompleteFacts) {
    auto bad = [&](ObserverFacts f) {
        SourceSample s;
        std::string err;
        return !buildObserverSample(f, 100, &s, &err);
    };
    {
        ObserverFacts f = goodFacts();
        f.adjtimex_valid = false;
        EXPECT_TRUE(bad(f));
    }
    {
        ObserverFacts f = goodFacts();
        f.clock_valid = false;
        EXPECT_TRUE(bad(f));
    }
    {
        ObserverFacts f = goodFacts();
        f.dbus_valid = false;
        EXPECT_TRUE(bad(f));
    }
}

// 非法锚点（age0<0）→ 拒绝。
TEST(ObserverBuilder, RejectsInvalidAnchor) {
    ObserverFacts f = goodFacts();
    f.anchor_valid = true;
    f.anchor_age0_ms = -1;
    SourceSample s;
    std::string err;
    EXPECT_FALSE(buildObserverSample(f, 100, &s, &err));
    EXPECT_NE(err.find("anchor"), std::string::npos);
}

// tolerance 无效时：drift 用已配置且经验证的 fallback（core.drift_bound_ppm）。
TEST(ObserverBuilder, ToleranceInvalidUsesConfiguredDriftFallback) {
    ObserverFacts f = goodFacts();
    f.tolerance_raw = -1;  // 非法（有符号）
    SourceSample s;
    std::string err;
    ASSERT_TRUE(buildObserverSample(f, 100, &s, &err)) << err;
    EXPECT_FALSE(s.error_bounded);          // uncertainty unknown（不改用零）
    EXPECT_EQ(s.error_upper_ms, 0);
    EXPECT_TRUE(s.drift_bounded);
    EXPECT_EQ(s.drift_upper_ppm, 100);      // fallback = core.drift_bound_ppm
}

// STA_UNSYNC 置位：结构有效样本仍产出，但对齐标志由核心按 sta_unsync 判定。
TEST(ObserverBuilder, StaUnsyncFlaggedInSample) {
    ObserverFacts f = goodFacts();
    f.sta_unsync = true;
    SourceSample s;
    std::string err;
    ASSERT_TRUE(buildObserverSample(f, 100, &s, &err)) << err;
    EXPECT_TRUE(s.healthy);
    EXPECT_TRUE(s.utc_valid);
    EXPECT_TRUE(s.observed_sta_unsync);
}

}  // namespace time
}  // namespace tbox
