#pragma once

//
// TBOX-TIME-DSN-CR-001：IPC 载荷 JSON 编解码（nlohmann_json）。
//
// wire 格式登记 docs/wire_registry.md。数值校验遵循快照 validate()。
//

#include <string>

#include "tbox/time/time_snapshot.h"

namespace tbox {
namespace time {

/// MethodId / EventId（本地登记；BUILD 统一 wire 登记待办见 docs）。
inline constexpr uint32_t kMethodGetTimeSnapshot = 0x0001;
inline constexpr uint32_t kMethodSubscribeTimeState = 0x0002;
inline constexpr uint32_t kEventTimeStateChanged = 0x0001;

/// 业务响应状态（区分"服务可达但时间不可用"与数据非法，设计 §6）。
enum class IpcStatus : uint8_t {
    kOk = 0,
    kTimeUnavailable = 1,  ///< 服务可达但当前无有效时间（返回明确快照）
    kUnsupported = 2,      ///< 方法/版本不支持
    kInvalid = 3,          ///< 请求数据非法
    kInternalError = 4,
};

/// 快照 → JSON 字符串（{status, snapshot:{...}}）。
std::string snapshotToJson(const TimeSnapshot& snap, IpcStatus status);
/// 事件 → JSON 字符串（time.state_changed 载荷）。
std::string eventToJson(const TimeStateEvent& ev);
/// 请求 params 解析（当前无参数方法；预留版本协商）。
bool parseRequestParams(const std::string& params_json, uint32_t* contract_version,
                        std::string* error);

}  // namespace time
}  // namespace tbox
