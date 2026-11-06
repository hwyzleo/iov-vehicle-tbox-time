#pragma once

//
// TBOX-TIME-DSN-CR-001：daemon 生命周期（FW Application 编排）。
//
// initialize: 配置加载 → 后端/核心/来源/IPC 构建
// execute:    调度线程周期推进状态机，等待停机
// cleanup:    停止 IPC → 撤销状态 → 停来源 → 释放后端（设计 §8）
//

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "application.h"  // hwyz::Application（framework SDK）

#include "backends/linux_system_clock_backend.h"
#include "backends/mock_backend.h"
#include "backends/system_observer_backend.h"
#include "config/time_config.h"
#include "core/time_core.h"
#include "ipc/time_ipc_server.h"
#include "sources/ntp_source.h"
#include "sources/source_manager.h"

namespace tbox {
namespace time {

class TimeApplication final : public hwyz::Application {
public:
    TimeApplication();
    ~TimeApplication() override = default;

    std::string getServiceName() const override { return "time"; }

protected:
    bool initialize() override;
    void cleanup() override;
    int execute() override;

private:
    bool loadConfig();
    void startScheduler();

    TimeConfig config_;
    std::unique_ptr<BackendAdapter> backend_;
    std::unique_ptr<TimeCore> core_;
    std::unique_ptr<SourceManager> source_mgr_;
    std::vector<std::unique_ptr<NtpSourceAdapter>> ntp_adapters_;
    std::unique_ptr<SystemObserverSource> observer_source_;  ///< system-observe（CR-002）
    std::unique_ptr<TimeIpcServer> ipc_;
    std::thread scheduler_thread_;
    std::atomic<bool> scheduler_running_{false};
    bool backend_ready_ = false;
};

}  // namespace time
}  // namespace tbox
