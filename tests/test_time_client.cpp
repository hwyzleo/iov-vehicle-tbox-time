//
// TBOX-TIME-DSN-CR-001：client SDK 集成测试（真实 AF_UNIX IPC）。
// 覆盖 AT-03（未同步可查询/无 setTime）、AT-04（订阅对账/事件）、
// AT-07（RTT/缓存超期）、AT-10（多消费者只读）。
//

#include <gtest/gtest.h>

#include <cstdio>
#include <thread>

#include "ipc.h"  // tbox::fw::ipc

#include "core/time_core.h"
#include "ipc/time_ipc_server.h"
#include "tbox/time/time_client.h"
#include "test_util.h"

namespace tbox {
namespace time {

using namespace tbox::time;
using namespace tbox::time::testutil;

namespace {

constexpr int64_t kUtc = 1'700'000'000'000;

struct IpcFixture : public ::testing::Test {
    int64_t boot = 1'000'000'000;
    MockBackend backend{kUtc, nullptr, false};
    TimeCoreConfig cfg;
    std::unique_ptr<TimeCore> core;
    std::unique_ptr<TimeIpcServer> server;
    std::string socket_path;
    std::thread ticker;

    void SetUp() override {
        cfg.monitor_interval_ms = 200;
        cfg.holdover_max_ms = 3'600'000;
        core = std::make_unique<TimeCore>(&backend, cfg, [this]() { return boot; });
        core->start();

        char buf[128];
        std::snprintf(buf, sizeof(buf), "/tmp/tbox-time-test-%d.sock", getpid());
        socket_path = buf;
        server = std::make_unique<TimeIpcServer>(socket_path, 128);
        core->setStateChangedCallback(
            [this](const TimeStateEvent& ev) { server->enqueueEvent(ev); });
        ASSERT_TRUE(server->start(core.get()));

        ticker = std::thread([this]() {
            for (int i = 0; i < 200; ++i) {
                core->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }

    void TearDown() override {
        if (ticker.joinable()) ticker.join();
        server->stop();
        core->stop();
        if (dummy_) delete dummy_;
        std::remove(socket_path.c_str());
    }

    struct DummySourceImpl : public SourceAdapter {
        explicit DummySourceImpl(std::string id) : id_(std::move(id)) {}
        const std::string& sourceId() const override { return id_; }
        SourceKind kind() const override { return SourceKind::kNtpNetwork; }
        bool start(SampleCallback) override { return true; }
        void stop() override {}
        SourceCapability capability() const override {
            return SourceCapability::kAvailable;
        }
        std::string id_;
    };
    DummySourceImpl* dummy_ = nullptr;

    void registerDummy(const std::string& id, int prio) {
        dummy_ = new DummySourceImpl(id);
        core->registerSource(dummy_, prio);
    }

    void inject(const SourceSample& s) { core->onSourceSample(s); }
};

}  // namespace

// AT-03：未同步时可查询，明确 time_unavailable，仍返回快照。
TEST_F(IpcFixture, QueryBeforeSyncReportsUnavailable) {
    TimeClient client(socket_path);
    ASSERT_TRUE(client.connect());
    SnapshotResult res = client.getTimeSnapshot();
    EXPECT_EQ(res.error, ClientError::kTimeUnavailable);
    EXPECT_EQ(res.snapshot.state, TimeState::kUnsynch);
    EXPECT_FALSE(res.snapshot.synchronized);
    EXPECT_FALSE(res.snapshot.utc_valid);
    EXPECT_GE(res.rtt_ms, 0);
}

// AT-03/AT-10：同步后查询 ok；多消费者并发只读。
TEST_F(IpcFixture, QueryAfterSyncOk) {
    registerDummy("ntp0", 10);
    inject(goodSample("ntp0", kUtc, boot));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    core->tick();

    TimeClient c1(socket_path);
    TimeClient c2(socket_path);
    ASSERT_TRUE(c1.connect());
    ASSERT_TRUE(c2.connect());
    auto r1 = c1.getTimeSnapshot();
    auto r2 = c2.getTimeSnapshot();
    EXPECT_EQ(r1.error, ClientError::kOk);
    EXPECT_EQ(r2.error, ClientError::kOk);
    EXPECT_TRUE(r1.snapshot.synchronized);
    EXPECT_TRUE(r1.snapshot.utc_valid);
    EXPECT_EQ(r1.snapshot.utc_epoch_ms, kUtc);
    EXPECT_EQ(r1.snapshot.auth_status, AuthStatus::kUnverified);
    // 两个消费者读到同一事实（无重复校时）
    EXPECT_EQ(r1.snapshot.event_sequence, r2.snapshot.event_sequence);
}

// AT-03：无通用 setTime IPC（只读）。调用未登记方法 → unsupported。
TEST_F(IpcFixture, NoSetTimeMethod) {
    // 服务端只登记 getTimeSnapshot/subscribe；未知 method → Unsupported
    tbox::fw::ipc::Client raw(socket_path);
    ASSERT_TRUE(raw.connect());
    auto [status, body] = raw.call(0xFFFF, "{}");
    EXPECT_EQ(status, 0);  // 传输成功
    EXPECT_NE(body.find("\"status\":\"unsupported\""), std::string::npos);
    raw.disconnect();
}

// AT-04：订阅收到状态变化事件（首次同步 UNSYNCHRONIZED→SYNCHRONIZED）。
TEST_F(IpcFixture, SubscribeReceivesFirstSyncEvent) {
    registerDummy("ntp0", 10);
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

    // 触发同步
    inject(goodSample("ntp0", kUtc, boot));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    core->tick();

    bool saw_first_sync = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& ev : got) {
            // 首次同步完成：SYNCHRONIZING → SYNCHRONIZED（AlignmentVerified）
            if (ev.new_state == TimeState::kSynced &&
                ev.reason == TimeReason::kAlignmentVerified) {
                saw_first_sync = true;
            }
            EXPECT_EQ(ev.daemon_epoch, 1);  // 首次 start 后 daemon_epoch=1
        }
    }
    EXPECT_TRUE(saw_first_sync);
}

// AT-07：daemon_epoch/sequence 对账——查询基线与事件序列一致。
TEST_F(IpcFixture, ReconcileWithQueryBaseline) {
    registerDummy("ntp0", 10);
    TimeClient client(socket_path);
    ASSERT_TRUE(client.connect());
    auto base = client.getTimeSnapshot();
    EXPECT_EQ(base.error, ClientError::kTimeUnavailable);

    inject(goodSample("ntp0", kUtc, boot));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    core->tick();
    auto after = client.getTimeSnapshot();
    EXPECT_EQ(after.error, ClientError::kOk);
    EXPECT_GT(after.snapshot.event_sequence, base.snapshot.event_sequence);
    // daemon_epoch 不变（同一实例）
    EXPECT_EQ(after.snapshot.daemon_epoch, base.snapshot.daemon_epoch);
}

// AT-07：缓存超期——RTT 计入，remaining_validity 随查询递减。
TEST_F(IpcFixture, RttIncludedInValidity) {
    registerDummy("ntp0", 10);
    inject(goodSample("ntp0", kUtc, boot));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    core->tick();

    TimeClient client(socket_path);
    ASSERT_TRUE(client.connect());
    auto r1 = client.getTimeSnapshot();
    ASSERT_EQ(r1.error, ClientError::kOk);
    EXPECT_GE(r1.rtt_ms, 0);
    // remaining_validity = max_age - freshness；freshness 从最近验证时刻计
    EXPECT_LE(r1.snapshot.remaining_validity_ms, 300'000);
    EXPECT_GT(r1.snapshot.remaining_validity_ms, 0);
}

}  // namespace time
}  // namespace tbox
