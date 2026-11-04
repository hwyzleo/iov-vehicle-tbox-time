#include "sources/ntp_source.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace tbox {
namespace time {

namespace {

int64_t monotonicMs() {
    struct timespec ts;
#if defined(CLOCK_BOOTTIME)
    clock_gettime(CLOCK_BOOTTIME, &ts);
#else
    // 平台无 suspend 计时的 steady 时钟：唤醒失效旧样本由上层策略处理
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t realtimeMs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// NTP 64 位时间戳（1900 epoch）：32 位秒 + 32 位分数。
int64_t ntpSecToUnixMs(uint32_t sec, uint32_t frac) {
    const int64_t unix_sec = static_cast<int64_t>(sec) - 2208988800LL;
    const int64_t frac_ms = (static_cast<int64_t>(frac) * 1000) >> 32;
    return unix_sec * 1000 + frac_ms;
}

#pragma pack(push, 1)
struct NtpPacket {
    uint8_t li_vn_mode;
    uint8_t stratum;
    int8_t poll;
    int8_t precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_time_sec;
    uint32_t ref_time_frac;
    uint32_t orig_time_sec;
    uint32_t orig_time_frac;
    uint32_t rx_time_sec;
    uint32_t rx_time_frac;
    uint32_t tx_time_sec;
    uint32_t tx_time_frac;
};
#pragma pack(pop)

static_assert(sizeof(NtpPacket) == 48, "NTP packet must be 48 bytes");

void encodeNtpTimestamp(uint32_t* sec, uint32_t* frac, int64_t unix_ms) {
    const int64_t ntp_sec = unix_ms / 1000 + 2208988800LL;
    const int64_t rem_ms = unix_ms % 1000;
    *sec = static_cast<uint32_t>(ntp_sec);
    *frac = static_cast<uint32_t>((rem_ms << 32) / 1000);
}

}  // namespace

int64_t NtpClient::defaultBootMs() { return monotonicMs(); }

NtpClient::NtpClient(NtpSourceConfig cfg, int64_t (*boot_ms)())
    : cfg_(std::move(cfg)), boot_ms_(boot_ms ? boot_ms : defaultBootMs) {}

NtpClient::~NtpClient() {
    if (sock_ >= 0) ::close(sock_);
}

bool NtpClient::query(SourceSample& out) {
    out = SourceSample{};
    out.kind = SourceKind::kNtpNetwork;
    out.source_id = cfg_.source_id;
    out.source_epoch = 1;
    out.auth_status = AuthStatus::kUnverified;  // 普通 NTP 未认证
    out.auth_verified = false;
    out.drift_bounded = true;
    out.drift_upper_ppm = cfg_.drift_upper_ppm;

    if (cfg_.host.empty()) {
        out.healthy = false;
        out.health = SourceHealth::kInterfaceMissing;
        out.health_detail = "no server host configured";
        return false;
    }

    struct addrinfo hints, *res = nullptr;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%u", cfg_.port);
    const int gai = getaddrinfo(cfg_.host.c_str(), port_str, &hints, &res);
    if (gai != 0) {
        out.healthy = false;
        out.health = SourceHealth::kUnreachable;
        out.health_detail = "resolve failed";
        return false;
    }

    bool ok = false;
    for (auto* ai = res; ai != nullptr; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            ::close(fd);
            continue;
        }
        struct timeval tv;
        tv.tv_sec = cfg_.timeout_ms / 1000;
        tv.tv_usec = (cfg_.timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        // T1（发送时刻）
        const int64_t t1 = realtimeMs();
        const int64_t t1_boot = boot_ms_();
        NtpPacket req;
        std::memset(&req, 0, sizeof(req));
        req.li_vn_mode = (4 << 3) | 3;  // VN=4, Mode=3 (client)
        encodeNtpTimestamp(&req.tx_time_sec, &req.tx_time_frac, t1);

        const ssize_t sent = ::send(fd, &req, sizeof(req), 0);
        if (sent != sizeof(req)) {
            ::close(fd);
            continue;
        }
        NtpPacket resp;
        std::memset(&resp, 0, sizeof(resp));
        const ssize_t got = ::recv(fd, &resp, sizeof(resp), 0);
        // T4（接收时刻）
        const int64_t t4 = realtimeMs();
        const int64_t t4_boot = boot_ms_();
        ::close(fd);
        if (got < static_cast<ssize_t>(sizeof(resp))) {
            out.healthy = false;
            out.health = SourceHealth::kTimeout;
            out.health_detail = "no response within timeout";
            break;
        }
        if (got != sizeof(resp)) {
            out.healthy = false;
            out.health = SourceHealth::kInvalidSample;
            out.health_detail = "short packet";
            break;
        }

        const uint8_t mode = resp.li_vn_mode & 0x07;
        const uint8_t stratum = resp.stratum;
        if (mode != 4) {
            out.healthy = false;
            out.health = SourceHealth::kInvalidSample;
            out.health_detail = "not a server response";
            break;
        }
        if (stratum == 0) {  // kiss-of-death
            out.healthy = false;
            out.health = SourceHealth::kAuthFailed;
            out.health_detail = "kiss-of-death (server rejected)";
            break;
        }
        if (stratum > 15) {
            out.healthy = false;
            out.health = SourceHealth::kInvalidSample;
            out.health_detail = "invalid stratum";
            break;
        }

        const int64_t t2 = ntpSecToUnixMs(resp.rx_time_sec, resp.rx_time_frac);
        const int64_t t3 = ntpSecToUnixMs(resp.tx_time_sec, resp.tx_time_frac);
        const int64_t offset = ((t2 - t1) + (t3 - t4)) / 2;
        int64_t delay = (t4 - t1) - (t3 - t2);
        if (delay > 10000) {
            out.healthy = false;
            out.health = SourceHealth::kInvalidSample;
            out.health_detail = "implausible delay";
            break;
        }
        if (delay < 0) {
            // 本地/低延迟路径可能出现负 RTT（服务器处理时间 > 往返）；
            // RFC 5905 语义下按 0 clamp，误差上界仍保守。
            delay = 0;
        }

        // 服务器重启检测（ref_time 大幅倒退 → source_epoch++）
        const int64_t ref_unix_ms =
            ntpSecToUnixMs(resp.ref_time_sec, resp.ref_time_frac);
        if (ref_unix_ms > 0) {
            if (last_ref_ms_ > 0 && ref_unix_ms + 5000 < last_ref_ms_) {
                out.source_epoch = next_epoch_++;
            }
            last_ref_ms_ = ref_unix_ms;
        }

        out.utc_valid = true;
        out.utc_epoch_ms = t4 + offset;
        out.age_valid = true;
        out.sampled_boot_ms = t4_boot;
        out.age_ms = 0;  // 年龄由核心按单调计龄计算
        out.error_bounded = true;
        out.error_upper_ms = delay / 2 + cfg_.queueing_budget_ms;
        out.healthy = true;
        out.health = SourceHealth::kOk;
        ok = true;
        break;
    }
    freeaddrinfo(res);
    return ok;
}

// ============================================================================
// 来源适配器
// ============================================================================

NtpSourceAdapter::NtpSourceAdapter(NtpSourceConfig cfg)
    : cfg_(std::move(cfg)), client_(cfg_), cap_(SourceCapability::kDisabled) {}

NtpSourceAdapter::~NtpSourceAdapter() { stop(); }

const std::string& NtpSourceAdapter::sourceId() const { return cfg_.source_id; }

bool NtpSourceAdapter::start(SampleCallback cb) {
    if (running_.exchange(true)) return false;
    if (cfg_.host.empty()) {
        cap_ = SourceCapability::kUnsupported;
        running_ = false;
        return false;
    }
    cb_ = std::move(cb);
    cap_ = SourceCapability::kDegraded;
    thread_ = std::thread(&NtpSourceAdapter::pollLoop, this);
    return true;
}

void NtpSourceAdapter::stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    if (thread_.joinable()) thread_.join();
}

SourceCapability NtpSourceAdapter::capability() const {
    return cap_.load();
}

void NtpSourceAdapter::pollLoop() {
    SourceSample sample;
    int64_t next_poll = 0;
    int64_t now = NtpClient::defaultBootMs();
    bool first = true;
    while (running_.load()) {
        now = NtpClient::defaultBootMs();
        if (first || now >= next_poll) {
            first = false;
            const bool ok = client_.query(sample);
            if (ok) {
                cap_ = SourceCapability::kAvailable;
            } else {
                cap_ = SourceCapability::kDegraded;
            }
            if (cb_) cb_(sample);
            next_poll = now + cfg_.poll_interval_ms;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

}  // namespace time
}  // namespace tbox
