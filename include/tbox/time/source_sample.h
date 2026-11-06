#pragma once

//
// TBOX-TIME-DSN-CR-001：来源采样样本（SourceSample，设计 §3）。
//
// 来源适配输出统一样本；异步回调必须携带 adapter generation，
// 旧请求不得覆盖当前来源（设计 §6 生成代次 / AT-06）。
//
// 数值范围与溢出校验由构造/校验函数保证；"unknown" 不得写零。
//

#include <cstdint>
#include <optional>
#include <string>

#include "tbox/time/types.h"

namespace tbox {
namespace time {

/// 来源健康/故障原因（诊断语义，不携带认证材料）。
enum class SourceHealth : uint8_t {
    kOk = 0,
    kUnreachable = 1,     ///< 端点/接口不可达
    kTimeout = 2,         ///< 采样超时
    kInvalidSample = 3,   ///< 报文非法/字段缺失/单位非法
    kTooOld = 4,          ///< 样本年龄超门限
    kErrorUnbounded = 5,  ///< 误差无法界定
    kAuthFailed = 6,      ///< 认证证据验证失败
    kInterfaceMissing = 7,///< 硬件/接口缺失（unsupported）
    kFault = 8,           ///< 其他故障
    kObserverInterfaceUnavailable = 9, ///< 观测接口故障（adjtimex 失败/缺 interface/契约非法，CR-002）
};

/// 来源采样结果（设计 §3 SourceSample）。
struct SourceSample {
    SourceKind kind = SourceKind::kNone;
    std::string source_id;          ///< 来源标识（配置登记，非凭据）
    uint64_t source_epoch = 0;      ///< 来源连续性代次（不能跨来源比较）
    uint32_t adapter_generation = 0;///< 适配器异步代次（旧回调拒绝）

    bool utc_valid = false;
    int64_t utc_epoch_ms = 0;       ///< UTC epoch 毫秒（无效时空）

    bool age_valid = false;
    int64_t sampled_boot_ms = 0;    ///< 采样时刻单调（BOOTTIME 语义）毫秒
    int64_t age_ms = 0;             ///< 样本年龄（到当前单调时刻），无效时置 0

    bool error_bounded = false;
    int64_t error_upper_ms = 0;     ///< 误差上界（含传输/排队延迟预算）
    bool drift_bounded = false;
    int64_t drift_upper_ppm = 0;    ///< 漂移上界 ppm（HOLDOVER 模型用）

    AuthStatus auth_status = AuthStatus::kUnknown;
    std::string evidence_ref;       ///< 受控证据引用，非凭据正文
    bool auth_verified = false;     ///< 派生：auth_status == kVerified

    bool healthy = false;           ///< 来源有效性
    SourceHealth health = SourceHealth::kOk;
    std::string health_detail;      ///< 诊断细节（脱敏）

    // ---- system-observe 观测事实（TBOX-TIME-DSN-CR-002，设计 §3.2/§5）----
    // 仅系统观测样本（source_id=system-timesyncd）填充，用于查询时按真实测量
    // 年龄与保守不确定度公式实时计算增长；其余来源不设置（保持既有行为）。
    bool observed_sample = false;        ///< 系统观测样本标记
    bool observed_sta_unsync = false;    ///< 边界 STA_UNSYNC 置位（内核未纪律 → 非合格候选）
    int64_t observed_age0_ms = 0;        ///< 首次观测该测量的已有年龄 age0 = R0 - D（D=DestinationTimestamp）
    int64_t observed_boot_ms = 0;        ///< 观测边界 CLOCK_BOOTTIME B0
    int64_t observed_kernel_maxerror_us = 0; ///< 边界 adjtimex.maxerror
    int64_t observed_root_distance_us = 0;   ///< 边界根距离 RootDelay/2+RootDispersion+Jitter
    int64_t observed_kernel_tolerance_ppm = 0; ///< 边界 adjtimex.tolerance 换算（有符号/溢出检查后）
    int64_t observed_alignment_error_ms = 0;   ///< ceil(abs(adjtimex.offset)/1000)（观测偏差，非读回验证）

    /// 校验数值范围/溢出。非法返回 false（健康失败样本仍可通过结构校验）。
    bool validate() const noexcept;
};

/// 来源能力状态（运行时检测结果，设计 §8）。
struct SourceCapabilityInfo {
    SourceKind kind = SourceKind::kNone;
    bool config_enabled = false;
    SourceCapability capability = SourceCapability::kDisabled;
    std::string detail;             ///< 检测依据（脱敏）
};

}  // namespace time
}  // namespace tbox
