#include "ipc/time_ipc_server.h"

#include <chrono>
#include <thread>

namespace tbox {
namespace time {

namespace {

// FW IPC 请求处理器：返回 (status_code, response_json)。
// status_code == 0 表示传输+处理成功；业务状态在 JSON 的 "status" 字段表达。
// RequestHandler 签名（FW）：(uint32_t method_id, std::string_view params, int client_fd) -> std::string
std::string handleRequest(
    TimeCore* core, uint32_t method_id, std::string_view params_json, int) {
    switch (method_id) {
        case kMethodGetTimeSnapshot: {
            uint32_t ver = 0;
            std::string err;
            if (!parseRequestParams(std::string(params_json), &ver, &err)) {
                return snapshotToJson(TimeSnapshot{}, IpcStatus::kInvalid);
            }
            if (ver > kContractVersion) {
                return snapshotToJson(TimeSnapshot{}, IpcStatus::kUnsupported);
            }
            const TimeSnapshot snap = core->snapshot();
            if (!snap.validate()) {
                return snapshotToJson(snap, IpcStatus::kInternalError);
            }
            const bool has_time = snap.utc_valid || snap.state == TimeState::kHoldover ||
                                  snap.state == TimeState::kSynced;
            return snapshotToJson(snap, has_time ? IpcStatus::kOk
                                           : IpcStatus::kTimeUnavailable);
        }
        case kMethodSubscribeTimeState: {
            // 订阅握手：校验版本，返回当前快照作为初始对账基线
            uint32_t ver = 0;
            std::string err;
            if (!parseRequestParams(std::string(params_json), &ver, &err)) {
                return snapshotToJson(TimeSnapshot{}, IpcStatus::kInvalid);
            }
            if (ver > kContractVersion) {
                return snapshotToJson(TimeSnapshot{}, IpcStatus::kUnsupported);
            }
            const TimeSnapshot snap = core->snapshot();
            const bool has_time = snap.utc_valid || snap.state == TimeState::kHoldover ||
                                  snap.state == TimeState::kSynced;
            return snapshotToJson(snap, has_time ? IpcStatus::kOk
                                           : IpcStatus::kTimeUnavailable);
        }
        default:
            return snapshotToJson(TimeSnapshot{}, IpcStatus::kUnsupported);
    }
}

}  // namespace

TimeIpcServer::TimeIpcServer(std::string socket_path, int64_t event_queue_limit)
    : socket_path_(std::move(socket_path)), queue_limit_(event_queue_limit) {}

TimeIpcServer::~TimeIpcServer() { stop(); }

bool TimeIpcServer::start(TimeCore* core) {
    server_ = std::make_shared<tbox::fw::ipc::Server>(socket_path_);
    if (!server_) return false;

    running_ = true;
    dispatch_thread_ = std::thread(&TimeIpcServer::eventDispatchLoop, this);

    // FW Server 不自动注册订阅：subscribe 握手时显式登记 event_type
    auto srv = server_;
    const bool ok = server_->start(
        [core, srv](uint32_t method_id, std::string_view params, int client_fd) {
            if (method_id == kMethodSubscribeTimeState) {
                uint32_t ver = 0;
                std::string err;
                if (parseRequestParams(std::string(params), &ver, &err) &&
                    ver <= kContractVersion) {
                    srv->add_subscription(client_fd, kEventTimeStateChanged);
                }
            }
            return handleRequest(core, method_id, params, client_fd);
        });
    if (!ok) {
        running_ = false;
        if (dispatch_thread_.joinable()) dispatch_thread_.join();
        server_.reset();
        return false;
    }
    return true;
}

void TimeIpcServer::stop() {
    if (server_) server_->stop();
    running_ = false;
    {
        std::lock_guard<std::mutex> lk(qmu_);
        std::queue<TimeStateEvent> empty;
        queue_.swap(empty);
    }
    if (dispatch_thread_.joinable()) dispatch_thread_.join();
    server_.reset();
}

void TimeIpcServer::enqueueEvent(const TimeStateEvent& ev) {
    {
        std::lock_guard<std::mutex> lk(qmu_);
        if (static_cast<int64_t>(queue_.size()) >= queue_limit_) {
            dropped_.fetch_add(1);
            return;  // 有界队列：不无限阻塞校时线程
        }
        queue_.push(ev);
    }
}

void TimeIpcServer::eventDispatchLoop() {
    while (running_.load()) {
        TimeStateEvent ev;
        bool have = false;
        {
            std::lock_guard<std::mutex> lk(qmu_);
            if (!queue_.empty()) {
                ev = queue_.front();
                queue_.pop();
                have = true;
            }
        }
        if (have) {
            if (server_) {
                server_->push_event(kEventTimeStateChanged, eventToJson(ev));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

}  // namespace time
}  // namespace tbox
