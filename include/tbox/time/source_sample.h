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
