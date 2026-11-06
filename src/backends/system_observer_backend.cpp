#include "backends/system_observer_backend.h"

#include <chrono>
#include <cstring>
#include <string>

#include "backends/observer_math.h"

// adjtimex / clock_gettime 由只读后端（capability/readClock）无条件使用，
// 其可用性只取决于 __linux__，与 libsystemd 是否存在无关：把这两个头
// 放进 HAVE_LIBSYSTEMD 守卫会导致缺 libsystemd 的交叉环境里
// 'adjtimex' was not declared in this scope。
#if defined(__linux__)
#include <sys/timex.h>
#include <time.h>
#endif

// sd-bus 仅 D-Bus 观测 I/O 需要。
#if defined(__linux__) && defined(HAVE_LIBSYSTEMD)
#include <systemd/sd-bus.h>
#endif

namespace tbox {
namespace time {

namespace {

constexpr const char* kObserverSourceId = "system-timesyncd";
constexpr const char* kTimesyncService = "org.freedesktop.timesync1";
constexpr const char* kTimesyncPath = "/org/freedesktop/timesync1";
constexpr const char* kTimesyncIface = "org.freedesktop.timesync1.Manager";
constexpr const char* kDbusService = "org.freedesktop.DBus";
constexpr const char* kDbusPath = "/org/freedesktop/DBus";
constexpr const char* kDbusIface = "org.freedesktop.DBus";

// 2000-01-01 .. 2100-01-01（usec epoch），与 SourceSample::validate 范围一致。
constexpr int64_t kMinEpochUsec = 946684800000000LL;
constexpr int64_t kMaxEpochUsec = 3471298560000000LL;

}  // namespace

// ============================================================================
// 纯函数：观测样本构造（设计 §3.2 SourceSample 映射 + §5 公式）
// ============================================================================

bool buildObserverSample(const ObserverFacts& f, int64_t fallback_drift_ppm,
                         SourceSample* out, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (!f.consistent) return fail("observation boundary inconsistent");
    if (!f.dbus_valid || !f.adjtimex_valid || !f.clock_valid) {
        return fail("incomplete observation facts");
    }
    // NTPMessage 必需项校验（设计 §3.1）
    if (f.leap == 3) return fail("leap indicator 3 (unsynchronized)");
    if (f.stratum == 0 || f.stratum == 16) return fail("invalid stratum");
    if (f.mode != 4) return fail("unexpected NTP mode (server expected)");
    if (!f.have_originate || !f.have_receive || !f.have_transmit ||
        !f.have_destination) {
        return fail("missing NTP timestamps");
    }
    if (f.ignored) return fail("NTPMessage ignored (diagnostic only)");
    if (f.destination_ts_us <= 0) return fail("destination timestamp zero");
    // DestinationTimestamp → UTC epoch ms（溢出/范围检查）
    if (f.destination_ts_us < kMinEpochUsec || f.destination_ts_us > kMaxEpochUsec) {
        return fail("destination timestamp out of range");
    }
    const int64_t dest_ms = f.destination_ts_us / 1000;

    // 真实测量新鲜度：age0 = R0 - D。墙钟只用于把真实测量时间戳映射为首次
    // 观察时的已有年龄；持续计龄只使用 BOOTTIME（设计 §5.2）。
    // 相同 DestinationTimestamp 的重复读取不得重置 age0/B0（source 维护锚点）；
    // 新测量才建立新锚点。
    int64_t age0_ms = f.realtime_ms - dest_ms;
    int64_t b0_ms = f.boot_ms;
    if (f.anchor_valid) {
        if (f.anchor_age0_ms < 0 || f.anchor_boot_ms < 0) {
            return fail("invalid measurement anchor");
        }
        age0_ms = f.anchor_age0_ms;
        b0_ms = f.anchor_boot_ms;
    }
    if (age0_ms < 0) return fail("destination timestamp newer than wall clock");

    // 保守不确定度（设计 §5.1）：提交时 Δ = 本次边界 BOOTTIME - B0（锚点）；
    // 查询时由核心按公式继续增长。
    const int64_t tol_ppm = observerTolerancePpm(f.tolerance_raw);
    const int64_t root_dist = observerRootDistanceUs(
        f.root_delay_us, f.root_dispersion_us, f.jitter_us);
    const int64_t unc = observerUncertaintyMs(
        age0_ms, b0_ms, f.boot_ms, f.maxerror_us, root_dist, tol_ppm);

    SourceSample s;
    s.kind = SourceKind::kNtpNetwork;
    s.source_id = kObserverSourceId;
    s.utc_valid = true;
    s.utc_epoch_ms = f.realtime_ms;  // 一致性边界读取的 CLOCK_REALTIME
    s.age_valid = true;
    // sampled_boot_ms 必须是「该样本的时间被测量的那一单调时刻」，不是本次观测
    // 时刻：核心的候选年龄门禁算的是 now - sampled_boot_ms
    // （time_core.cpp candidateOkLocked），若填本次观测边界 B0，age 恒约等于
    // 一个观测周期（~1s），max_source_age_ms 对观测路径等于失效 —— 实测会在
    // 真实测量已陈旧 500s+ 时仍报 SYNCHRONIZED，且与同一快照里按真实新鲜度
    // 计算的 remaining_validity_ms 自相矛盾。
    // 测量发生在 BOOTTIME 的 (B0 - age0)：建立锚点时单调钟为 B0，而该测量
    // 当时已有 age0 的年龄。这样 now - sampled_boot_ms 恒等于
    // observerFreshnessMs(age0, B0, now)，与 age_ms 同口径。
    // 契约要求（config_schema.md）：system-observe 下 max_source_age_ms
    // 按真实测量（DestinationTimestamp）计龄。
    // 不对负值做钳位：测量早于本次 boot 时 age 本就应偏大，钳位会低估年龄
    // （乐观方向），宁可让候选被门禁拒绝。
    s.sampled_boot_ms = b0_ms - age0_ms;
    s.age_ms = observerFreshnessMs(age0_ms, b0_ms, f.boot_ms);  // 提交时真实年龄
    s.error_bounded = unc >= 0;
    s.error_upper_ms = unc >= 0 ? unc : 0;
    // 漂移上界：优先 tolerance 换算；无效时用已配置且经验证的 fallback（core.drift_bound_ppm）。
    if (tol_ppm >= 0) {
        s.drift_bounded = true;
        s.drift_upper_ppm = tol_ppm;
    } else if (fallback_drift_ppm > 0) {
        s.drift_bounded = true;
        s.drift_upper_ppm = fallback_drift_ppm;
    }
    s.auth_status = AuthStatus::kUnverified;  // 普通 NTP 固定 unverified
    s.healthy = true;   // 证据完整；STA_UNSYNC 由 observed_sta_unsync 单独表达（核心判定对齐）
    s.health = SourceHealth::kOk;

    // 观测事实（查询时公式增长的输入）
    s.observed_sample = true;
    s.observed_sta_unsync = f.sta_unsync;
    s.observed_age0_ms = age0_ms;
    s.observed_boot_ms = b0_ms;
    s.observed_kernel_maxerror_us = f.maxerror_us;
    s.observed_root_distance_us = root_dist >= 0 ? root_dist : 0;
    s.observed_kernel_tolerance_ppm = tol_ppm >= 0 ? tol_ppm : 0;
    s.observed_alignment_error_ms = observerAlignmentErrorMs(f.offset_us);

    if (!s.validate()) return fail("sample validation failed");
    *out = s;
    return true;
}

// ============================================================================
// SystemObserverSource：Linux + libsystemd 真实轮询；其他平台 Unsupported
// ============================================================================

SystemObserverSource::SystemObserverSource(SystemObserverConfig cfg)
    : cfg_(std::move(cfg)) {
#if defined(__linux__) && defined(HAVE_LIBSYSTEMD)
    cap_ = SourceCapability::kAvailable;
#else
    cap_ = SourceCapability::kUnsupported;
#endif
}

SystemObserverSource::~SystemObserverSource() { stop(); }

const std::string& SystemObserverSource::sourceId() const {
    static const std::string kId = kObserverSourceId;
    return kId;
}

bool SystemObserverSource::start(SampleCallback cb) {
    if (running_.exchange(true)) return false;
    if (cap_ != SourceCapability::kAvailable) {
        running_ = false;
        return false;
    }
    cb_ = std::move(cb);
    thread_ = std::thread(&SystemObserverSource::pollLoop, this);
    return true;
}

void SystemObserverSource::stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    if (thread_.joinable()) thread_.join();
}

SourceCapability SystemObserverSource::capability() const { return cap_.load(); }

#if defined(__linux__) && defined(HAVE_LIBSYSTEMD)

namespace {

/// NTPMessage 必需字段。
///
/// 属性完整签名（Orin Nano / systemd 245 实机 busctl introspect 确认）：
///   (uuuuittayttttbtt)
/// 字段顺序（与同机 timedatectl show-timesync 逐项对账确认）：
///   u  Leap              u  Version      u  Mode        u  Stratum
///   i  Precision（有符号，实测 -23）
///   t  RootDelay(us)     t  RootDispersion(us)
///   ay ReferenceId（4 字节，实测 {100,122,36,196} = 0x647A24C4）
///   t  Originate         t  Receive      t  Transmit    t  Destination（us epoch）
///   b  Ignored           t  PacketCount  t  Jitter(us)
/// 注意 Jitter 是**最后一个**字段，不在 RootDispersion 之后；
/// Leap/Version/Mode/Stratum 是 u(uint32) 而非 y(byte)。
constexpr const char* kNtpMessageSignature = "(uuuuittayttttbtt)";
constexpr const char* kNtpMessageStructBody = "uuuuittayttttbtt";

struct NtpMessageFields {
    bool ok = false;
    uint8_t leap = 0;
    uint8_t version = 0;
    uint8_t mode = 0;
    uint8_t stratum = 0;
    int64_t root_delay_us = 0;
    int64_t root_dispersion_us = 0;
    int64_t jitter_us = 0;
    bool have_originate = false;
    int64_t originate_ts_us = 0;
    bool have_receive = false;
    int64_t receive_ts_us = 0;
    bool have_transmit = false;
    int64_t transmit_ts_us = 0;
    bool have_destination = false;
    int64_t destination_ts_us = 0;
    bool ignored = false;
    uint64_t packet_count = 0;
};

int readNtpMessage(sd_bus* bus, NtpMessageFields* out) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* m = nullptr;
    int r = sd_bus_get_property(bus, kTimesyncService, kTimesyncPath,
                                kTimesyncIface, "NTPMessage", &err, &m,
                                kNtpMessageSignature);
    if (r < 0) {
        sd_bus_error_free(&err);
        return r;
    }
    sd_bus_error_free(&err);

    uint32_t leap = 0, version = 0, mode = 0, stratum = 0;
    int32_t precision = 0;
    uint64_t root_delay = 0, root_disp = 0;
    uint64_t orig = 0, recv = 0, trans = 0, dest = 0;
    int ignored = 0;
    uint64_t packet_count = 0, jitter = 0;

    r = sd_bus_message_enter_container(m, 'r', kNtpMessageStructBody);
    if (r >= 0) {
        r = sd_bus_message_read(m, "uuuui", &leap, &version, &mode, &stratum,
                                &precision);
    }
    if (r >= 0) r = sd_bus_message_read(m, "tt", &root_delay, &root_disp);
    // ReferenceId：仅诊断用，不进入不确定度公式，跳过但必须消费以保持游标对齐。
    if (r >= 0) r = sd_bus_message_skip(m, "ay");
    if (r >= 0) r = sd_bus_message_read(m, "tttt", &orig, &recv, &trans, &dest);
    if (r >= 0) r = sd_bus_message_read(m, "btt", &ignored, &packet_count, &jitter);
    if (r >= 0) r = sd_bus_message_exit_container(m);
    sd_bus_message_unref(m);
    if (r < 0) return r;

    out->ok = true;
    out->leap = static_cast<uint8_t>(leap);
    out->version = static_cast<uint8_t>(version);
    out->mode = static_cast<uint8_t>(mode);
    out->stratum = static_cast<uint8_t>(stratum);
    out->root_delay_us = static_cast<int64_t>(root_delay);
    out->root_dispersion_us = static_cast<int64_t>(root_disp);
    out->jitter_us = static_cast<int64_t>(jitter);
    out->have_originate = true;
    out->originate_ts_us = static_cast<int64_t>(orig);
    out->have_receive = true;
    out->receive_ts_us = static_cast<int64_t>(recv);
    out->have_transmit = true;
    out->transmit_ts_us = static_cast<int64_t>(trans);
    out->have_destination = true;
    out->destination_ts_us = static_cast<int64_t>(dest);
    out->ignored = ignored != 0;
    out->packet_count = packet_count;
    return 0;
}

/// 读取字符串属性（ServerName，签名 s）。返回 0 成功；<0 失败。
int readStringProperty(sd_bus* bus, const char* name, std::string* out) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* m = nullptr;
    int r = sd_bus_get_property(bus, kTimesyncService, kTimesyncPath,
                                kTimesyncIface, name, &err, &m, "s");
    if (r < 0) {
        sd_bus_error_free(&err);
        return r;
    }
    sd_bus_error_free(&err);
    const char* s = nullptr;
    r = sd_bus_message_read(m, "s", &s);
    if (r >= 0 && s) *out = s;
    sd_bus_message_unref(m);
    return r < 0 ? r : 0;
}

/// 读取 ServerAddress（签名 (iay)：地址族 + 原始地址字节）并格式化为稳定字符串。
///
/// 该属性**不是** s：实机 busctl 确认为 (iay)，例如 `2 4 119 28 183 184`
/// = AF_INET + {119,28,183,184}。只用于来源标识与一致性边界比较（服务器变更
/// 即丢弃本轮混合样本），故格式化为可比较的稳定文本即可。
int readServerAddress(sd_bus* bus, std::string* out) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* m = nullptr;
    int r = sd_bus_get_property(bus, kTimesyncService, kTimesyncPath,
                                kTimesyncIface, "ServerAddress", &err, &m,
                                "(iay)");
    if (r < 0) {
        sd_bus_error_free(&err);
        return r;
    }
    sd_bus_error_free(&err);

    int32_t family = 0;
    const void* raw = nullptr;
    size_t len = 0;
    r = sd_bus_message_enter_container(m, 'r', "iay");
    if (r >= 0) r = sd_bus_message_read(m, "i", &family);
    if (r >= 0) r = sd_bus_message_read_array(m, 'y', &raw, &len);
    if (r >= 0) r = sd_bus_message_exit_container(m);
    if (r >= 0 && raw != nullptr) {
        const auto* b = static_cast<const unsigned char*>(raw);
        char buf[64];
        if (family == 2 /* AF_INET */ && len == 4) {
            std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            *out = buf;
        } else if (family == 10 /* AF_INET6 */ && len == 16) {
            std::string hex;
            hex.reserve(40);
            for (size_t i = 0; i < len; ++i) {
                std::snprintf(buf, sizeof(buf), "%02x", b[i]);
                hex += buf;
                if (i % 2 == 1 && i + 1 < len) hex += ':';
            }
            *out = hex;
        } else {
            // 未知族/长度：如实标注而非猜测，仍可用于变更比较。
            std::snprintf(buf, sizeof(buf), "family=%d,len=%zu", family, len);
            *out = buf;
        }
    }
    sd_bus_message_unref(m);
    return r < 0 ? r : 0;
}

/// 读取 D-Bus unique owner（timesyncd 实例代次）。返回 0 成功；<0 服务 absent。
///
/// sd_bus_call_method 没有超时参数（第 8 个形参是 const char* types）；
/// 超时经 sd_bus_set_method_call_timeout(bus, usec) 在连接上设置。
int readOwner(sd_bus* bus, std::string* out) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call_method(bus, kDbusService, kDbusPath, kDbusIface,
                               "GetNameOwner", &err, &reply, "s",
                               kTimesyncService);
    if (r < 0) {
        sd_bus_error_free(&err);
        return r;
    }
    sd_bus_error_free(&err);
    const char* owner = nullptr;
    r = sd_bus_message_read(reply, "s", &owner);
    if (r >= 0 && owner) *out = owner;
    sd_bus_message_unref(reply);
    return r;
}

}  // namespace

void SystemObserverSource::pollLoop() {
    sd_bus* bus = nullptr;
    uint64_t source_epoch = 1;
    std::string last_owner;
    uint64_t last_dest_us = 0;      ///< 已锚定的 DestinationTimestamp（新测量才重新锚定）
    int64_t anchor_age0_ms = 0;
    int64_t anchor_boot_ms = 0;
    int64_t next_poll = 0;
    bool first = true;

    while (running_.load()) {
        const int64_t now_ms = std::chrono::duration_cast<
                                   std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now()
                                       .time_since_epoch())
                                   .count();  // 仅轮询节奏，不进入样本事实
        if (!first && now_ms < next_poll) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        first = false;
        next_poll = now_ms + cfg_.poll_interval_ms;

        auto emitFail = [this](SourceHealth h, const std::string& detail) {
            if (!cb_) return;
            SourceSample s;
            s.kind = SourceKind::kNtpNetwork;
            s.source_id = kObserverSourceId;
            s.healthy = false;
            s.health = h;
            s.health_detail = detail;
            if (cb_) cb_(s);
        };

        // --- 连接（超时按证据不可用分类） ---
        if (!bus) {
            int r = sd_bus_open_system(&bus);
            if (r < 0) {
                emitFail(SourceHealth::kUnreachable,
                         "system bus unavailable");
                continue;
            }
            sd_bus_set_method_call_timeout(bus, cfg_.dbus_timeout_ms * 1000);
        }

        // --- 排空连接队列（防御性，非 B 的解法）---
        //
        // 本连接长生命周期，而 timesync1 的 NTPMessage 声明了 emits-change，
        // timesyncd 每接受新测量都会广播 PropertiesChanged。轮询客户端从不
        // 驱动 sd_bus_process 会让这些信号在入队队列里无界堆积，因此这里做
        // 有界排空属于正确用法。
        //
        // 但要如实记录：**这不能解决实测到的属性陈旧问题。**
        // 实机（Orin Nano / systemd 245）现象：PacketCount 前进后，本连接的
        // sd_bus_get_property 会在整个测量周期（100~500s+）持续返回旧的
        // DestinationTimestamp，而同时刻新建连接的 busctl 已能读到新值。
        // 加入本排空后复测，跳过现象依旧（PC=16 → 17 → 18 全程未更新），
        // 故"未排空队列导致陈旧"的假设已被实测否证，根因待定位。
        {
            int pr;
            // 有界排空：只处理已排队消息，不阻塞等待（不调用 sd_bus_wait）。
            while ((pr = sd_bus_process(bus, nullptr)) > 0) {
            }
            if (pr < 0) {
                // 连接已不可用：丢弃并下一周期重连，不在坏连接上继续读。
                sd_bus_flush_close_unref(bus);
                bus = nullptr;
                emitFail(SourceHealth::kUnreachable,
                         "system bus processing failed");
                continue;
            }
        }

        // --- 一致性边界第 1 读：NTPMessage + 服务器标识 ---
        ObserverFacts f;
        NtpMessageFields n1;
        if (readNtpMessage(bus, &n1) < 0) {
            emitFail(SourceHealth::kUnreachable,
                     "NTPMessage read failed (service absent or timeout)");
            continue;
        }
        std::string server_name_1, server_addr_1;
        if (readStringProperty(bus, "ServerName", &server_name_1) < 0 ||
            readServerAddress(bus, &server_addr_1) < 0) {
            emitFail(SourceHealth::kObserverInterfaceUnavailable,
                     "timesync1 interface missing required property");
            continue;
        }
        std::string owner;
        if (readOwner(bus, &owner) < 0) {
            emitFail(SourceHealth::kUnreachable,
                     "timesync1 service absent (GetNameOwner failed)");
            continue;
        }
        if (owner != last_owner) {
            // D-Bus owner 变化：timesyncd 实例代次 → source_epoch 递增；
            // 旧测量锚点失效（新实例代次），等待新的 DestinationTimestamp 重新锚定。
            source_epoch++;
            last_owner = owner;
            last_dest_us = 0;
        }

        // --- 只读 adjtimex(modes=0) + 一致性边界时钟读数 ---
        struct timex tx;
        std::memset(&tx, 0, sizeof(tx));
        tx.modes = 0;  // 只读查询（不写钟）
        const int adjrc = adjtimex(&tx);
        if (adjrc < 0) {
            emitFail(SourceHealth::kObserverInterfaceUnavailable,
                     "adjtimex(modes=0) failed");
            continue;
        }
        struct timespec rt, bt;
        if (clock_gettime(CLOCK_REALTIME, &rt) != 0 ||
            clock_gettime(CLOCK_BOOTTIME, &bt) != 0) {
            emitFail(SourceHealth::kObserverInterfaceUnavailable,
                     "clock_gettime failed");
            continue;
        }
        f.adjtimex_valid = true;
        f.sta_unsync = (tx.status & STA_UNSYNC) != 0;
        // maxerror/esterror/precision 恒为微秒；只有 offset 与 PPS jitter 的
        // 单位取决于 STA_NANO：置位时为纳秒，否则微秒（man 2 adjtimex）。
        // 实机实测（Orin Nano / systemd 245）status 在 0x2001 与 0x6001 间变化
        // —— timesyncd 以 ADJ_NANO 调用时 STA_NANO 置位，此时把 offset 当微秒
        // 处理会让 alignment_error_ms 虚高 1000 倍（实测 6.15ms 报成 6976ms）。
        // 注意不要把该换算套到 precision 上：precision 本就是微秒，
        // 实测值 1us 被误除会变成 0 并导致观测样本校验失败。
        const bool nano = (tx.status & STA_NANO) != 0;
        f.maxerror_us = tx.maxerror;
        f.offset_us = nano ? tx.offset / 1000 : tx.offset;
        f.tolerance_raw = tx.tolerance;
        f.precision_us = tx.precision;
        f.clock_valid = true;
        f.realtime_ms = static_cast<int64_t>(rt.tv_sec) * 1000 +
                        rt.tv_nsec / 1000000;
        f.boot_ms = static_cast<int64_t>(bt.tv_sec) * 1000 + bt.tv_nsec / 1000000;

        // --- 一致性边界第 2 读：再读 NTPMessage/服务器标识 ---
        NtpMessageFields n2;
        std::string server_name_2, server_addr_2;
        if (readNtpMessage(bus, &n2) < 0 ||
            readStringProperty(bus, "ServerName", &server_name_2) < 0 ||
            readServerAddress(bus, &server_addr_2) < 0) {
            emitFail(SourceHealth::kObserverInterfaceUnavailable,
                     "timesync1 re-read failed (boundary)");
            continue;
        }
        // DestinationTimestamp / PacketCount / 服务器标识变化 → 丢弃混合样本，
        // 下一有界周期重试（设计 §3.1）。
        if (n1.destination_ts_us != n2.destination_ts_us ||
            n1.packet_count != n2.packet_count ||
            server_name_1 != server_name_2 || server_addr_1 != server_addr_2) {
            continue;  // 不提交本轮（有界重试）
        }
        f.consistent = true;
        f.dbus_valid = true;
        f.server_name = server_name_2;
        f.server_address = server_addr_2;
        f.dbus_owner = owner;
        f.packet_count = n2.packet_count;
        f.leap = n2.leap;
        f.version = n2.version;
        f.mode = n2.mode;
        f.stratum = n2.stratum;
        f.root_delay_us = n2.root_delay_us;
        f.root_dispersion_us = n2.root_dispersion_us;
        f.jitter_us = n2.jitter_us;
        f.have_originate = n2.have_originate;
        f.originate_ts_us = n2.originate_ts_us;
        f.have_receive = n2.have_receive;
        f.receive_ts_us = n2.receive_ts_us;
        f.have_transmit = n2.have_transmit;
        f.transmit_ts_us = n2.transmit_ts_us;
        f.have_destination = n2.have_destination;
        f.destination_ts_us = n2.destination_ts_us;
        f.ignored = n2.ignored;

        // 测量锚点（设计 §5.2）：新 DestinationTimestamp 才建立新锚点
        // （首次观测边界记录 R0/B0）；相同报文重复读取不得重置 age0/B0。
        if (f.destination_ts_us != last_dest_us) {
            last_dest_us = f.destination_ts_us;
            anchor_age0_ms = f.realtime_ms - f.destination_ts_us / 1000;
            anchor_boot_ms = f.boot_ms;
        }
        f.anchor_valid = true;
        f.anchor_age0_ms = anchor_age0_ms;
        f.anchor_boot_ms = anchor_boot_ms;

        // --- 构造样本 ---
        SourceSample sample;
        std::string berr;
        if (!buildObserverSample(f, cfg_.drift_upper_ppm, &sample, &berr)) {
            // 证据不完整/非法：可诊断的不可用（不伪造新鲜度/误差）
            emitFail(SourceHealth::kInvalidSample, berr);
            continue;
        }
        sample.source_epoch = source_epoch;
        sample.evidence_ref =
            "systemd-timesyncd:owner=" + owner +
            ":server=" + f.server_name +
            ":addr=" + f.server_address +
            ":dest_us=" + std::to_string(f.destination_ts_us);
        if (cb_) cb_(sample);
    }

    if (bus) {
        sd_bus_flush_close_unref(bus);
    }
}

#else  // !(Linux && libsystemd)

void SystemObserverSource::pollLoop() {
    while (running_.load()) {
        // 非 Linux / 无 libsystemd：观测 unsupported（capability 表达，
        // 不持续报故障；daemon 仍正常启动并明确报告时间不可用）。
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

#endif

// ============================================================================
// SystemObserverBackend：只读后端（不写钟，不检测竞争写入者）
// ============================================================================

BackendCapability SystemObserverBackend::capability() const {
#if defined(__linux__)
    struct timex tx;
    std::memset(&tx, 0, sizeof(tx));
    tx.modes = 0;
    if (adjtimex(&tx) < 0) return BackendCapability::kFault;
    return BackendCapability::kReady;
#else
    return BackendCapability::kUnsupported;
#endif
}

ClockRead SystemObserverBackend::readClock() {
#if defined(__linux__)
    struct timespec rt, bt;
    if (clock_gettime(CLOCK_REALTIME, &rt) != 0 ||
        clock_gettime(CLOCK_BOOTTIME, &bt) != 0) {
        return ClockRead{false, 0, 0, "clock_gettime failed"};
    }
    ClockRead r;
    r.valid = true;
    r.realtime_epoch_ms =
        static_cast<int64_t>(rt.tv_sec) * 1000 + rt.tv_nsec / 1000000;
    r.boot_monotonic_ms =
        static_cast<int64_t>(bt.tv_sec) * 1000 + bt.tv_nsec / 1000000;
    return r;
#else
    return ClockRead{false, 0, 0,
                     "non-Linux: read-only observe backend unsupported"};
#endif
}

BackendResult SystemObserverBackend::stepTo(int64_t utc_epoch_ms) {
    (void)utc_epoch_ms;
    return BackendResult{false, 0,
                         "read-only observe mode: no clock write"};
}

BackendResult SystemObserverBackend::slewBy(int64_t offset_ms) {
    (void)offset_ms;
    return BackendResult{false, 0,
                         "read-only observe mode: no clock write"};
}

BackendResult SystemObserverBackend::verifyAlignment(
    int64_t utc_epoch_ms, int64_t error_upper_ms, int64_t* alignment_error_ms) {
    (void)utc_epoch_ms;
    (void)error_upper_ms;
    (void)alignment_error_ms;
    return BackendResult{false, 0,
                         "read-only observe mode: no alignment verify"};
}

bool SystemObserverBackend::detectCompetingWriters(std::string* detail) {
    if (detail) {
        *detail = "systemd-timesyncd is the expected writer "
                  "(system-observe, no competing-writer scan)";
    }
    return false;
}

}  // namespace time
}  // namespace tbox
