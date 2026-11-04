#pragma once

//
// TBOX-TIME-DSN-CR-001：公共类型与常量。
//
// 时间状态机、来源、快照、事件的语义名。wire MethodId/EventId 与
// 配置 schema 名称登记于 docs/wire_registry.md / docs/config_schema.md。
//
// 时间戳统一 UTC epoch 毫秒（1970-01-01T00:00:00Z），不带本地时区偏移；
// 单调计龄使用含休眠经过时间的 CLOCK_BOOTTIME 语义（Linux）。
//

#include <cstdint>
#include <string>

namespace tbox {
namespace time {

/// 快照契约版本。客户端发现未知必需能力必须拒绝（AT-03）。
inline constexpr uint32_t kContractVersion = 1;
/// 策略版本由配置解释（policy_version 字段携带）。
inline constexpr const char* kPolicyDefaultVersion = "1";

/// 同步状态（设计 §3 状态机）。
enum class TimeState : uint8_t {
    kUnsynch = 0,    ///< UNSYNCHRONIZED：尚无可用同步依据
    kSyncing = 1,    ///< SYNCHRONIZING：获取/校准/对齐验证中
    kSynced = 2,     ///< SYNCHRONIZED：同步依据有效且系统时钟对齐满足策略
    kHoldover = 3,   ///< HOLDOVER：依靠已校准本机时钟保持，非当前同步成功
    kFault = 4,      ///< FAULT：本地执行/权限/契约故障导致无法校时
};

/// 稳定原因（快照 reason / 事件 reason 的稳定语义）。
enum class TimeReason : uint8_t {
    kNone = 0,
    kInitial = 1,            ///< 初始无依据
    kQualifiedCandidate = 2, ///< 获得合格候选，进入校准
    kAlignmentVerified = 3,  ///< 读回对齐验证成功
    kAlignmentLost = 4,      ///< 对齐失效
    kSourceLost = 5,         ///< 当前来源丢失
    kSourceChanged = 6,      ///< 来源切换
    kHoldoverEntered = 7,    ///< 进入保持
    kHoldoverExpired = 8,    ///< 保持超误差/超龄/漂移未知
    kBackendFault = 9,       ///< 后端故障（权限/执行/契约）
    kRollbackDetected = 10,  ///< 回拨/大幅偏移/外部改时
    kGenerationStale = 11,   ///< 旧代次回调被拒绝
    kNoSourceAvailable = 12, ///< 无可用来源
    kBackendUnsupported = 13,///< 后端不支持（平台缺失）
    kCompetingWriter = 14,   ///< 检测到竞争校时写入者
    kPolicyViolation = 15,   ///< 超策略门限（年龄/误差/漂移）
};

/// 认证证据状态（设计 §3/§5；TIME 不输出 security_trusted=true）。
enum class AuthStatus : uint8_t {
    kUnknown = 0,    ///< 未配置证据验证
    kUnverified = 1, ///< 来源未经认证验证（如普通 NTP）
    kVerified = 2,   ///< 证据已验证（引用 evidence_ref）
    kFailed = 3,     ///< 证据验证失败
};

/// 来源种类（source_kind）。
enum class SourceKind : uint8_t {
    kNone = 0,
    kNtpNetwork = 1,   ///< 网络授时（Wi-Fi/以太网承载）
    kCellular = 2,     ///< 蜂窝时间
    kGnss = 3,         ///< GNSS
    kRtc = 4,          ///< 硬件 RTC（仅后备，需可证明健康/provision/误差可界定）
};

/// 来源能力状态（运行时检测 + 配置共同决定，设计 §8）。
enum class SourceCapability : uint8_t {
    kDisabled = 0,     ///< 配置未启用
    kUnsupported = 1,  ///< 硬件/接口不存在或平台不支持
    kAvailable = 2,    ///< 已启用且接口存在，可采样
    kDegraded = 3,     ///< 接口存在但暂时不可达/故障（可恢复退避）
};

/// 校时后端类型。
enum class BackendKind : uint8_t {
    kLinuxSystemClock = 1, ///< 自研 Linux 校时后端（唯一写入方）
    kChronyObserve = 2,    ///< 复用 chrony 观测模式（扩展点，未实现）
    kMock = 3,             ///< 测试后端（仅测试 profile，production 拒绝）
    kUnsupported = 4,      ///< 当前平台无可用后端
};

/// 状态 → 派生 synchronized。synchronized 只能由状态推导，不独立维护（REQ-003）。
inline bool isSynchronized(TimeState s) noexcept { return s == TimeState::kSynced; }

/// 稳定语义字符串（日志/事件 reason 脱敏，不含认证材料）。
const char* timeStateName(TimeState s) noexcept;
const char* timeReasonName(TimeReason r) noexcept;
const char* authStatusName(AuthStatus a) noexcept;
const char* sourceKindName(SourceKind k) noexcept;

}  // namespace time
}  // namespace tbox
