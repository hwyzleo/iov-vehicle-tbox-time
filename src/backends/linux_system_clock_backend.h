#pragma once

//
// TBOX-TIME-DSN-CR-001：Linux 系统时钟后端（真实唯一写入方）。
//
// - step：clock_settime(CLOCK_REALTIME)
// - slew：adjtimex(ADJ_OFFSET, ADJ_TIMECONST)
// - 读回验证：clock_gettime(CLOCK_REALTIME) 与目标 UTC 区间比对
// - 竞争写入者检测：chronyd / ntpd / systemd-timesyncd / openntpd
//   （检测到且配置不允许时拒绝执行，避免双写入方）
//
// 仅 Linux 编译；其他平台 capability=Unsupported（如实标记，待盘点 ORIN）。
// 实现细节与阈值登记 docs/implementation_notes.md。
//

#include <cstdint>
#include <string>

#include "tbox/time/backend.h"

namespace tbox {
namespace time {

struct LinuxBackendConfig {
    /// 检测到竞争校时进程时是否允许继续（默认 false：拒绝执行 → FAULT）。
    bool allow_competing_writers = false;
    /// 竞争进程列表（进程名）。
    std::string competing_processes =
        "chronyd:ntpd:systemd-timesyncd:openntpd";
    /// 读回验证重试次数（slew 收敛等待）。
    int verify_retries = 3;
};

/// Linux 系统时钟校时后端。非 Linux 平台编译为空实现（Unsupported）。
class LinuxSystemClockBackend final : public BackendAdapter {
public:
    explicit LinuxSystemClockBackend(LinuxBackendConfig cfg = {});
    ~LinuxSystemClockBackend() override = default;

    BackendKind kind() const override { return BackendKind::kLinuxSystemClock; }
    BackendCapability capability() const override;

    ClockRead readClock() override;
    BackendResult stepTo(int64_t utc_epoch_ms) override;
    BackendResult slewBy(int64_t offset_ms) override;
    BackendResult verifyAlignment(int64_t utc_epoch_ms, int64_t error_upper_ms,
                                  int64_t* alignment_error_ms) override;
    bool detectCompetingWriters(std::string* detail) override;

private:
    LinuxBackendConfig cfg_;
#if defined(__linux__)
    bool detectCompetingLocked(std::string* detail) const;
#endif
};

}  // namespace time
}  // namespace tbox
