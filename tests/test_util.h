#pragma once

//
// 测试公共工具：可控单调钟 + MockBackend + TimeCore 装配。
//

#include <memory>
#include <vector>

#include "backends/mock_backend.h"
#include "core/time_core.h"
#include "tbox/time/source_sample.h"
#include "tbox/time/time_snapshot.h"

namespace tbox {
namespace time {
namespace testutil {

using namespace tbox::time;

/// 构造合格样本（未认证网络授时语义）。
inline SourceSample goodSample(const std::string& id, int64_t utc,
                               int64_t boot_sampled, int64_t err = 50,
                               AuthStatus auth = AuthStatus::kUnverified,
                               int epoch = 1, uint32_t gen = 1) {
    SourceSample s;
    s.kind = SourceKind::kNtpNetwork;
    s.source_id = id;
    s.source_epoch = epoch;
    s.adapter_generation = gen;
    s.utc_valid = true;
    s.utc_epoch_ms = utc;
    s.age_valid = true;
    s.sampled_boot_ms = boot_sampled;
    s.error_bounded = true;
    s.error_upper_ms = err;
    s.drift_bounded = true;
    s.drift_upper_ppm = 100;
    s.auth_status = auth;
    s.auth_verified = (auth == AuthStatus::kVerified);
    s.healthy = true;
    s.health = SourceHealth::kOk;
    return s;
}

/// 失效样本（源断开/失败）。generation 必须递增（旧回调语义）。
inline SourceSample failSample(const std::string& id, SourceHealth h,
                               const std::string& detail = "",
                               uint32_t gen = 1) {
    SourceSample s;
    s.kind = SourceKind::kNtpNetwork;
    s.source_id = id;
    s.adapter_generation = gen;
    s.healthy = false;
    s.health = h;
    s.health_detail = detail;
    return s;
}

/// 测试用哑来源适配器（用于注册 source_id/优先级）。
class DummySource final : public SourceAdapter {
public:
    DummySource(std::string id, SourceKind k = SourceKind::kNtpNetwork)
        : id_(std::move(id)), kind_(k) {}
    const std::string& sourceId() const override { return id_; }
    SourceKind kind() const override { return kind_; }
    bool start(SampleCallback cb) override {
        started_ = true;
        return true;
    }
    void stop() override { started_ = false; }
    SourceCapability capability() const override {
        return SourceCapability::kAvailable;
    }

private:
    std::string id_;
    SourceKind kind_;
    bool started_ = false;
};

/// 测试装配：可控单调钟 + 虚拟墙钟后端 + TimeCore。
struct Harness {
    int64_t boot = 1'000'000'000;  // 可控单调钟（BOOTTIME 语义）
    MockBackend backend;
    TimeCoreConfig cfg;
    std::unique_ptr<TimeCore> core;
    std::vector<TimeStateEvent> events;
    std::vector<std::unique_ptr<DummySource>> dummy_sources_;

    Harness(int64_t wall_init, TimeCoreConfig c = {}, bool with_default_ntp = true)
        : backend(wall_init, nullptr, /*auto_advance=*/false), cfg(c) {
        cfg.monitor_interval_ms = 1000;
        core = std::make_unique<TimeCore>(&backend, cfg,
                                          [this]() { return boot; });
        core->setStateChangedCallback(
            [this](const TimeStateEvent& ev) { events.push_back(ev); });
        core->start();
        if (with_default_ntp) {
            registerSource("ntp0", 10);
        }
    }

    /// 注册测试来源（id + 优先级），返回注册的适配器引用。
    DummySource& registerSource(const std::string& id, int priority,
                                SourceKind k = SourceKind::kNtpNetwork) {
        auto d = std::make_unique<DummySource>(id, k);
        DummySource* raw = d.get();
        core->registerSource(raw, priority);
        dummy_sources_.push_back(std::move(d));
        return *raw;
    }

    /// 推进单调钟与墙钟（模拟时间自然流逝）并 tick。
    void advance(int64_t ms) {
        boot += ms;
        backend.advanceWall(ms);
        core->tick();
    }

    void tick() { core->tick(); }

    void inject(const SourceSample& s) { core->onSourceSample(s); }

    TimeSnapshot snap() const { return core->snapshot(); }

    TimeState st() const { return core->state(); }
};

}  // namespace testutil
}  // namespace time
}  // namespace tbox
