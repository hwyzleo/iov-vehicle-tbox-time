//
// TBOX-TIME-DSN-CR-001：选源规则单元测试（设计 §3）。
// 覆盖 AT-01/08：筛选（无效/陈旧/误差未知/认证失败）、配置优先级、
// 稳定窗口/滞回、当前来源失效立即撤销。
//

#include <gtest/gtest.h>

#include "test_util.h"

namespace tbox {
namespace time {
namespace testutil {

using namespace tbox::time;

namespace {

constexpr int64_t kUtc = 1'700'000'000'000;

TimeCoreConfig smallCfg() {
    TimeCoreConfig c;
    c.max_source_age_ms = 300'000;
    c.max_source_error_ms = 500;
    c.step_threshold_ms = 1'000;
    c.slew_threshold_ms = 50;
    c.alignment_tolerance_ms = 50;
    c.stability_window_syncs = 3;
    c.switch_hysteresis_ms = 2'000;
    c.holdover_max_ms = 3'600'000;
    c.holdover_max_error_ms = 1'000;
    c.drift_bound_ppm = 100;
    return c;
}

// 注入 ntp 源（优先级 10）。
void injectNtp(Harness& h, int64_t utc, int64_t boot, uint32_t gen = 1) {
    auto s = goodSample("ntp0", utc, boot);
    s.adapter_generation = gen;
    h.inject(s);
}

}  // namespace

// 优先级：高优先级（小数字）先选。
TEST(SourceSelection, HigherPriorityWins) {
    Harness h(kUtc, smallCfg(), false);
    h.registerSource("ntp0", 10);
    h.registerSource("cell0", 30);
    h.inject(goodSample("ntp0", kUtc, h.boot));
    h.inject(goodSample("cell0", kUtc + 3, h.boot));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);
    EXPECT_EQ(h.snap().source_id, "ntp0");  // 优先级 10 < 30
}

// 筛选：误差未知（error_bounded=false）→ 拒绝可用声明。
TEST(SourceSelection, RejectsUnboundedError) {
    Harness h(kUtc, smallCfg(), false);
    h.registerSource("ntp0", 10);
    auto s = goodSample("ntp0", kUtc, h.boot);
    s.error_bounded = false;
    s.error_upper_ms = 0;
    h.inject(s);
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kUnsynch);
    EXPECT_FALSE(h.snap().utc_valid);
}

// 筛选：误差超策略 → 拒绝。
TEST(SourceSelection, RejectsErrorOverPolicy) {
    TimeCoreConfig c = smallCfg();
    c.max_source_error_ms = 100;
    Harness h(kUtc, c, false);
    h.registerSource("ntp0", 10);
    h.inject(goodSample("ntp0", kUtc, h.boot, /*err=*/200));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kUnsynch);
}

// 筛选：样本超龄 → 拒绝。
TEST(SourceSelection, RejectsStaleSample) {
    Harness h(kUtc, smallCfg(), false);
    h.registerSource("ntp0", 10);
    h.inject(goodSample("ntp0", kUtc, h.boot - 301'000));  // 301s 前采样
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kUnsynch);
}

// 筛选：认证证据失败（kFailed）→ 不可用（AT-08）。
TEST(SourceSelection, RejectsAuthFailed) {
    Harness h(kUtc, smallCfg(), false);
    h.registerSource("ntp0", 10);
    h.inject(goodSample("ntp0", kUtc, h.boot, 50, AuthStatus::kFailed));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kUnsynch);
    EXPECT_FALSE(h.snap().synchronized);
}

// require_authenticated_sources=true：未认证源不可用于校时。
TEST(SourceSelection, RequireAuthenticatedFiltersUnverified) {
    TimeCoreConfig c = smallCfg();
    c.require_authenticated_sources = true;
    Harness h(kUtc, c, false);
    h.registerSource("ntp0", 10);
    h.inject(goodSample("ntp0", kUtc, h.boot, 50, AuthStatus::kUnverified));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kUnsynch);  // 未认证被拒绝

    h.inject(goodSample("ntp0", kUtc, h.boot, 50, AuthStatus::kVerified));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kSynced);  // 已验证通过
    EXPECT_TRUE(h.snap().auth_verified);
}

// 当前来源失效：立即撤销，不受稳定窗口阻碍（AT-05）。
TEST(SourceSelection, ActiveSourceLossRevokesImmediately) {
    Harness h(kUtc, smallCfg(), false);
    h.registerSource("ntp0", 10);
    injectNtp(h, kUtc, h.boot);
    h.tick();
    ASSERT_EQ(h.st(), TimeState::kSynced);

    // 源失效（失败样本，携带新代次：旧回调不覆盖，新回调传达失效）
    h.inject(failSample("ntp0", SourceHealth::kTimeout, "", 2));
    h.tick();
    EXPECT_EQ(h.st(), TimeState::kHoldover);  // 立即进入保持（无滞回等待）
    EXPECT_FALSE(h.snap().synchronized);
}

// 滞回：低优先级新源不切换；稳定窗口未满足的高优先级源不切换。
TEST(SourceSelection, HysteresisBlocksSwitchUntilStable) {
    Harness h(kUtc, smallCfg(), false);
    h.registerSource("ntp0", 10);
    h.registerSource("cell0", 30);
    injectNtp(h, kUtc, h.boot);
    h.tick();
    ASSERT_EQ(h.st(), TimeState::kSynced);
    ASSERT_EQ(h.snap().source_id, "ntp0");

    // 同优先级/低优先级样本到达：不切换
    auto cell = goodSample("cell0", kUtc + 2, h.boot);
    h.inject(cell);
    h.advance(1'000);
    h.tick();
    EXPECT_EQ(h.snap().source_id, "ntp0");
}

// 稳定窗口：连续合格采样达到窗口 → 切换来源。
TEST(SourceSelection, SwitchAfterStabilityWindow) {
    TimeCoreConfig c = smallCfg();
    c.switch_hysteresis_ms = 0;  // 稳定窗口满足即切换（滞回由窗口表达）
    Harness h(kUtc, c, false);
    h.registerSource("ntp0", 10);
    h.registerSource("gnss0", 5);
    injectNtp(h, kUtc, h.boot);
    h.tick();
    ASSERT_EQ(h.st(), TimeState::kSynced);

    // 注入更高优先级源（gnss，优先级 5 < ntp 10），需要连续 3 次稳定采样
    auto g1 = goodSample("gnss0", kUtc + 1, h.boot);
    g1.kind = SourceKind::kGnss;
    g1.adapter_generation = 1;
    h.inject(g1);
    h.advance(1'000);
    h.tick();
    EXPECT_EQ(h.snap().source_id, "ntp0");  // 稳定窗口未满

    auto g2 = g1;
    g2.adapter_generation = 2;
    g2.sampled_boot_ms = h.boot;
    h.inject(g2);
    h.advance(1'000);
    h.tick();
    EXPECT_EQ(h.snap().source_id, "ntp0");  // 仍不足

    auto g3 = g1;
    g3.adapter_generation = 3;
    g3.sampled_boot_ms = h.boot;
    h.inject(g3);
    h.tick();  // 达到窗口 → 切换并立即校准
    EXPECT_EQ(h.snap().source_id, "gnss0");  // 达到窗口 → 切换
    EXPECT_EQ(h.st(), TimeState::kSynced);   // 校准完成（新源偏差极小）
}

}  // namespace testutil
}  // namespace time
}  // namespace tbox
