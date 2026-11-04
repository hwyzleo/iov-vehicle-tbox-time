#pragma once

//
// TBOX-TIME-DSN-CR-001：配置加载与纯校验（设计 §8）。
//
// - 纯校验类型/范围/冲突；不在配置校验时调钟或联网；
// - 禁止 production fake（backend.mode=mock 仅测试 profile 允许）；
// - 未知关键配置拒绝（strict）。
// 配置 schema 与默认值登记 docs/config_schema.md。
//

#include <cstdint>
#include <string>
#include <vector>

#include "core/time_core.h"
#include "sources/ntp_source.h"
#include "sources/source_manager.h"
#include "backends/linux_system_clock_backend.h"

namespace tbox {
namespace time {

/// 运行 profile。
enum class RunProfile : uint8_t {
    kProduction = 0,
    kTest = 1,  ///< dev/test：允许 mock 后端与模拟来源（测试 fixture 隔离）
};

struct TimeConfig {
    // 通用
    RunProfile profile = RunProfile::kProduction;
    std::string policy_version = "1";
    std::string ipc_socket_path = "/tmp/tbox-time.sock";
    int64_t event_queue_limit = 1024;
    int64_t scheduler_interval_ms = 1000;

    // 后端
    BackendKind backend_kind = BackendKind::kLinuxSystemClock;
    LinuxBackendConfig linux_backend;

    // 校时/选源/保持核心配置
    TimeCoreConfig core;

    // 来源
    std::vector<NtpSourceConfig> ntp_servers;
    CellularSourceConfig cellular;
    GnssSourceConfig gnss;
    RtcSourceConfig rtc;
};

/// 从 yaml 文本加载配置。失败返回 false 并给出错误信息。
/// 校验失败（类型/范围/冲突/未知关键配置/非法 fake）不执行任何副作用。
bool loadTimeConfig(const std::string& yaml_text, TimeConfig* out,
                    std::string* error);

/// 校验已填充配置（供单元测试直接调用）。
bool validateTimeConfig(const TimeConfig& cfg, std::string* error);

}  // namespace time
}  // namespace tbox
