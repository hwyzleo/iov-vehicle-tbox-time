#include "core/time_core.h"

#include <algorithm>
#include <mutex>

#include "tbox/time/time_snapshot.h"

namespace tbox {
namespace time {

namespace {

// 漂移模型 U(t) = U0 + drift_ppm * elapsed_ms / 1e6（设计 §4）。
int64_t holdoverUncertainty(int64_t base_uncertainty_ms, int64_t drift_ppm,
                            int64_t elapsed_ms) {
    if (elapsed_ms <= 0) return base_uncertainty_ms;
    const int64_t drift_ms = drift_ppm * elapsed_ms / 1000000;
    return base_uncertainty_ms + drift_ms;
}

int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }

}  // namespace

class TimeCore::Impl {
public:
    Impl(BackendAdapter* backend, TimeCoreConfig cfg, BootClock boot_clock)
        : backend(backend), cfg(cfg), boot_clock(std::move(boot_clock)) {}

    // ---- 状态与数据 ----
    BackendAdapter* backend;
    TimeCoreConfig cfg;
    BootClock boot_clock;

    struct PerSource {
        SourceAdapter* adapter = nullptr;
        int priority = 0;
        uint32_t generation = 0;         // 适配器回调代次
        std::optional<SourceSample> last;  // 最近合格样本
        int stable_count = 0;            // 稳定窗口计数
        int64_t stable_since_boot_ms = 0;
    };
    std::vector<std::unique_ptr<PerSource>> sources;
    std::mutex mu;

    TimeSnapshot snap;
    bool running = false;

    std::function<void(const TimeStateEvent&)> event_cb;

    // 校准
    int64_t sync_start_boot_ms = 0;
    int calibrate_attempts = 0;
    // 保持
    int64_t holdover_start_boot_ms = 0;
    int64_t holdover_base_uncertainty_ms = 0;
    int64_t holdover_base_utc_ms = 0;
    // 对齐基准（外部改时检测）
    int64_t last_verified_wall_ms = 0;
    int64_t last_verified_boot_ms = 0;
    bool has_verified_baseline = false;
    // 最近一次成功校准的同步依据（HOLDOVER/质量摘要用，来源失效样本不覆盖）
    std::optional<SourceSample> sync_basis;
    // 故障探测
    int64_t last_fault_probe_boot_ms = 0;
    // 其他
    uint64_t daemon_epoch = 0;  // startLocked 时 ++ → 1；重启递增（服务实例标识）
    int64_t start_boot_ms = 0;

    int64_t nowBoot() const { return boot_clock(); }

    PerSource* findSource(const std::string& id) {
        for (auto& s : sources) {
            if (s->adapter->sourceId() == id) return s.get();
        }
        return nullptr;
    }

    // ---- 事件/提交 ----
    void commitLocked(TimeState new_state, TimeReason reason,
                      const std::string& detail, const SourceSample* src,
                      int64_t alignment_ms, bool force) {
        const TimeState old_state = snap.state;
        const std::string old_source = snap.source_id;
        const bool fact_changed =
            force || old_state != new_state || snap.reason != reason ||
            (src != nullptr && snap.source_id != src->source_id) ||
            (src != nullptr && snap.source_epoch != src->source_epoch) ||
            (old_state == new_state && reason == TimeReason::kSourceChanged);

        if (!fact_changed) return;

        if (new_state == TimeState::kFault) {
            // FAULT 探测从进入 FAULT 时刻起算（避免同 tick 误恢复）
            last_fault_probe_boot_ms = nowBoot();
        }

        TimeSnapshot prev = snap;
        snap.state = new_state;
        snap.reason = reason;
        snap.reason_detail = detail;
        if (src != nullptr) {
            snap.source_id = src->source_id;
            snap.source_kind = src->kind;
            snap.source_epoch = src->source_epoch;
            snap.auth_status = src->auth_status;
            snap.evidence_ref = src->evidence_ref;
            snap.auth_verified = src->auth_verified;
        }
        snap.event_sequence++;  // 状态事实变化才递增
        snap.synchronized = isSynchronized(new_state);

        // 快照 UTC/质量摘要（提交时按状态语义填充）
        fillQualityLocked();

        TimeStateEvent ev;
        ev.daemon_epoch = daemon_epoch;
        ev.event_sequence = snap.event_sequence;
        ev.source_epoch = snap.source_epoch;
        ev.old_state = prev.state;
        ev.new_state = new_state;
        ev.reason = reason;
        ev.source_id = snap.source_id;
        ev.source_kind = snap.source_kind;
        ev.utc_valid = snap.utc_valid;
        ev.utc_epoch_ms = snap.utc_epoch_ms;
        ev.uncertainty_ms = snap.uncertainty_ms;
        ev.uncertainty_valid = snap.uncertainty_valid;
        ev.auth_status = snap.auth_status;
        if (ev.validate() && event_cb) {
            // 事件发布不阻塞校时：上层接有界队列
            event_cb(ev);
        }
    }

    void fillQualityLocked() {
        const int64_t now = nowBoot();
        switch (snap.state) {
            case TimeState::kSynced: {
                if (sync_basis) {
                    const auto& s = *sync_basis;
                    snap.utc_valid = s.utc_valid;
                    snap.utc_epoch_ms = s.utc_epoch_ms;
                    snap.uncertainty_valid = s.error_bounded;
                    snap.uncertainty_ms = s.error_bounded ? s.error_upper_ms : 0;
                    snap.drift_bound_ppm =
                        s.drift_bounded ? s.drift_upper_ppm : cfg.drift_bound_ppm;
                    snap.system_clock_aligned = true;
                    snap.freshness_age_ms =
                        has_verified_baseline ? now - last_verified_boot_ms
                                              : 0;
                    snap.remaining_validity_ms =
                        max64(cfg.max_source_age_ms - snap.freshness_age_ms, 0);
                }
                break;
            }
            case TimeState::kHoldover: {
                const int64_t elapsed = now - holdover_start_boot_ms;
                const int64_t u = holdoverUncertainty(
                    holdover_base_uncertainty_ms, cfg.drift_bound_ppm, elapsed);
                snap.utc_valid = true;
                snap.utc_epoch_ms = holdover_base_utc_ms + elapsed;
                snap.uncertainty_valid = true;
                snap.uncertainty_ms = u;
                snap.system_clock_aligned = true;
                snap.freshness_age_ms =
                    has_verified_baseline ? now - last_verified_boot_ms : elapsed;
                snap.remaining_validity_ms =
                    max64(cfg.holdover_max_ms - elapsed, 0);
                snap.drift_bound_ppm = cfg.drift_bound_ppm;
                break;
            }
            default:
                // 未同步/校准中/故障：明确不可用，不伪装
                snap.utc_valid = false;
                snap.utc_epoch_ms = 0;
                snap.uncertainty_valid = false;
                snap.uncertainty_ms = 0;
                snap.system_clock_aligned = false;
                snap.freshness_age_ms = 0;
                snap.remaining_validity_ms = 0;
                break;
        }
    }

    // ---- 候选筛选（设计 §3 选源规则）----
    bool candidateOkLocked(const PerSource& s, int64_t now) {
        if (!s.last) return false;
        const auto& smp = *s.last;
        if (!smp.utc_valid || !smp.healthy) return false;
        const int64_t age = now - smp.sampled_boot_ms;
        if (smp.age_valid && age > cfg.max_source_age_ms) return false;
        if (smp.error_bounded && smp.error_upper_ms > cfg.max_source_error_ms)
            return false;
        if (!smp.error_bounded) return false;  // 无法界定时拒绝可用声明
        if (smp.auth_status == AuthStatus::kFailed) return false;  // 证据验证失败不可用
        if (cfg.require_authenticated_sources &&
            smp.auth_status != AuthStatus::kVerified)
            return false;
        return true;
    }

    PerSource* bestCandidateLocked(int64_t now) {
        PerSource* best = nullptr;
        for (auto& s : sources) {
            if (!candidateOkLocked(*s, now)) continue;
            if (best == nullptr || s->priority < best->priority) best = s.get();
        }
        return best;
    }

    // ---- 状态推进（由 tick 驱动）----
    void reevaluateLocked(int64_t now) {
        const TimeState st = snap.state;
        PerSource* active = nullptr;
        if (!snap.source_id.empty()) active = findSource(snap.source_id);
        const bool active_ok =
            active != nullptr && candidateOkLocked(*active, now);

        if (!active_ok) {
            // 当前来源失效：立即撤销有效状态，不受稳定窗口阻碍
            if (st == TimeState::kSynced) {
                if (cfg.holdover_enabled) {
                    holdover_start_boot_ms = now;
                    holdover_base_uncertainty_ms =
                        sync_basis && sync_basis->error_bounded
                            ? sync_basis->error_upper_ms
                            : cfg.max_source_error_ms;
                    holdover_base_utc_ms =
                        sync_basis ? sync_basis->utc_epoch_ms : 0;
                    commitLocked(TimeState::kHoldover, TimeReason::kHoldoverEntered,
                                 "source lost, holdover",
                                 sync_basis ? &*sync_basis : nullptr, 0, false);
                } else {
                    commitLocked(TimeState::kUnsynch, TimeReason::kSourceLost,
                                 "source lost", nullptr, 0, false);
                }
                return;
            }
            if (st == TimeState::kSyncing) {
                commitLocked(TimeState::kUnsynch, TimeReason::kSourceLost,
                             "sync target lost", nullptr, 0, false);
                return;
            }
            // UNSYNCHRONIZED/HOLDOVER/FAULT 无 active 或 active 失效：继续选源
        } else {
            // active 仍有效：滞回——只有更高优先级且稳定窗口满足才切换
            PerSource* best = bestCandidateLocked(now);
            if (best && best != active) {
                const bool stable =
                    best->stable_count >= cfg.stability_window_syncs &&
                    (now - best->stable_since_boot_ms) >= cfg.switch_hysteresis_ms;
                fprintf(stderr, "DBG2 best=%s active=%s stable=%d/%d dur=%lld hyst=%lld ok=%d\n",
                        best->adapter->sourceId().c_str(), active->adapter->sourceId().c_str(),
                        best->stable_count, cfg.stability_window_syncs,
                        (long long)(now - best->stable_since_boot_ms), (long long)cfg.switch_hysteresis_ms);
                if (stable) {
                    commitLocked(TimeState::kSyncing, TimeReason::kSourceChanged,
                                 "switched to higher priority source",
                                 best->last ? &*best->last : nullptr, 0, false);
                    sync_start_boot_ms = now;
                    calibrate_attempts = 0;
                    return;
                }
            }
            // active 有效且无切换：若处于待同步状态，保持校准超时基准
            // （sync_start_boot_ms 仅在进入 SYNCHRONIZING 时设置，不随 tick 刷新）
            if (st == TimeState::kUnsynch && active && active->last) {
                // 撤销后重新获取（RollbackDetected 后 active 仍有效）
                commitLocked(TimeState::kSyncing, TimeReason::kQualifiedCandidate,
                             "re-sync after revoke", &*active->last, 0, false);
                sync_start_boot_ms = now;
                calibrate_attempts = 0;
            }
            return;
        }

        // 无有效 active：从候选池选源（立即，无滞回阻碍）
        PerSource* best = bestCandidateLocked(now);
        if (!best) {
            if (st == TimeState::kSyncing) {
                commitLocked(TimeState::kUnsynch, TimeReason::kNoSourceAvailable,
                             "no qualified candidate", nullptr, 0, false);
            }
            return;
        }
        commitLocked(TimeState::kSyncing, TimeReason::kQualifiedCandidate,
                     "qualified candidate", best->last ? &*best->last : nullptr, 0,
                     false);
        sync_start_boot_ms = now;
        calibrate_attempts = 0;
    }

    // 校准执行（后端 IO，tick 线程）
    void calibrateLocked(int64_t now) {
        const TimeState st = snap.state;
        if (st != TimeState::kSyncing) return;
        PerSource* target = findSource(snap.source_id);
        if (!target || !target->last || !candidateOkLocked(*target, now)) return;
        const SourceSample& smp = *target->last;

        // 校准超时
        if (now - sync_start_boot_ms > cfg.synchronizing_timeout_ms) {
            commitLocked(TimeState::kUnsynch, TimeReason::kNoSourceAvailable,
                         "synchronizing timeout", nullptr, 0, false);
            return;
        }

        const BackendCapability cap = backend->capability();
        if (cap == BackendCapability::kCompeting) {
            commitLocked(TimeState::kFault, TimeReason::kCompetingWriter,
                         "competing time writer detected", nullptr, 0, false);
            return;
        }
        if (cap == BackendCapability::kFault || cap == BackendCapability::kUnsupported) {
            commitLocked(TimeState::kFault, TimeReason::kBackendFault,
                         "backend unavailable", nullptr, 0, false);
            return;
        }

        ClockRead wall = backend->readClock();
        if (!wall.valid) {
            commitLocked(TimeState::kFault, TimeReason::kBackendFault,
                         "read clock failed: " + wall.detail, nullptr, 0, false);
            return;
        }

        const int64_t offset = smp.utc_epoch_ms - wall.realtime_epoch_ms;

        // 大偏差/回拨：先原子撤销旧同步并发出变化事件，再受控重同步（设计 §4.3）
        if (offset > cfg.step_threshold_ms || offset < -cfg.step_threshold_ms) {
            if (snap.state == TimeState::kSynced) {
                commitLocked(TimeState::kUnsynch, TimeReason::kRollbackDetected,
                             "large offset, revoking sync", &smp, 0, false);
            }
            BackendResult res = backend->stepTo(smp.utc_epoch_ms);
            if (!res.ok) {
                calibrate_attempts++;
                if (calibrate_attempts >= cfg.calibration_retry_limit) {
                    commitLocked(TimeState::kFault, TimeReason::kBackendFault,
                                 "step failed: " + res.detail, nullptr, 0, false);
                }
                return;
            }
        } else if (offset > cfg.slew_threshold_ms || offset < -cfg.slew_threshold_ms) {
            BackendResult res = backend->slewBy(offset);
            if (!res.ok) {
                calibrate_attempts++;
                if (calibrate_attempts >= cfg.calibration_retry_limit) {
                    commitLocked(TimeState::kFault, TimeReason::kBackendFault,
                                 "slew failed: " + res.detail, nullptr, 0, false);
                }
                return;
            }
        }
        // |offset| <= slew_threshold：视为已对齐，无需调整

        // 写入后读回验证（设计 §4.4）
        int64_t alignment = 0;
        BackendResult verify =
            backend->verifyAlignment(smp.utc_epoch_ms, smp.error_upper_ms, &alignment);
        if (!verify.ok) {
            calibrate_attempts++;
            if (calibrate_attempts >= cfg.calibration_retry_limit) {
                commitLocked(TimeState::kFault, TimeReason::kBackendFault,
                             "verify failed: " + verify.detail, nullptr, 0, false);
            }
            return;
        }
        if (alignment > cfg.alignment_tolerance_ms ||
            alignment < -cfg.alignment_tolerance_ms) {
            // slew 未收敛/未对齐：不得提前宣告同步
            calibrate_attempts++;
            if (calibrate_attempts >= cfg.calibration_retry_limit) {
                commitLocked(TimeState::kFault, TimeReason::kBackendFault,
                             "alignment not converged", nullptr, 0, false);
            }
            return;
        }

        // 对齐验证成功 → SYNCHRONIZED（设计 §3 状态转换）
        // 校准后读回墙钟作为外部改时检测基准（不能用校准前墙钟，否则同 tick 误判回拨）
        ClockRead post = backend->readClock();
        last_verified_wall_ms =
            post.valid ? post.realtime_epoch_ms : wall.realtime_epoch_ms;
        last_verified_boot_ms = now;
        has_verified_baseline = true;
        sync_basis = smp;  // 同步依据：HOLDOVER/质量摘要基准
        calibrate_attempts = 0;
        commitLocked(TimeState::kSynced, TimeReason::kAlignmentVerified,
                     "alignment verified", &smp, alignment, false);
    }

    void monitorLocked(int64_t now) {
        const TimeState st = snap.state;
        if (st != TimeState::kSynced && st != TimeState::kHoldover) return;

        ClockRead wall = backend->readClock();
        const bool wall_ok = wall.valid;

        // 外部改时/回拨检测（设计 §4.6；校准期间跳过）
        if (has_verified_baseline && wall_ok) {
            const int64_t elapsed = now - last_verified_boot_ms;
            const int64_t drift_budget =
                cfg.drift_bound_ppm * elapsed / 1000000;
            const int64_t expected = last_verified_wall_ms + elapsed;
            const int64_t dev = wall.realtime_epoch_ms - expected;
            if (dev > cfg.external_change_threshold_ms + drift_budget ||
                dev < -(cfg.external_change_threshold_ms + drift_budget)) {
                commitLocked(TimeState::kUnsynch, TimeReason::kRollbackDetected,
                             "external clock change detected", nullptr, 0, false);
                return;
            }
        }

        // 运行期大偏差新样本的撤销检测移到 onSourceSampleLocked（样本到达时），
        // 避免把"最近样本随墙钟自然流逝"误判为大偏差（设计 §4.3）。

        if (st == TimeState::kHoldover) {
            const int64_t elapsed = now - holdover_start_boot_ms;
            const int64_t u = holdoverUncertainty(
                holdover_base_uncertainty_ms, cfg.drift_bound_ppm, elapsed);
            if (elapsed > cfg.holdover_max_ms ||
                u > cfg.holdover_max_error_ms) {
                commitLocked(TimeState::kUnsynch, TimeReason::kHoldoverExpired,
                             "holdover exceeded bounds", nullptr, 0, false);
                return;
            }
            // 有新合格候选 → 重同步
            PerSource* best = bestCandidateLocked(now);
            if (best) {
                commitLocked(TimeState::kSyncing, TimeReason::kQualifiedCandidate,
                             "re-sync from candidate", best->last ? &*best->last : nullptr,
                             0, false);
                sync_start_boot_ms = now;
                calibrate_attempts = 0;
            }
        }
    }

    void probeFaultLocked(int64_t now) {
        if (snap.state != TimeState::kFault) return;
        if (now - last_fault_probe_boot_ms < cfg.fault_probe_interval_ms) return;
        last_fault_probe_boot_ms = now;
        const BackendCapability cap = backend->capability();
        if (cap == BackendCapability::kReady) {
            // 故障消除 → 重新获取与校验，不直接恢复同步（设计 §3）
            commitLocked(TimeState::kUnsynch, TimeReason::kInitial,
                         "backend recovered, re-acquiring", nullptr, 0, false);
            reevaluateLocked(now);
        }
    }

    void tickLocked(int64_t now) {
        if (!running) return;
        probeFaultLocked(now);
        reevaluateLocked(now);
        calibrateLocked(now);
        monitorLocked(now);
    }

public:
    // ---- 公开入口 ----
    void onSourceSampleLocked(const SourceSample& sample, int64_t now) {
        if (!running) return;
        if (!sample.validate()) return;
        PerSource* s = findSource(sample.source_id);
        if (!s) return;
        // 旧 generation 回调不得覆盖当前来源（设计 §6 / AT-06）
        if (sample.adapter_generation < s->generation) {
            return;
        }
        s->generation = sample.adapter_generation;

        if (!sample.healthy || !sample.utc_valid) {
            // 来源失效：立即撤销有效状态（设计 §3：当前来源失效不受稳定窗口阻碍）
            s->last.reset();
            s->stable_count = 0;
            reevaluateLocked(now);
            return;
        }
        const int64_t age = now - sample.sampled_boot_ms;
        if (sample.age_valid && age > cfg.max_source_age_ms) {
            s->last.reset();
            s->stable_count = 0;
            return;
        }
        // 运行期新样本偏差大于 step 阈值或发生回拨：先原子撤销同步并发出
        // 变化事件，再执行受控重同步（设计 §4.3）。不得静默跳变。
        if (snap.state == TimeState::kSynced && sample.source_id == snap.source_id) {
            const ClockRead wall = backend->readClock();
            if (wall.valid) {
                const int64_t dev = sample.utc_epoch_ms - wall.realtime_epoch_ms;
                if (dev > cfg.step_threshold_ms ||
                    dev < -cfg.step_threshold_ms) {
                    s->stable_count = 1;
                    s->stable_since_boot_ms = now;
                    s->last = sample;
                    commitLocked(TimeState::kUnsynch,
                                 TimeReason::kRollbackDetected,
                                 "source offset beyond step threshold", nullptr, 0,
                                 false);
                    return;  // 下一 tick reevaluate → 受控重同步
                }
            }
        }
        if (s->last && s->last->source_epoch == sample.source_epoch) {
            s->stable_count++;
        } else {
            s->stable_count = 1;
        }
        s->stable_since_boot_ms = now;
        s->last = sample;
        reevaluateLocked(now);
    }

    void startLocked(int64_t now) {
        running = true;
        start_boot_ms = now;
        daemon_epoch++;
        snap = TimeSnapshot{};
        snap.daemon_epoch = daemon_epoch;
        snap.contract_version = kContractVersion;
        snap.capabilities = "snapshot,v1";
        snap.policy_version = "1";
        snap.state = TimeState::kUnsynch;
        snap.reason = TimeReason::kInitial;
        has_verified_baseline = false;
        holdover_start_boot_ms = 0;
        calibrate_attempts = 0;
        sync_start_boot_ms = 0;
    }

    void stopLocked() {
        running = false;
        // 停机：撤销状态、取消计时/回调（设计 §8）
        has_verified_baseline = false;
        for (auto& s : sources) {
            if (s->adapter) s->adapter->stop();
            s->last.reset();
            s->stable_count = 0;
        }
    }
};

TimeCore::TimeCore(BackendAdapter* backend, TimeCoreConfig cfg, BootClock boot_clock)
    : impl_(std::make_unique<Impl>(backend, cfg, std::move(boot_clock))) {}

TimeCore::~TimeCore() = default;

void TimeCore::registerSource(SourceAdapter* adapter, int priority) {
    if (!adapter) return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto ps = std::make_unique<Impl::PerSource>();
    ps->adapter = adapter;
    ps->priority = priority;
    impl_->sources.push_back(std::move(ps));
}

void TimeCore::onSourceSample(const SourceSample& sample) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->onSourceSampleLocked(sample, impl_->nowBoot());
}

void TimeCore::tick() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->tickLocked(impl_->nowBoot());
}

void TimeCore::setStateChangedCallback(
    std::function<void(const TimeStateEvent&)> cb) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->event_cb = std::move(cb);
}

TimeSnapshot TimeCore::snapshot() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    // 读取时刷新年龄/有效期事实（不得只在客户端查询时才检查失步，
    // 但 freshness/remaining 应在回复生成时计算）
    impl_->fillQualityLocked();
    return impl_->snap;
}

void TimeCore::start() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->startLocked(impl_->nowBoot());
}

void TimeCore::stop() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->stopLocked();
}

TimeState TimeCore::state() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->snap.state;
}

uint64_t TimeCore::eventSequence() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->snap.event_sequence;
}

bool TimeCore::hasActiveSource() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return !impl_->snap.source_id.empty();
}

const std::string& TimeCore::activeSourceId() const {
    static const std::string kEmpty;
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->snap.source_id.empty() ? kEmpty : impl_->snap.source_id;
}

}  // namespace time
}  // namespace tbox
