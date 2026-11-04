#pragma once

//
// TBOX-TIME-DSN-CR-001：来源编排（能力检测 + 代次注入）。
//
// 配置允许 + 运行时检测可用（设计 §8）：
//   - 未配置/无硬件 → disabled/unsupported，不持续报故障、不阻塞启动；
//   - 已启用但断网/不可达 → 可恢复退避重试并重新选源（适配器 Degraded）；
//   - 自动检测不得擅自启用未授权来源，不得自动把未认证来源升级为可信。
//
// 回调代次（generation）由本管理器注入：旧请求不能覆盖当前来源（AT-06）。
//

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "tbox/time/source_adapter.h"
#include "tbox/time/source_sample.h"

namespace tbox {
namespace time {

class TimeCore;

/// 来源检测结果。
struct SourceDetectInfo {
    bool config_enabled = false;
    SourceCapability capability = SourceCapability::kDisabled;
    std::string detail;  ///< 检测依据（脱敏）
};

/// 蜂窝来源配置（协议为厂商契约，缺口登记 docs/implementation_notes.md）。
struct CellularSourceConfig {
    std::string source_id = "cell0";
    bool enabled = false;
    int priority = 30;
    std::string device;            ///< 空 = 自动检测
    int64_t poll_interval_ms = 60000;
    int64_t error_upper_ms = 200;  ///< 厂商协议未定：登记缺口
    int64_t drift_upper_ppm = 100;
};

/// GNSS 来源配置（驱动/协议为厂商契约，缺口登记）。
struct GnssSourceConfig {
    std::string source_id = "gnss0";
    bool enabled = false;
    int priority = 20;
    std::string device;            ///< 空 = 自动检测
    int64_t poll_interval_ms = 1000;
    int64_t error_upper_ms = 50;
    int64_t drift_upper_ppm = 100;
};

/// RTC 来源配置（仅后备；需可证明健康/provision/误差可界定）。
struct RtcSourceConfig {
    std::string source_id = "rtc0";
    bool enabled = false;
    int priority = 40;
    std::string device;            ///< 空 = 自动检测 (/dev/rtc0)
    bool require_provisioned = true; ///< provision 证据接口登记缺口
    int64_t error_upper_ms = 500;
    int64_t drift_upper_ppm = 50;
};

/// 平台来源检测（真实代码，缺硬件返回 unsupported；非 Linux 平台 RTC/GNSS/蜂窝 unsupported）。
SourceCapability detectCellularModule(std::string* detail);
SourceCapability detectGnssModule(std::string* detail);
SourceCapability detectRtcModule(std::string* detail);

/// 蜂窝来源适配器。无模块 → unsupported 且不启动；
/// 有模块但厂商协议未登记 → start 失败并保留缺口说明（不占位标完成）。
class CellularSourceAdapter final : public SourceAdapter {
public:
    explicit CellularSourceAdapter(CellularSourceConfig cfg);
    const std::string& sourceId() const override;
    SourceKind kind() const override { return SourceKind::kCellular; }
    bool start(SampleCallback cb) override;
    void stop() override;
    SourceCapability capability() const override;

private:
    CellularSourceConfig cfg_;
    SampleCallback cb_;
    bool started_ = false;
    SourceCapability cap_;
};

/// GNSS 来源适配器。同上：无硬件/无协议契约 → unsupported。
class GnssSourceAdapter final : public SourceAdapter {
public:
    explicit GnssSourceAdapter(GnssSourceConfig cfg);
    const std::string& sourceId() const override;
    SourceKind kind() const override { return SourceKind::kGnss; }
    bool start(SampleCallback cb) override;
    void stop() override;
    SourceCapability capability() const override;

private:
    GnssSourceConfig cfg_;
    SampleCallback cb_;
    bool started_ = false;
    SourceCapability cap_;
};

/// RTC 来源适配器：Linux 真实读取（/dev/rtc0 + RTC_RD_TIME）。
/// 仅在配置允许且 provision/健康/误差可界定时参与；非 Linux 平台 unsupported。
class RtcSourceAdapter final : public SourceAdapter {
public:
    explicit RtcSourceAdapter(RtcSourceConfig cfg);
    ~RtcSourceAdapter() override;
    const std::string& sourceId() const override;
    SourceKind kind() const override { return SourceKind::kRtc; }
    bool start(SampleCallback cb) override;
    void stop() override;
    SourceCapability capability() const override;

private:
    void pollLoop();

    RtcSourceConfig cfg_;
    SampleCallback cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<SourceCapability> cap_{SourceCapability::kDisabled};
};

/// 来源管理器：注册、能力检测、启动与代次注入。
class SourceManager {
public:
    explicit SourceManager(TimeCore* core);

    /// 按配置注册并启动来源。检测结果汇总于 infos。
    void setupConfiguredSources(const CellularSourceConfig& cellular,
                                const GnssSourceConfig& gnss,
                                const RtcSourceConfig& rtc,
                                std::vector<SourceDetectInfo>* infos);

    void stopAll();

    /// 启动所有已注册且可用的来源（带代次注入回调）。
    void startAll();

    /// 代次注入 helper（NTP 等外部注册源复用）。
    static SampleCallback wrapGeneration(
        SampleCallback cb, std::shared_ptr<std::atomic<uint32_t>> gen);

private:
    TimeCore* core_;
    std::vector<std::unique_ptr<SourceAdapter>> adapters_;
    std::vector<std::unique_ptr<SourceAdapter>> ntp_adapters_;
};

}  // namespace time
}  // namespace tbox
