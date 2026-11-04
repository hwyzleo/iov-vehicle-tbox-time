//
// TBOX-TIME-DSN-CR-001：真实 NTP 取时路径测试。
// 本地 UDP mock NTP 服务器（RFC 5905 报文）→ NtpClient 真实采样：
// 验证报文构造/解析、offset 计算、误差上界（RTT/2 + 排队预算）、
// 认证状态（普通 NTP = unverified）、服务器重启 → source_epoch 递增。
//

#include <gtest/gtest.h>

#include <stdexcept>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "sources/ntp_source.h"

namespace tbox {
namespace time {

using namespace tbox::time;

namespace {

constexpr int64_t kServerUtc = 1'700'000'000'000;

int64_t realtimeMs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 与 NtpClient 相同布局的 48 字节报文。
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

void encodeNtp(uint32_t* sec, uint32_t* frac, int64_t unix_ms) {
    *sec = static_cast<uint32_t>(unix_ms / 1000 + 2208988800LL);
    *frac = static_cast<uint32_t>(((unix_ms % 1000) << 32) / 1000);
}

/// 本地 UDP mock NTP 服务器。可控制响应行为。
class MockNtpServer {
public:
    MockNtpServer() {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) throw std::runtime_error("socket failed");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // 随机端口
        if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            throw std::runtime_error("bind failed");
        socklen_t len = sizeof(addr);
        getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        running_ = true;
        struct timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  // 100ms：析构时 recvfrom 可退出，避免 join 死锁
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        thread_ = std::thread(&MockNtpServer::loop, this);
    }

    ~MockNtpServer() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) ::close(fd_);
    }

    uint16_t port() const { return port_; }
    void setRespond(bool v) { respond_.store(v); }
    void setRefTime(int64_t unix_ms) { ref_time_.store(unix_ms); }
    int64_t lastRequestOrigSec() const { return last_orig_sec_.load(); }
    uint32_t requestCount() const { return req_count_.load(); }

private:
    void loop() {
        char buf[512];
        while (running_.load()) {
            sockaddr_in from{};
            socklen_t flen = sizeof(from);
            const ssize_t n =
                recvfrom(fd_, buf, sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &flen);
            if (n < 48) continue;
            NtpPacket req;
            std::memcpy(&req, buf, sizeof(req));
            last_orig_sec_.store(req.tx_time_sec);
            req_count_.fetch_add(1);
            if (!respond_.load()) continue;  // 模拟断网/无响应

            NtpPacket resp{};
            const int64_t now = realtimeMs();
            resp.li_vn_mode = (4 << 3) | 4;  // VN=4, Mode=4 (server)
            resp.stratum = 1;
            resp.poll = 6;
            resp.precision = -6;
            resp.root_delay = 0;
            resp.root_dispersion = 0;
            encodeNtp(&resp.ref_time_sec, &resp.ref_time_frac, ref_time_.load());
            resp.orig_time_sec = req.tx_time_sec;   // 回显 T1
            resp.orig_time_frac = req.tx_time_frac;
            encodeNtp(&resp.rx_time_sec, &resp.rx_time_frac, now - 1);
            encodeNtp(&resp.tx_time_sec, &resp.tx_time_frac, now);
            sendto(fd_, &resp, sizeof(resp), 0,
                   reinterpret_cast<sockaddr*>(&from), flen);
        }
    }

    int fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> respond_{true};
    std::atomic<int64_t> ref_time_{kServerUtc};
    std::atomic<uint32_t> last_orig_sec_{0};
    std::atomic<uint32_t> req_count_{0};
    std::thread thread_;
};

}  // namespace

// 真实取时：本地 NTP 服务器 → 采样成功、UTC 接近服务器时间、误差有界。
TEST(NtpClient, RealQueryAgainstMockServer) {
    MockNtpServer server;
    NtpSourceConfig cfg;
    cfg.source_id = "ntp0";
    cfg.host = "127.0.0.1";
    cfg.port = server.port();
    cfg.timeout_ms = 2000;
    cfg.queueing_budget_ms = 20;

    NtpClient client(cfg);
    SourceSample out;
    ASSERT_TRUE(client.query(out));
    EXPECT_TRUE(out.utc_valid);
    // 服务器时间与本地墙钟同源（本地真实 UTC），偏差应在合理范围
    const int64_t wall = realtimeMs();
    EXPECT_LT(std::abs(out.utc_epoch_ms - wall), 5000);
    EXPECT_TRUE(out.error_bounded);
    EXPECT_GT(out.error_upper_ms, 0);  // RTT/2 + 排队预算
    EXPECT_TRUE(out.age_valid);
    EXPECT_TRUE(out.healthy);
    // 普通 NTP：明确未认证（AT-08：不得成为 SEC Trusted 依据）
    EXPECT_EQ(out.auth_status, AuthStatus::kUnverified);
    EXPECT_FALSE(out.auth_verified);
}

// 断网/超时：无响应服务器 → 明确失败，不产生可用样本。
TEST(NtpClient, TimeoutOnUnresponsiveServer) {
    MockNtpServer server;
    server.setRespond(false);
    NtpSourceConfig cfg;
    cfg.source_id = "ntp0";
    cfg.host = "127.0.0.1";
    cfg.port = server.port();
    cfg.timeout_ms = 300;

    NtpClient client(cfg);
    SourceSample out;
    EXPECT_FALSE(client.query(out));
    EXPECT_FALSE(out.healthy);
    EXPECT_FALSE(out.utc_valid);
}

// 服务器重启：ref_time 大幅倒退 → source_epoch 递增（代次失效）。
TEST(NtpClient, SourceEpochIncrementsOnServerRestart) {
    MockNtpServer server;
    NtpSourceConfig cfg;
    cfg.source_id = "ntp0";
    cfg.host = "127.0.0.1";
    cfg.port = server.port();
    cfg.timeout_ms = 2000;

    NtpClient client(cfg);
    SourceSample s1;
    ASSERT_TRUE(client.query(s1));
    EXPECT_EQ(s1.source_epoch, 1);

    // 模拟服务器重启：ref_time 回退 1 小时
    server.setRefTime(kServerUtc - 3'600'000);
    SourceSample s2;
    ASSERT_TRUE(client.query(s2));
    EXPECT_GT(s2.source_epoch, s1.source_epoch);
}

// 适配器：轮询驱动能力状态（Available → Degraded）。
TEST(NtpClient, AdapterCapabilityLifecycle) {
    MockNtpServer server;
    NtpSourceConfig cfg;
    cfg.source_id = "ntp0";
    cfg.host = "127.0.0.1";
    cfg.port = server.port();
    cfg.timeout_ms = 500;
    cfg.poll_interval_ms = 200;

    NtpSourceAdapter adapter(cfg);
    EXPECT_EQ(adapter.capability(), SourceCapability::kDisabled);

    std::atomic<int> samples{0};
    ASSERT_TRUE(adapter.start(
        [&](const SourceSample&) { samples.fetch_add(1); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    EXPECT_GE(samples.load(), 1);
    EXPECT_EQ(adapter.capability(), SourceCapability::kAvailable);

    adapter.stop();
}

}  // namespace time
}  // namespace tbox
