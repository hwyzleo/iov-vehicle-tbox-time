#pragma once

//
// TBOX-TIME-DSN-CR-001：时间核心（状态机 + 选源 + 校时 + 保持）。
//
// 单实例逻辑唯一责任方；校时执行通过唯一后端（BackendAdapter）。
// synchronized 只能由状态推导；状态 sequence 只在状态事实变化时递增。
//
// 线程模型：来源采样为异步回调（onSourceSample），状态推进由调度线程
// 周期 tick() 驱动；内部互斥保护状态，快照读取原子。
//

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tbox/time/backend.h"
#include "tbox/time/source_adapter.h"
#include "tbox/time/source_sample.h"
#include "tbox/time/time_snapshot.h"
#include "tbox/time/types.h"

namespace tbox {
namespace time {

/// 单调钟（BOOTTIME 语义，含休眠）时钟源。注入便于测试。
using BootClock = std::function<int64_t()>;

/// 核心配置（阈值实际值登记于 docs/config_schema.md）。
struct TimeCoreConfig {
    // 选源
    int64_t max_source_age_ms = 300000;      ///< 候选最大年龄（默认 300s）
    int64_t max_source_error_ms = 500;       ///< 候选最大误差上界
    bool require_authenticated_sources = false; ///< 未认证源可否用于校时
    int stability_window_syncs = 3;          ///< 切源稳定窗口（连续合格采样数）
    int64_t switch_hysteresis_ms = 2000;     ///< 切源滞回（新来源稳定窗口时长）
    // 校时
    int64_t step_threshold_ms = 1000;        ///< |偏差| >= 此值 → step
    int64_t slew_threshold_ms = 50;          ///< |偏差| <= 此值 → 视为已对齐
    int64_t alignment_tolerance_ms = 50;     ///< 读回验证容差
    int calibration_retry_limit = 5;         ///< 校准连续失败上限 → FAULT
    // 保持
    int64_t holdover_max_ms = 3600000;       ///< 保持时长上限（默认 1h）
    int64_t holdover_max_error_ms = 1000;    ///< 保持误差上界上限
    int64_t drift_bound_ppm = 100;           ///< 漂移上界 ppm
    // 监测
    int64_t monitor_interval_ms = 1000;      ///< 监测 tick 间隔
    int64_t external_change_threshold_ms = 200; ///< 外部改时/回拨判定阈值
    int64_t fault_probe_interval_ms = 5000;  ///< FAULT 恢复探测间隔
    int64_t synchronizing_timeout_ms = 30000;///< SYNCHRONIZING 校准超时
    int64_t source_lost_age_ms = 350000;     ///< 源丢失判定（超龄进入 HOLDOVER）
    int64_t holdover_enter_min_age_ms = 120000; ///< 最短需保持时长才进入 HOLDOVER
    bool holdover_enabled = true;
    int64_t poll_interval_ms = 30000;        ///< 来源轮询间隔（调度层使用）
};

/// 时间核心。
class TimeCore {
public:
    TimeCore(BackendAdapter* backend, TimeCoreConfig cfg, BootClock boot_clock);
    ~TimeCore();

    TimeCore(const TimeCore&) = delete;
    TimeCore& operator=(const TimeCore&) = delete;

    /// 注册来源适配器（start 前调用）。priority 越小优先级越高（配置登记）。
    void registerSource(SourceAdapter* adapter, int priority);

    /// 来源采样结果入口（SourceManager 调用，含 generation 过滤）。
    void onSourceSample(const SourceSample& sample);

    /// 周期性推进（调度线程）。now_boot_ms 可省略（用注入时钟）。
    void tick();

    /// 状态变化事件回调（上层接 IPC 发布；有界投递由上层保证）。
    void setStateChangedCallback(std::function<void(const TimeStateEvent&)> cb);

    /// 当前快照（原子读取）。
    TimeSnapshot snapshot() const;

    /// 启动/停止。
    void start();
    void stop();

    // 测试辅助
    TimeState state() const;
    uint64_t eventSequence() const;
    bool hasActiveSource() const;
    const std::string& activeSourceId() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace time
}  // namespace tbox
