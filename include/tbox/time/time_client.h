#pragma once

//
// TBOX-TIME-DSN-CR-001：TIME client SDK（只读）。
//
// - 查询快照：区分服务不可达/超时/不支持/数据非法/服务可达但时间不可用；
// - 订阅变化：先完成订阅注册并缓冲事件，再查询快照对账（AT-04）；
//   以 daemon_epoch/event_sequence 丢弃旧事件；支持乱序/重复/丢失；
// - 缓存：取 provider 剩余有效期与消费者预算较小值，扣除 RTT/本地年龄；
//   到期失效，不等待下次成功事件（设计 §5/§6）。
//
// client 不拥有来源选取、设时、持久化安全状态或安全信任算法（设计 §2）。
//

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "tbox/time/time_snapshot.h"
#include "tbox/time/types.h"

namespace tbox {
namespace time {

/// 查询错误分类（设计 §6）。
enum class ClientError : uint8_t {
    kOk = 0,
    kServiceUnreachable = 1,  ///< 服务不可达/连接失败
    kTimeout = 2,             ///< 超时
    kUnsupported = 3,         ///< 不支持/版本不兼容
    kInvalidData = 4,         ///< 数据非法
    kTimeUnavailable = 5,     ///< 服务可达但时间不可用（返回明确快照）
    kInternalError = 6,
};

struct SnapshotResult {
    ClientError error = ClientError::kOk;
    TimeSnapshot snapshot;
    int64_t rtt_ms = 0;  ///< 本次请求-响应本地单调 RTT
};

/// 状态变化事件回调（带事件载荷）。
using StateChangedCallback = std::function<void(const TimeStateEvent&)>;

/// 只读 TIME client。
class TimeClient {
public:
    explicit TimeClient(std::string socket_path);
    ~TimeClient();

    TimeClient(const TimeClient&) = delete;
    TimeClient& operator=(const TimeClient&) = delete;

    bool connect();
    void disconnect();
    bool isConnected() const;

    /// 查询当前快照。返回明确错误分类；时间不可用仍返回快照。
    SnapshotResult getTimeSnapshot();

    /// 订阅状态变化。回调在订阅线程触发；返回 RAII 句柄，析构自动取消。
    /// 内部完成"先订阅缓冲 → 再查询对账"。
    std::shared_ptr<void> subscribeStateChanged(StateChangedCallback cb);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace time
}  // namespace tbox
