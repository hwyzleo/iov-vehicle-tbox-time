//
// TBOX-TIME-DSN-CR-001：后端单元测试（AT-02）。
// MockBackend 注入故障；LinuxSystemClockBackend 平台能力如实标记。
//

#include <gtest/gtest.h>

#include "backends/linux_system_clock_backend.h"
#include "backends/mock_backend.h"

namespace tbox {
namespace time {

using namespace tbox::time;

namespace {
constexpr int64_t kUtc = 1'700'000'000'000;
}

// MockBackend：step/slew/verify 基本语义。
TEST(Backend, MockStepSlewVerify) {
    MockBackend b(kUtc);
    EXPECT_EQ(b.capability(), BackendCapability::kReady);
    EXPECT_TRUE(b.readClock().valid);
    EXPECT_EQ(b.readClock().realtime_epoch_ms, kUtc);

    auto step = b.stepTo(kUtc + 5'000);
    EXPECT_TRUE(step.ok);
    EXPECT_EQ(b.wallClockMs(), kUtc + 5'000);

    int64_t align = 0;
    auto v = b.verifyAlignment(kUtc + 5'000, 100, &align);
    EXPECT_TRUE(v.ok);
    EXPECT_EQ(align, 0);  // 读回一致
}

// MockBackend：写失败（权限/后端故障）→ 失败结果，不得报告成功。
TEST(Backend, MockStepFailure) {
    MockBackend b(kUtc);
    b.ctrl.fail_step = true;
    auto res = b.stepTo(kUtc);
    EXPECT_FALSE(res.ok);
    EXPECT_EQ(res.errno_code, 5);
    EXPECT_EQ(b.wallClockMs(), kUtc);  // 未写入
}

// MockBackend：slew 不收敛（bias 超容差）→ verifyAlignment 返回偏差超限。
TEST(Backend, MockSlewNotConverged) {
    MockBackend b(kUtc);
    b.ctrl.align_immediately = false;  // slew 挂起不生效
    auto res = b.slewBy(500);
    EXPECT_TRUE(res.ok);
    int64_t align = 0;
    auto v = b.verifyAlignment(kUtc, 50, &align);
    EXPECT_TRUE(v.ok);
    EXPECT_GT(align, 50);  // 未收敛：偏差 500 > 容差
}

// MockBackend：竞争写入者检测。
TEST(Backend, MockCompetingWriter) {
    MockBackend b(kUtc);
    EXPECT_FALSE(b.detectCompetingWriters(nullptr));
    b.ctrl.competing_writer = true;
    std::string detail;
    EXPECT_TRUE(b.detectCompetingWriters(&detail));
    EXPECT_EQ(b.capability(), BackendCapability::kCompeting);
}

// Linux 后端：非 Linux 平台如实标记 unsupported（ORIN 实机验证待盘点）。
TEST(Backend, LinuxBackendPlatformCapability) {
    LinuxSystemClockBackend b;
#if defined(__linux__)
    EXPECT_NE(b.capability(), BackendCapability::kUnsupported);
#else
    EXPECT_EQ(b.capability(), BackendCapability::kUnsupported);
    ClockRead r = b.readClock();
    EXPECT_FALSE(r.valid);
#endif
}

}  // namespace time
}  // namespace tbox
