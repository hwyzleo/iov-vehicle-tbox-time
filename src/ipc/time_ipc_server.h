#pragma once

//
// TBOX-TIME-DSN-CR-001：只读 IPC 服务端（复用 FW ipc）。
//
// 业务只读：仅 getTimeSnapshot / subscribeTimeStateChanged，无通用 setTime
// IPC（AT-03）。事件投递有界（不阻塞校时线程，设计 §6）。
//

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "ipc.h"  // tbox::fw::ipc（framework SDK）

#include "core/time_core.h"
#include "ipc/time_json.h"

namespace tbox {
namespace time {

/// IPC 服务端：挂接 TimeCore 快照读取与状态事件发布。
class TimeIpcServer {
public:
    TimeIpcServer(std::string socket_path, int64_t event_queue_limit);
    ~TimeIpcServer();

    bool start(TimeCore* core);
    void stop();

    /// 事件投递线程（有界队列 → push_event）。
    void enqueueEvent(const TimeStateEvent& ev);

    /// 事件队列溢出计数（诊断）。
    uint64_t droppedEvents() const { return dropped_.load(); }

private:
    void eventDispatchLoop();

    std::string socket_path_;
    int64_t queue_limit_;
    std::atomic<uint64_t> dropped_{0};
    std::atomic<bool> running_{false};
    std::thread dispatch_thread_;
    std::mutex qmu_;
    std::queue<TimeStateEvent> queue_;

    std::shared_ptr<tbox::fw::ipc::Server> server_;
};

}  // namespace time
}  // namespace tbox
