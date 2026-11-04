#pragma once

//
// TBOX-TIME-DSN-CR-001：校时后端契约（设计 §2/§4）。
//
// TIME 是逻辑唯一校时责任方，后端为唯一写入方（二选一）：
//   - 复用系统校时守护进程：TIME 通过受控接口配置/观测且不另行写钟；
//   - 自有平台后端：禁用竞争校时进程。
// 后端必须支持写入后读回并验证对齐（alignment_error_ms）。
//

#include <cstdint>
#include <string>

#include "tbox/time/types.h"

namespace tbox {
namespace time {

/// 时钟读取结果。
struct ClockRead {
    bool valid = false;
    int64_t realtime_epoch_ms = 0;  ///< CLOCK_REALTIME epoch 毫秒
    int64_t boot_monotonic_ms = 0;  ///< CLOCK_BOOTTIME 毫秒（含休眠）
    std::string detail;             ///< 失败诊断（脱敏）
};

/// 后端操作结果。
struct BackendResult {
    bool ok = false;
    int32_t errno_code = 0;
    std::string detail;             ///< 失败诊断（脱敏）
};

/// 后端能力状态。
enum class BackendCapability : uint8_t {
    kReady = 0,      ///< 可执行校时
    kUnsupported = 1,///< 平台不支持
    kFault = 2,      ///< 已故障（权限/执行/契约）
    kCompeting = 3,  ///< 检测到竞争写入者，拒绝执行
};

/// 校时后端抽象（唯一写入方）。
class BackendAdapter {
public:
    virtual ~BackendAdapter() = default;

    virtual BackendKind kind() const = 0;
    virtual BackendCapability capability() const = 0;

    /// 读当前系统墙钟与单调钟（BOOTTIME 语义）。
    virtual ClockRead readClock() = 0;

    /// 一次性 step 校时（冷启动首次校准按策略允许）。
    virtual BackendResult stepTo(int64_t utc_epoch_ms) = 0;

    /// slew 校时（运行期小偏差优先）。
    virtual BackendResult slewBy(int64_t offset_ms) = 0;

    /// 写入后读回并验证 source UTC 区间与本机墙钟偏差。
    /// 返回测得偏差（alignment_error_ms，可正可负）。
    virtual BackendResult verifyAlignment(int64_t utc_epoch_ms,
                                          int64_t error_upper_ms,
                                          int64_t* alignment_error_ms) = 0;

    /// 检测系统上是否有其他校时写入者（chronyd/ntpd/systemd-timesyncd 等）。
    /// 返回 true 表示存在竞争写入者。
    virtual bool detectCompetingWriters(std::string* detail) = 0;
};

}  // namespace time
}  // namespace tbox
