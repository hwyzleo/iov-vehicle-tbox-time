#pragma once

//
// TBOX-TIME-DSN-CR-001：时间快照（设计 §5）。
//
// 版本化只读快照：UTC、年龄、误差与对齐在同一串行提交/采样边界形成
// 可解释快照；状态 sequence 只在状态事实变化时递增。
// 数值必须有范围与溢出校验；时间戳统一 UTC epoch 毫秒。
//

#include <cstdint>
#include <optional>
#include <string>

#include "tbox/time/types.h"

namespace tbox {
namespace time {

/// 版本化时间快照（设计 §5 建议字段全量）。
struct TimeSnapshot {
    // 版本与能力
    uint32_t contract_version = kContractVersion;
    std::string capabilities;   ///< 能力描述（客户端校验必需能力）

    // 服务实例标识与状态序列
    uint64_t daemon_epoch = 0;      ///< 服务实例标识（重启改变）
    uint64_t event_sequence = 0;    ///< 本实例内严格递增状态序列
    TimeState state = TimeState::kUnsynch;
    bool synchronized = false;      ///< 派生：state == SYNCHRONIZED
    TimeReason reason = TimeReason::kNone;
    std::string reason_detail;      ///< 稳定原因补充（脱敏）

    // 来源
    std::string source_id;
    SourceKind source_kind = SourceKind::kNone;
    uint64_t source_epoch = 0;      ///< 来源连续性代次（不跨来源比较）

    // UTC 估计
    bool utc_valid = false;
    int64_t utc_epoch_ms = 0;       ///< 当前 UTC 估计；不可用允许空，不得返回零伪装

    // 本机执行时钟对齐
    bool system_clock_aligned = false;
    int64_t alignment_error_ms = 0; ///< 读回测得偏差（未对齐时记录）

    // 年龄与误差
    int64_t freshness_age_ms = 0;   ///< 回复生成时年龄（单调计龄）
    bool uncertainty_valid = false;
    int64_t uncertainty_ms = 0;     ///< 当前保守误差上界；unknown 不写零

    // 有效期与漂移
    int64_t remaining_validity_ms = 0;  ///< 本快照事实可缓存剩余时长
    int64_t drift_bound_ppm = 0;        ///< 可用漂移上界

    // 认证证据状态
    AuthStatus auth_status = AuthStatus::kUnknown;
    std::string evidence_ref;       ///< 受控证据引用（非凭据正文）
    bool auth_verified = false;     ///< 派生

    // 策略
    std::string policy_version = kPolicyDefaultVersion;

    /// 数值范围/溢出校验。非法快照不得发布（AT-03）。
    bool validate() const noexcept;
};

/// 状态变化事件（time.state_changed 载荷，设计 §6）。
/// old/new state 区分首次同步与恢复；不引入另一套独立权威成功信号。
struct TimeStateEvent {
    uint64_t daemon_epoch = 0;
    uint64_t event_sequence = 0;    ///< 与快照相同版本
    uint64_t source_epoch = 0;
    TimeState old_state = TimeState::kUnsynch;
    TimeState new_state = TimeState::kUnsynch;
    TimeReason reason = TimeReason::kNone;
    std::string source_id;
    SourceKind source_kind = SourceKind::kNone;
    bool utc_valid = false;
    int64_t utc_epoch_ms = 0;
    int64_t uncertainty_ms = 0;
    bool uncertainty_valid = false;
    AuthStatus auth_status = AuthStatus::kUnknown;

    bool validate() const noexcept;
};

}  // namespace time
}  // namespace tbox
