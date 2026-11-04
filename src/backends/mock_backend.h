#pragma once

//
// TBOX-TIME-DSN-CR-001：测试校时后端（MockBackend）。
//
// 仅测试 profile 使用；production 配置校验拒绝 mock（AT-12 fake 隔离）。
// 可注入：写失败（errno）、权限失败、slew 不收敛、读回偏差、竞争写入者。
// 墙钟模式：auto_advance=true（默认）时墙钟随注入单调钟自然流逝（模拟真实
// 系统时钟）；false 时为手动模式（测试通过 advanceWall 推进，避免外部改时误判）。
//

#include <cstdint>
#include <functional>
#include <string>

#include "tbox/time/backend.h"

namespace tbox {
namespace time {

/// 可注入故障控制。
struct MockBackendControl {
    bool fail_step = false;        ///< step 返回失败
    bool fail_slew = false;        ///< slew 返回失败
    bool fail_verify = false;      ///< 读回验证返回失败
    bool fail_read = false;        ///< 读墙钟失败
    int32_t fail_errno = 5;        ///< 模拟 errno（EIO）
    bool competing_writer = false; ///< 检测到竞争写入者
    bool capability_fault = false; ///< 后端能力故障
    int64_t verify_bias_ms = 0;    ///< 读回验证偏差（>容差 → slew 未收敛）
    bool align_immediately = true; ///< slew 是否立即生效（否则由测试手动对齐）
};

/// 模拟系统时钟后端。维护一个"虚拟系统墙钟"：
///   - stepTo 直接设置
///   - slewBy 仅记录 offset，若 align_immediately 立即应用
///   - verifyAlignment 计算虚拟墙钟与目标 UTC 偏差（可注入 bias）
class MockBackend final : public BackendAdapter {
public:
    /// boot_clock：单调钟源；auto_advance=true 时墙钟随单调钟流逝。
    MockBackend(int64_t initial_wall_ms,
                std::function<int64_t()> boot_clock = nullptr,
                bool auto_advance = true);
    ~MockBackend() override = default;

    BackendKind kind() const override { return BackendKind::kMock; }
    BackendCapability capability() const override;

    ClockRead readClock() override;
    BackendResult stepTo(int64_t utc_epoch_ms) override;
    BackendResult slewBy(int64_t offset_ms) override;
    BackendResult verifyAlignment(int64_t utc_epoch_ms, int64_t error_upper_ms,
                                  int64_t* alignment_error_ms) override;
    bool detectCompetingWriters(std::string* detail) override;

    MockBackendControl ctrl;

    /// 当前虚拟墙钟（测试断言用，含自动流逝）。
    int64_t wallClockMs() const;

    /// 手动推进墙钟（手动模式，避免外部改时误判）。
    void advanceWall(int64_t ms);

private:
    int64_t effectiveWall() const;
    int64_t wall_ms_;
    int64_t pending_slew_ms_ = 0;
    std::function<int64_t()> boot_clock_;
    bool auto_advance_;
    int64_t boot_anchor_ = 0;
};

}  // namespace time
}  // namespace tbox
