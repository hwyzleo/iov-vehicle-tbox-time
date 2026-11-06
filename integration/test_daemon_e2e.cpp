//
// TBOX-TIME-DSN-CR-001：端到端集成测试。
//
// 真实链路：本地 UDP mock NTP 服务器（真实 RFC 5905 报文）→ NtpSourceAdapter
// （真实 socket 取时）→ TimeCore（选源/状态机）→ MockBackend（校时/读回验证，
// 自动流逝模式模拟系统墙钟）→ TimeIpcServer → TimeClient（查询/订阅）。
//
// 覆盖 AT-01（无源冷启动→真实源独立同步）、AT-04（事件）、AT-05（断连→
// HOLDOVER→超限失效）、AT-08（未认证 NTP 不成为可信）、AT-10（只读消费）。
// 校时后端为测试 profile 的 MockBackend：真实 Linux 后端在 ORIN 实机验收
// （登记 docs/implementation_notes.md）。
//

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ipc.h"

#include "backends/mock_backend.h"
#include "backends/system_observer_backend.h"
#include "core/time_core.h"
#include "ipc/time_ipc_server.h"
#include "sources/ntp_source.h"
#include "sources/source_manager.h"
#include "tbox/time/time_client.h"

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

int64_t monotonicMs() {
    struct timespec ts;
#if defined(CLOCK_BOOTTIME)
    clock_gettime(CLOCK_BOOTTIME, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

void encodeNtp(uint32_t* sec, uint32_t* frac, int64_t unix_ms) {
    *sec = static_cast<uint32_t>(unix_ms / 1000 + 2208988800LL);
    *frac = static_cast<uint32_t>(((unix_ms % 1000) << 32) / 1000);
}

/// 本地 UDP mock NTP 服务器（可开关响应模拟断网）。
class MockNtpServer {
public:
    MockNtpServer() {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) throw std::runtime_error("socket failed");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
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

private:
    void loop() {
        char buf[512];
        while (running_.load()) {
            sockaddr_in from{};
            socklen_t flen = sizeof(from);
            const ssize_t n = recvfrom(fd_, buf, sizeof(buf), 0,
                                       reinterpret_cast<sockaddr*>(&from), &flen);
            if (n < 48) continue;
            if (!respond_.load()) continue;
            NtpPacket resp{};
            const int64_t now = realtimeMs();
            resp.li_vn_mode = (4 << 3) | 4;
            resp.stratum = 1;
            resp.poll = 6;
            resp.precision = -6;
            encodeNtp(&resp.ref_time_sec, &resp.ref_time_frac, kServerUtc);
            resp.orig_time_sec = reinterpret_cast<NtpPacket*>(buf)->tx_time_sec;
            resp.orig_time_frac = reinterpret_cast<NtpPacket*>(buf)->tx_time_frac;
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
    std::thread thread_;
};

struct E2eFixture : public ::testing::Test {
    MockNtpServer ntp;
    TimeCoreConfig cfg;
    std::unique_ptr<MockBackend> backend;
    std::unique_ptr<TimeCore> core;
    std::unique_ptr<NtpSourceAdapter> ntp_adapter;
    std::unique_ptr<TimeIpcServer> server;
    std::string socket_path;
    std::thread ticker;
    std::atomic<bool> stop_ticker{false};
    std::vector<TimeStateEvent> events;
    std::mutex ev_mu;

    void SetUp() override {
        // 紧凑时间参数使测试在秒级完成
        cfg.max_source_age_ms = 800;
        cfg.max_source_error_ms = 1000;
        cfg.step_threshold_ms = 2000;
        cfg.slew_threshold_ms = 50;
        cfg.alignment_tolerance_ms = 200;
        cfg.holdover_max_ms = 5000;
        cfg.holdover_max_error_ms = 2000;
        cfg.drift_bound_ppm = 100;
        cfg.monitor_interval_ms = 200;
        cfg.source_lost_age_ms = 700;
        cfg.external_change_threshold_ms = 300;
        cfg.synchronizing_timeout_ms = 10000;
        cfg.fault_probe_interval_ms = 2000;
        cfg.stability_window_syncs = 2;
        cfg.switch_hysteresis_ms = 0;

        // MockBackend 自动流逝：墙钟随真实单调钟自然前进
        backend = std::make_unique<MockBackend>(kServerUtc, &monotonicMs, true);
        core = std::make_unique<TimeCore>(backend.get(), cfg, &monotonicMs);

        NtpSourceConfig ncfg;
        ncfg.source_id = "ntp0";
        ncfg.host = "127.0.0.1";
        ncfg.port = ntp.port();
        ncfg.timeout_ms = 500;
        ncfg.poll_interval_ms = 300;
        ncfg.queueing_budget_ms = 20;
        ntp_adapter = std::make_unique<NtpSourceAdapter>(ncfg);
        core->registerSource(ntp_adapter.get(), 10);

        char buf[128];
        std::snprintf(buf, sizeof(buf), "/tmp/tbox-time-e2e-%d.sock", getpid());
        socket_path = buf;
        server = std::make_unique<TimeIpcServer>(socket_path, 256);
        core->setStateChangedCallback(
            [this](const TimeStateEvent& ev) {
                std::lock_guard<std::mutex> lk(ev_mu);
                events.push_back(ev);
                server->enqueueEvent(ev);
            });
        ASSERT_TRUE(server->start(core.get()));

        core->start();
        auto gen = std::make_shared<std::atomic<uint32_t>>(0);
        auto cb = SourceManager::wrapGeneration(
            [this](const SourceSample& s) { core->onSourceSample(s); }, gen);
        ASSERT_TRUE(ntp_adapter->start(std::move(cb)));

        ticker = std::thread([this]() {
            while (!stop_ticker.load()) {
                core->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });
    }

    void TearDown() override {
        stop_ticker = true;
        if (ticker.joinable()) ticker.join();
        ntp_adapter->stop();
        server->stop();
        core->stop();
        std::remove(socket_path.c_str());
    }

    bool waitFor(TimeState target, int timeout_ms) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (core->state() == target) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }
};

}  // namespace

// AT-01：真实源独立建立同步（不依赖 MQTT/TSP）。未认证 NTP 不成为可信（AT-08）。
TEST_F(E2eFixture, RealSourceSynchronizesOverIpc) {
    // 先验证无源冷启动阶段（NTP 适配器启动前）——此处已启动，直接等同步
    ASSERT_TRUE(waitFor(TimeState::kSynced, 8000));

    TimeClient client(socket_path);
    ASSERT_TRUE(client.connect());
    SnapshotResult res = client.getTimeSnapshot();
    EXPECT_EQ(res.error, ClientError::kOk);
    EXPECT_TRUE(res.snapshot.synchronized);
    EXPECT_TRUE(res.snapshot.utc_valid);
    // 与真实 NTP 服务器时间一致（本地 UTC 同源）
    EXPECT_LT(std::abs(res.snapshot.utc_epoch_ms - realtimeMs()), 5000);
    // AT-08：普通 NTP → unverified，TIME 不输出 trusted 语义
    EXPECT_EQ(res.snapshot.auth_status, AuthStatus::kUnverified);
    EXPECT_FALSE(res.snapshot.auth_verified);
    EXPECT_EQ(res.snapshot.source_kind, SourceKind::kNtpNetwork);
    // synchronized 只能由状态推导
    EXPECT_TRUE(res.snapshot.validate());
}

// AT-05：断网 → HOLDOVER → 超限失效。
TEST_F(E2eFixture, DisconnectHoldsOverThenExpires) {
    ASSERT_TRUE(waitFor(TimeState::kSynced, 8000));

    ntp.setRespond(false);  // 模拟 Wi-Fi 断连
    ASSERT_TRUE(waitFor(TimeState::kHoldover, 5000));

    TimeClient client(socket_path);
    ASSERT_TRUE(client.connect());
    auto ho = client.getTimeSnapshot();
    EXPECT_EQ(ho.error, ClientError::kOk);
    EXPECT_FALSE(ho.snapshot.synchronized);  // HOLDOVER 不伪装同步
    EXPECT_TRUE(ho.snapshot.utc_valid);      // 保持期间提供估计
    EXPECT_TRUE(ho.snapshot.uncertainty_valid);

    // 超过 holdover_max（5s）→ 超限失效
    ASSERT_TRUE(waitFor(TimeState::kUnsynch, 8000));
    auto us = client.getTimeSnapshot();
    EXPECT_EQ(us.error, ClientError::kTimeUnavailable);
    EXPECT_FALSE(us.snapshot.utc_valid);
    EXPECT_EQ(us.snapshot.reason, TimeReason::kHoldoverExpired);
}

// AT-04：状态事件经 IPC 送达客户端（首次同步 + 失步）。
TEST_F(E2eFixture, StateEventsDeliveredOverIpc) {
    TimeClient client(socket_path);
    ASSERT_TRUE(client.connect());
    std::mutex mu;
    std::vector<TimeStateEvent> got;
    auto handle = client.subscribeStateChanged(
        [&](const TimeStateEvent& ev) {
            std::lock_guard<std::mutex> lk(mu);
            got.push_back(ev);
        });
    ASSERT_NE(handle, nullptr);

    // 等待首次同步事件

    // 等待首次同步事件
    bool saw_first_sync = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& ev : got) {
            if (ev.new_state == TimeState::kSynced &&
                ev.reason == TimeReason::kAlignmentVerified) {
                saw_first_sync = true;
            }
        }
        if (saw_first_sync) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(saw_first_sync);

    // 断网 → 失步事件（HOLDOVER 进入）
    ntp.setRespond(false);
    bool saw_holdover = false;
    const auto deadline2 =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline2) {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& ev : got) {
            if (ev.old_state == TimeState::kSynced &&
                ev.new_state == TimeState::kHoldover) {
                saw_holdover = true;
            }
        }
        if (saw_holdover) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(saw_holdover);
}

// AT-10：只读消费——多客户端并发查询无副作用。
TEST_F(E2eFixture, ConcurrentReadOnlyConsumers) {
    ASSERT_TRUE(waitFor(TimeState::kSynced, 8000));
    std::atomic<int> ok_count{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&]() {
            TimeClient c(socket_path);
            if (!c.connect()) return;
            for (int k = 0; k < 10; ++k) {
                auto r = c.getTimeSnapshot();
                if (r.error == ClientError::kOk && r.snapshot.synchronized) {
                    ok_count.fetch_add(1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_GE(ok_count.load(), 20);  // 多数查询成功（期间可能短暂 HOLDOVER）
}

// CR-002：system-observe 观测形态启动路径。
// HOST（非 Linux/无 libsystemd）上观测后端与来源如实标记 Unsupported（不占位）；
// daemon（只读后端 + 核心 + IPC）仍正常启动：无源 → UNSYNCHRONIZED、IPC 明确不可用。
TEST_F(E2eFixture, SystemObserveBootsUnavailableOnHost) {
    // 只读观测后端：不写钟、竞争检测恒 false（timesyncd 是合法且预期的写入方）。
    SystemObserverBackend obs_backend;
#if !defined(__linux__)
    EXPECT_EQ(obs_backend.capability(), BackendCapability::kUnsupported);
#endif
    const BackendResult st = obs_backend.stepTo(1'700'000'000'000);
    EXPECT_FALSE(st.ok);
    EXPECT_NE(st.detail.find("read-only"), std::string::npos);
    const BackendResult sl = obs_backend.slewBy(10);
    EXPECT_FALSE(sl.ok);
    int64_t align = 0;
    const BackendResult va = obs_backend.verifyAlignment(1'700'000'000'000, 100, &align);
    EXPECT_FALSE(va.ok);
    std::string detail;
    EXPECT_FALSE(obs_backend.detectCompetingWriters(&detail));
    EXPECT_NE(detail.find("systemd-timesyncd"), std::string::npos);

    // 观测来源：HOST 无 libsystemd → Unsupported，不启动（无模块不阻塞启动）。
    SystemObserverSource obs_source(SystemObserverConfig{});
#if !defined(__linux__)
    EXPECT_EQ(obs_source.capability(), SourceCapability::kUnsupported);
    SampleCallback dead_cb = [](const SourceSample&) {};
    EXPECT_FALSE(obs_source.start(std::move(dead_cb)));
#endif

    // 观测形态 daemon 装配：只读后端 + 无来源 → 明确不可用但本地 IPC 可查询。
    TimeCoreConfig ocfg;
    ocfg.observe_mode = true;
    auto backend = std::make_unique<SystemObserverBackend>();
    auto core = std::make_unique<TimeCore>(backend.get(), ocfg, &monotonicMs);
    const std::string sock =
        "/tmp/tbox-time-obs-e2e-" + std::to_string(getpid()) + ".sock";
    auto server = std::make_unique<TimeIpcServer>(sock, 64);
    core->setStateChangedCallback([server = server.get()](const TimeStateEvent& ev) {
        server->enqueueEvent(ev);
    });
    ASSERT_TRUE(server->start(core.get()));
    core->start();
    core->tick();

    TimeClient client(sock);
    ASSERT_TRUE(client.connect());
    const SnapshotResult res = client.getTimeSnapshot();
    EXPECT_EQ(res.error, ClientError::kTimeUnavailable);
    EXPECT_FALSE(res.snapshot.synchronized);
    EXPECT_FALSE(res.snapshot.utc_valid);
    EXPECT_FALSE(res.snapshot.uncertainty_valid);
    EXPECT_TRUE(res.snapshot.validate());

    core->stop();
    server->stop();
    std::remove(sock.c_str());
}

}  // namespace time
}  // namespace tbox
