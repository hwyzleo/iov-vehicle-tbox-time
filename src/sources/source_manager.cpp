#include "sources/source_manager.h"

#include <atomic>
#include <cstring>
#include <thread>

#include "core/time_core.h"
#include "sources/ntp_source.h"

#if defined(__linux__)
#include <fcntl.h>
#include <linux/rtc.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#endif

namespace tbox {
namespace time {

// ============================================================================
// 平台检测（真实代码；缺硬件返回 unsupported，不阻塞启动）
// ============================================================================

SourceCapability detectCellularModule(std::string* detail) {
#if defined(__linux__)
    // 常见蜂窝模块设备节点（厂商契约登记前仅检测存在性）
    const char* patterns[] = {"/dev/ttyACM", "/dev/ttyUSB", "/dev/cdc-wdm"};
    for (const char* p : patterns) {
        for (int i = 0; i < 16; ++i) {
            char path[64];
            std::snprintf(path, sizeof(path), "%s%d", p, i);
            if (access(path, F_OK) == 0) {
                if (detail) *detail = std::string("module node: ") + path;
                return SourceCapability::kAvailable;
            }
        }
    }
    if (detail) *detail = "no cellular module device found";
    return SourceCapability::kUnsupported;
#else
    if (detail) *detail = "non-Linux platform: cellular unsupported";
    return SourceCapability::kUnsupported;
#endif
}

SourceCapability detectGnssModule(std::string* detail) {
#if defined(__linux__)
    const char* patterns[] = {"/dev/gnss", "/dev/ttyS", "/dev/ttyTHS"};
    for (const char* p : patterns) {
        for (int i = 0; i < 16; ++i) {
            char path[64];
            std::snprintf(path, sizeof(path), "%s%d", p, i);
            if (access(path, F_OK) == 0) {
                if (detail) *detail = std::string("gnss node: ") + path;
                return SourceCapability::kAvailable;
            }
        }
    }
    if (detail) *detail = "no gnss device found";
    return SourceCapability::kUnsupported;
#else
    if (detail) *detail = "non-Linux platform: gnss unsupported";
    return SourceCapability::kUnsupported;
#endif
}

// RTC 设备默认路径集中在此，避免「探测用一个路径、读取用另一个路径」的漂移。
//
// 必须用 /dev/rtc（内核指定的系统 RTC 符号链接）而不是 /dev/rtc0：Orin Nano
// 上存在两个 RTC —— rtc0 = nvvrs-pseq-rtc（PMIC 电源时序计数器，hctosys=0，
// 实测读出 1970-01-01）、rtc1 = tegra_rtc（真实 RTC，hctosys=1，内核用它初始化
// 系统钟），/dev/rtc -> rtc1。写死 rtc0 会把 1970 当作合法 UTC 样本喂进选源。
const char* kDefaultRtcDevice = "/dev/rtc";

std::string rtcDevicePath(const std::string& configured) {
    return configured.empty() ? std::string(kDefaultRtcDevice) : configured;
}

SourceCapability detectRtcModule(std::string* detail, const std::string& device) {
#if defined(__linux__)
    const std::string path = rtcDevicePath(device);
    if (access(path.c_str(), F_OK) == 0) {
        if (detail) *detail = "rtc node: " + path;
        return SourceCapability::kAvailable;
    }
    if (detail) *detail = "no rtc node at " + path;
    return SourceCapability::kUnsupported;
#else
    (void)device;
    if (detail) *detail = "non-Linux platform: rtc unsupported";
    return SourceCapability::kUnsupported;
#endif
}

// ============================================================================
// 蜂窝来源（协议为厂商契约：缺口登记，不占位标完成）
// ============================================================================

CellularSourceAdapter::CellularSourceAdapter(CellularSourceConfig cfg)
    : cfg_(std::move(cfg)), cap_(SourceCapability::kDisabled) {
    std::string det;
    const SourceCapability c = detectCellularModule(&det);
    if (!cfg_.enabled) {
        cap_ = SourceCapability::kDisabled;
    } else if (c == SourceCapability::kAvailable) {
        // 检测到模块；厂商时间协议（AT+CCLK/CMT 等）契约待登记，
        // 未登记前如实标记 unsupported（docs/implementation_notes.md）。
        cap_ = SourceCapability::kUnsupported;
    } else {
        cap_ = SourceCapability::kUnsupported;
    }
}

const std::string& CellularSourceAdapter::sourceId() const { return cfg_.source_id; }

bool CellularSourceAdapter::start(SampleCallback cb) {
    if (started_) return false;
    if (cap_ != SourceCapability::kAvailable) return false;  // 无模块/契约缺口
    cb_ = std::move(cb);
    started_ = true;
    // 厂商时间协议未登记：此处为扩展点，真实采样实现待契约（AT+CCLK/CMT）。
    // 不在未确认接口上凭空实现（项目约束）。
    return false;
}

void CellularSourceAdapter::stop() { started_ = false; }

SourceCapability CellularSourceAdapter::capability() const { return cap_; }

// ============================================================================
// GNSS 来源（驱动/协议契约登记前保留扩展点）
// ============================================================================

GnssSourceAdapter::GnssSourceAdapter(GnssSourceConfig cfg)
    : cfg_(std::move(cfg)), cap_(SourceCapability::kDisabled) {
    std::string det;
    const SourceCapability c = detectGnssModule(&det);
    if (!cfg_.enabled) {
        cap_ = SourceCapability::kDisabled;
    } else if (c == SourceCapability::kAvailable) {
        // 检测到设备；GNSS 时间协议（NMEA RMC/GGA 等）契约待登记。
        cap_ = SourceCapability::kUnsupported;
    } else {
        cap_ = SourceCapability::kUnsupported;
    }
}

const std::string& GnssSourceAdapter::sourceId() const { return cfg_.source_id; }

bool GnssSourceAdapter::start(SampleCallback cb) {
    if (started_) return false;
    if (cap_ != SourceCapability::kAvailable) return false;
    cb_ = std::move(cb);
    started_ = true;
    // 驱动/协议契约未登记：扩展点，真实采样待实现（NMEA）。
    return false;
}

void GnssSourceAdapter::stop() { started_ = false; }

SourceCapability GnssSourceAdapter::capability() const { return cap_; }

// ============================================================================
// RTC 来源（Linux 真实读取 /dev/rtc0；provision 证据接口登记缺口）
// ============================================================================

RtcSourceAdapter::RtcSourceAdapter(RtcSourceConfig cfg)
    : cfg_(std::move(cfg)), cap_(SourceCapability::kDisabled) {
    std::string det;
    const SourceCapability c = detectRtcModule(&det, cfg_.device);
    if (!cfg_.enabled) {
        cap_ = SourceCapability::kDisabled;
    } else if (c == SourceCapability::kAvailable) {
        cap_ = SourceCapability::kAvailable;
    } else {
        cap_ = SourceCapability::kUnsupported;
    }
}

RtcSourceAdapter::~RtcSourceAdapter() { stop(); }

const std::string& RtcSourceAdapter::sourceId() const { return cfg_.source_id; }

bool RtcSourceAdapter::start(SampleCallback cb) {
    if (running_.exchange(true)) return false;
    if (cap_ != SourceCapability::kAvailable) {
        running_ = false;
        return false;
    }
    cb_ = std::move(cb);
    thread_ = std::thread(&RtcSourceAdapter::pollLoop, this);
    return true;
}

void RtcSourceAdapter::stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    if (thread_.joinable()) thread_.join();
}

SourceCapability RtcSourceAdapter::capability() const { return cap_.load(); }

void RtcSourceAdapter::pollLoop() {
    // Linux 真实读取（/dev/rtc0 + RTC_RD_TIME）。provision/电池健康证据接口
    // 为平台契约（登记缺口）：require_provisioned=true 时无证据 → 不可用。
    // 首次实施：基础读取 + 配置 gate；无 provision 证据默认拒绝作为后备源。
#if defined(__linux__)
    const std::string dev_path = rtcDevicePath(cfg_.device);
    const char* dev = dev_path.c_str();
    int64_t next_poll = 0;
    bool first = true;
    while (running_.load()) {
        const int64_t now = NtpClient::defaultBootMs();
        if (first || now >= next_poll) {
            first = false;
            SourceSample sample;
            sample.kind = SourceKind::kRtc;
            sample.source_id = cfg_.source_id;
            sample.source_epoch = 1;
            sample.drift_bounded = true;
            sample.drift_upper_ppm = cfg_.drift_upper_ppm;
            if (cfg_.require_provisioned) {
                // provision 证据接口未登记（缺口）：如实标记，不伪装健康
                sample.healthy = false;
                sample.health = SourceHealth::kInterfaceMissing;
                sample.health_detail = "rtc provision evidence interface pending";
            } else {
                // 基础读取路径（无 provision 要求时）：ioctl RTC_RD_TIME
                int fd = open(dev, O_RDONLY);
                if (fd >= 0) {
                    struct rtc_time tm;
                    if (ioctl(fd, RTC_RD_TIME, &tm) == 0) {
                        struct tm t;
                        std::memset(&t, 0, sizeof(t));
                        t.tm_year = tm.tm_year + 1900 - 1900;
                        t.tm_mon = tm.tm_mon;
                        t.tm_mday = tm.tm_mday;
                        t.tm_hour = tm.tm_hour;
                        t.tm_min = tm.tm_min;
                        t.tm_sec = tm.tm_sec;
                        // 变量名避开 unix：GCC 在 GNU 方言（-std=gnu++17）下
                        // 预定义 unix 宏（展开为 1），同名变量会解析失败。
                        const time_t unix_secs = timegm(&t);
                        sample.utc_valid = true;
                        sample.utc_epoch_ms = static_cast<int64_t>(unix_secs) * 1000;
                        sample.age_valid = true;
                        sample.sampled_boot_ms = now;
                        sample.error_bounded = true;
                        sample.error_upper_ms = cfg_.error_upper_ms;
                        sample.healthy = true;
                        sample.health = SourceHealth::kOk;
                    } else {
                        sample.healthy = false;
                        sample.health = SourceHealth::kFault;
                        sample.health_detail = "RTC_RD_TIME failed";
                    }
                    ::close(fd);
                } else {
                    sample.healthy = false;
                    sample.health = SourceHealth::kInterfaceMissing;
                    sample.health_detail = "open rtc device failed";
                }
            }
            if (cb_) cb_(sample);
            next_poll = now + 60000;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
#else
    while (running_.load()) {
        // 非 Linux 平台：RTC unsupported（由 capability 表达，不持续报故障）
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
#endif
}

// ============================================================================
// SourceManager：注册 + 代次注入
// ============================================================================

SourceManager::SourceManager(TimeCore* core) : core_(core) {}

/// 回调代次注入：旧请求不能覆盖当前来源（设计 §6 / AT-06）。
/// 每来源独立递增；sample.adapter_generation 由注入层设置。
SampleCallback SourceManager::wrapGeneration(
    SampleCallback cb, std::shared_ptr<std::atomic<uint32_t>> gen) {
    return [cb = std::move(cb), gen = std::move(gen)](const SourceSample& s) {
        SourceSample sample = s;
        sample.adapter_generation = gen->fetch_add(1) + 1;
        if (cb) cb(sample);
    };
}

void SourceManager::startAll() {
    for (auto& a : adapters_) {
        if (!a || a->capability() != SourceCapability::kAvailable) continue;
        auto gen = std::make_shared<std::atomic<uint32_t>>(0);
        auto cb = wrapGeneration(
            [core = core_](const SourceSample& s) { core->onSourceSample(s); }, gen);
        a->start(std::move(cb));
    }
}

void SourceManager::setupConfiguredSources(
    const CellularSourceConfig& cellular, const GnssSourceConfig& gnss,
    const RtcSourceConfig& rtc, std::vector<SourceDetectInfo>* infos) {
    auto add = [&](std::unique_ptr<SourceAdapter> adapter, int priority) {
        if (!adapter) return;
        SourceDetectInfo info;
        info.config_enabled = true;
        info.capability = adapter->capability();
        info.detail = "";
        if (infos) infos->push_back(info);
        adapters_.push_back(std::move(adapter));
        core_->registerSource(adapters_.back().get(), priority);
    };

    auto cellular_adapter = std::make_unique<CellularSourceAdapter>(cellular);
    if (cellular_adapter->capability() == SourceCapability::kAvailable ||
        cellular_adapter->capability() == SourceCapability::kUnsupported) {
        // 未配置/无模块：注册但不启动（Disabled/Unsupported 表达）
        add(std::move(cellular_adapter), cellular.priority);
    }

    auto gnss_adapter = std::make_unique<GnssSourceAdapter>(gnss);
    if (gnss_adapter->capability() == SourceCapability::kAvailable ||
        gnss_adapter->capability() == SourceCapability::kUnsupported) {
        add(std::move(gnss_adapter), gnss.priority);
    }

    auto rtc_adapter = std::make_unique<RtcSourceAdapter>(rtc);
    if (rtc_adapter->capability() == SourceCapability::kAvailable) {
        add(std::move(rtc_adapter), rtc.priority);
    } else {
        // RTC 无硬件：登记检测结果但不注册（避免无意义轮询）
        if (infos) {
            SourceDetectInfo info;
            info.config_enabled = rtc.enabled;
            info.capability = rtc_adapter->capability();
            infos->push_back(info);
        }
    }
}

void SourceManager::stopAll() {
    for (auto& a : adapters_) {
        if (a) a->stop();
    }
    adapters_.clear();
}

}  // namespace time
}  // namespace tbox
