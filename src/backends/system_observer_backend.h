#pragma once

//
// TBOX-TIME-DSN-CR-002：系统时钟观测后端（SystemClockObserver）。
//
// Orin 目标形态：TIME 是系统时钟同步状态的观测方与时间事实分发方，不是写钟方。
// 观测接口：只读 adjtimex(modes=0)（内核纪律状态）+ D-Bus
// org.freedesktop.timesync1（systemd-timesyncd NTP 证据）。
//
// 组件：
//   - 纯函数部分（observer_math.h / buildObserverSample）：全平台可编译、可单测，
//     实现设计 §3.1 一致性边界验证与 §5.1/§5.2 保守不确定度/真实测量新鲜度公式；
//   - SystemObserverSource（SourceAdapter）：Linux + libsystemd 下真实轮询
//     （D-Bus 属性 + 只读 adjtimex），非 Linux/无 libsystemd → capability=Unsupported
//     （如实标记，同 LinuxSystemClockBackend 模式）；
//   - SystemObserverBackend（BackendAdapter，只读）：提供 readClock 供外部改时
//     检测；step/slew/verifyAlignment 一律拒绝（观测形态不写钟，不实例化
//     writer/校准逻辑）；竞争检测恒 false（systemd-timesyncd 是合法且预期的写入方）。
//

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "tbox/time/backend.h"
#include "tbox/time/source_adapter.h"
#include "tbox/time/source_sample.h"
#include "tbox/time/types.h"

namespace tbox {
namespace time {

/// 观测周期一致性边界收集的事实（设计 §3.1：NTPMessage → adjtimex → 再读 NTPMessage）。
struct ObserverFacts {
    // D-Bus timesync1 Manager 属性
    bool dbus_valid = false;         ///< 已连接到 timesync1 且读到服务器标识/NTPMessage
    std::string server_name;
    std::string server_address;
    std::string dbus_owner;          ///< unique owner（timesyncd 实例代次识别）
    uint64_t packet_count = 0;
    // NTPMessage 必需字段（Leap/Version/Mode/Stratum/Precision/RootDelay/RootDispersion/
    // Jitter/四个时间戳/Ignored/PacketCount）
    uint8_t leap = 0;
    uint8_t version = 0;
    uint8_t mode = 0;
    uint8_t stratum = 0;
    int64_t root_delay_us = 0;       ///< RootDelay（usec）
    int64_t root_dispersion_us = 0;  ///< RootDispersion（usec）
    int64_t jitter_us = 0;           ///< NTPMessage.Jitter（usec；esterror/内核 jitter 不进入公式）
    bool have_originate = false;
    int64_t originate_ts_us = 0;
    bool have_receive = false;
    int64_t receive_ts_us = 0;
    bool have_transmit = false;
    int64_t transmit_ts_us = 0;
    bool have_destination = false;
    int64_t destination_ts_us = 0;   ///< DestinationTimestamp（usec epoch，真实测量时刻）
    bool ignored = false;            ///< Ignored=yes 只作诊断，不得作为当前有效测量

    // 内核纪律状态（只读 adjtimex(modes=0)）
    bool adjtimex_valid = false;
    bool sta_unsync = false;         ///< STA_UNSYNC 置位（内核未纪律）
    int64_t maxerror_us = 0;         ///< 误差上界（内核维护，随 tolerance 增长）
    int64_t offset_us = 0;           ///< 有符号偏差（PLL 正在修正的值）
    int64_t tolerance_raw = 0;       ///< 内核刻度（32768000 = 500ppm）
    int64_t precision_us = 0;

    // 一致性边界时钟读数（同一边界记录 REALTIME 与 BOOTTIME）
    bool clock_valid = false;
    int64_t realtime_ms = 0;         ///< CLOCK_REALTIME epoch 毫秒
    int64_t boot_ms = 0;             ///< CLOCK_BOOTTIME 毫秒（含休眠）

    // 测量锚点（设计 §5.2）：相同 DestinationTimestamp 的重复读取不得重置
    // age0/B0（新测量才建立新锚点）；source 维护，builder 优先使用。
    bool anchor_valid = false;
    int64_t anchor_age0_ms = 0;      ///< 首次观测该测量的已有年龄 age0 = R0 - D
    int64_t anchor_boot_ms = 0;      ///< 首次观测该测量的 BOOTTIME B0

    // 双读一致性：再读 NTPMessage/服务器标识后若 DestinationTimestamp、PacketCount
    // 或服务器标识变化 → 丢弃本轮混合样本（设计 §3.1）。
    bool consistent = true;
};

/// 观测来源配置（设计 §6）。
struct SystemObserverConfig {
    std::string driver = "timesyncd";   ///< 当前唯一 driver；其他值 unsupported 并拒绝
    int64_t poll_interval_ms = 1000;    ///< [100,10000]；只决定观测频率，不改变来源真实年龄
    int64_t dbus_timeout_ms = 1000;     ///< [100,5000]；必须有界，超时按证据不可用分类
    int64_t drift_upper_ppm = 100;      ///< tolerance 不可用时的配置上界（core.drift_bound_ppm）
};

/// 观测样本构造（设计 §3.2 SourceSample 映射 + §5 公式）。
/// fallback_drift_ppm：tolerance 不可用时的已配置且经验证漂移上界
/// （core.drift_bound_ppm）。返回 false 时 out 不保证有效；error 为脱敏诊断。
/// healthy=false 的失败样本仍可经结构校验（用于触发 HOLDOVER/UNSYNCHRONIZED/FAULT）。
bool buildObserverSample(const ObserverFacts& f, int64_t fallback_drift_ppm,
                         SourceSample* out, std::string* error);

/// timesyncd 系统观测来源（SourceAdapter）。
/// Linux + libsystemd：真实轮询线程（D-Bus timesync1 属性 + 只读 adjtimex）；
/// 非 Linux / 无 libsystemd：capability=Unsupported（如实标记，不占位标完成）。
class SystemObserverSource final : public SourceAdapter {
public:
    explicit SystemObserverSource(SystemObserverConfig cfg);
    ~SystemObserverSource() override;

    const std::string& sourceId() const override;
    SourceKind kind() const override { return SourceKind::kNtpNetwork; }
    bool start(SampleCallback cb) override;
    void stop() override;
    SourceCapability capability() const override;

private:
    void pollLoop();

    SystemObserverConfig cfg_;
    SampleCallback cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<SourceCapability> cap_{SourceCapability::kDisabled};
};

/// 只读观测后端（BackendAdapter，system-observe 形态）。
/// readClock 提供真实墙钟/单调钟（外部改时检测基准）；写钟方法一律拒绝；
/// 竞争检测恒 false（timesyncd 是合法且预期的写入方，不得因其存在进入 FAULT）。
class SystemObserverBackend final : public BackendAdapter {
public:
    SystemObserverBackend() = default;
    ~SystemObserverBackend() override = default;

    BackendKind kind() const override { return BackendKind::kSystemObserve; }
    BackendCapability capability() const override;

    ClockRead readClock() override;
    BackendResult stepTo(int64_t utc_epoch_ms) override;
    BackendResult slewBy(int64_t offset_ms) override;
    BackendResult verifyAlignment(int64_t utc_epoch_ms, int64_t error_upper_ms,
                                  int64_t* alignment_error_ms) override;
    bool detectCompetingWriters(std::string* detail) override;
};

}  // namespace time
}  // namespace tbox
