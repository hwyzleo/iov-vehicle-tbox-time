#include "tbox/time/time_client.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "ipc.h"  // tbox::fw::ipc（framework SDK）
#include "ipc/time_json.h"

namespace tbox {
namespace time {

using json = nlohmann::json;
namespace fwipc = tbox::fw::ipc;

namespace {

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

TimeState parseState(const std::string& st) {
    if (st == "UNSYNCHRONIZED") return TimeState::kUnsynch;
    if (st == "SYNCHRONIZING") return TimeState::kSyncing;
    if (st == "SYNCHRONIZED") return TimeState::kSynced;
    if (st == "HOLDOVER") return TimeState::kHoldover;
    if (st == "FAULT") return TimeState::kFault;
    return TimeState::kUnsynch;
}

TimeReason parseReason(const std::string& r) {
    if (r == "initial") return TimeReason::kInitial;
    if (r == "qualified_candidate") return TimeReason::kQualifiedCandidate;
    if (r == "alignment_verified") return TimeReason::kAlignmentVerified;
    if (r == "alignment_lost") return TimeReason::kAlignmentLost;
    if (r == "source_lost") return TimeReason::kSourceLost;
    if (r == "source_changed") return TimeReason::kSourceChanged;
    if (r == "holdover_entered") return TimeReason::kHoldoverEntered;
    if (r == "holdover_expired") return TimeReason::kHoldoverExpired;
    if (r == "backend_fault") return TimeReason::kBackendFault;
    if (r == "rollback_detected") return TimeReason::kRollbackDetected;
    if (r == "generation_stale") return TimeReason::kGenerationStale;
    if (r == "no_source_available") return TimeReason::kNoSourceAvailable;
    if (r == "backend_unsupported") return TimeReason::kBackendUnsupported;
    if (r == "competing_writer") return TimeReason::kCompetingWriter;
    if (r == "policy_violation") return TimeReason::kPolicyViolation;
    if (r == "observed_synchronized") return TimeReason::kObservedSynchronized;
    if (r == "observer_interface_unavailable")
        return TimeReason::kObserverInterfaceUnavailable;
    if (r == "observer_evidence_unavailable")
        return TimeReason::kObserverEvidenceUnavailable;
    return TimeReason::kNone;
}

AuthStatus parseAuth(const std::string& a) {
    if (a == "verified") return AuthStatus::kVerified;
    if (a == "unverified") return AuthStatus::kUnverified;
    if (a == "failed") return AuthStatus::kFailed;
    return AuthStatus::kUnknown;
}

bool parseSnapshotJson(const json& j, TimeSnapshot* out) {
    if (!j.is_object()) return false;
    TimeSnapshot s;
    try {
        s.contract_version = j.at("contract_version").get<uint32_t>();
        s.capabilities = j.value("capabilities", "");
        s.daemon_epoch = j.at("daemon_epoch").get<uint64_t>();
        s.event_sequence = j.at("event_sequence").get<uint64_t>();
        const std::string st = j.at("state").get<std::string>();
        s.state = parseState(st);
        s.synchronized = j.at("synchronized").get<bool>();
        s.reason = parseReason(j.value("reason", "none"));
        s.reason_detail = j.value("reason_detail", "");
        s.source_id = j.value("source_id", "");
        const std::string sk = j.value("source_kind", "none");
        if (sk == "ntp_network") s.source_kind = SourceKind::kNtpNetwork;
        else if (sk == "cellular") s.source_kind = SourceKind::kCellular;
        else if (sk == "gnss") s.source_kind = SourceKind::kGnss;
        else if (sk == "rtc") s.source_kind = SourceKind::kRtc;
        else s.source_kind = SourceKind::kNone;
        s.source_epoch = j.value("source_epoch", 0ULL);
        s.utc_valid = j.value("utc_valid", false);
        if (s.utc_valid) s.utc_epoch_ms = j.at("utc_epoch_ms").get<int64_t>();
        s.system_clock_aligned = j.value("system_clock_aligned", false);
        s.alignment_error_ms = j.value("alignment_error_ms", 0LL);
        s.freshness_age_ms = j.value("freshness_age_ms", 0LL);
        s.uncertainty_valid = j.value("uncertainty_valid", false);
        if (s.uncertainty_valid) s.uncertainty_ms = j.at("uncertainty_ms").get<int64_t>();
        s.remaining_validity_ms = j.value("remaining_validity_ms", 0LL);
        s.drift_bound_ppm = j.value("drift_bound_ppm", 0LL);
        s.auth_status = parseAuth(j.value("authentication_status", "unknown"));
        s.evidence_ref = j.value("evidence_ref", "");
        s.policy_version = j.value("policy_version", "1");
    } catch (const std::exception&) {
        return false;
    }
    if (!s.validate()) return false;
    *out = s;
    return true;
}

bool parseEventJson(const std::string& payload, TimeStateEvent* out) {
    try {
        const json j = json::parse(payload);
        TimeStateEvent ev;
        ev.daemon_epoch = j.at("daemon_epoch").get<uint64_t>();
        ev.event_sequence = j.at("event_sequence").get<uint64_t>();
        ev.source_epoch = j.value("source_epoch", 0ULL);
        ev.old_state = parseState(j.at("old_state").get<std::string>());
        ev.new_state = parseState(j.at("new_state").get<std::string>());
        ev.reason = parseReason(j.value("reason", "none"));
        ev.source_id = j.value("source_id", "");
        ev.utc_valid = j.value("utc_valid", false);
        if (ev.utc_valid) ev.utc_epoch_ms = j.at("utc_epoch_ms").get<int64_t>();
        ev.uncertainty_valid = j.value("uncertainty_valid", false);
        if (ev.uncertainty_valid) ev.uncertainty_ms = j.at("uncertainty_ms").get<int64_t>();
        ev.auth_status = parseAuth(j.value("authentication_status", "unknown"));
        if (!ev.validate()) return false;
        *out = ev;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

class TimeClient::Impl {
public:
    explicit Impl(std::string socket_path)
        : socket_path_(std::move(socket_path)), client_(socket_path_) {}

    bool connect() { return client_.connect(); }
    void disconnect() { client_.disconnect(); }
    bool isConnected() const { return client_.is_connected(); }

    SnapshotResult getTimeSnapshot() {
        SnapshotResult res;
        const int64_t t0 = steadyMs();
        auto [status, body] = client_.call(kMethodGetTimeSnapshot, "{}");
        res.rtt_ms = steadyMs() - t0;
        if (status != 0) {
            res.error = (status < 0) ? ClientError::kServiceUnreachable
                                     : ClientError::kInternalError;
            return res;
        }
        try {
            const json j = json::parse(body);
            const std::string st = j.value("status", "");
            json snap_json;
            if (j.contains("snapshot")) snap_json = j["snapshot"];
            if (!parseSnapshotJson(snap_json, &res.snapshot)) {
                res.error = ClientError::kInvalidData;
                return res;
            }
            if (st == "ok") res.error = ClientError::kOk;
            else if (st == "time_unavailable") res.error = ClientError::kTimeUnavailable;
            else if (st == "unsupported") res.error = ClientError::kUnsupported;
            else if (st == "invalid") res.error = ClientError::kInvalidData;
            else res.error = ClientError::kInternalError;
        } catch (const std::exception&) {
            res.error = ClientError::kInvalidData;
        }
        return res;
    }

    /// 订阅内部状态（对账）。
    struct SubState {
        fwipc::Subscription sub;
        std::mutex mu;
        StateChangedCallback user_cb;
        bool has_baseline = false;
        uint64_t last_daemon_epoch = 0;
        uint64_t last_sequence = 0;
        std::vector<TimeStateEvent> pending;  // 订阅建立后的缓冲
        size_t pending_cap = 256;
        bool stale_epoch = false;  // daemon_epoch 改变：旧缓存不可用
    };

    std::shared_ptr<void> subscribeStateChanged(StateChangedCallback cb) {
        auto state = std::make_shared<SubState>();
        state->user_cb = std::move(cb);

        auto event_handler = [this, state](uint32_t event_type, std::string_view payload) {
            if (event_type != kEventTimeStateChanged) return;
            TimeStateEvent ev;
            if (!parseEventJson(std::string(payload), &ev)) return;
            std::lock_guard<std::mutex> lk(state->mu);
            if (!state->has_baseline) {
                // 先缓冲，待查询快照对账（设计 §6）
                if (state->pending.size() < state->pending_cap) {
                    state->pending.push_back(ev);
                }
                return;
            }
            // daemon_epoch 改变：旧缓存不可用，标记待重查询
            if (ev.daemon_epoch != state->last_daemon_epoch) {
                state->stale_epoch = true;
                return;
            }
            // 乱序/重复丢弃
            if (ev.event_sequence <= state->last_sequence) return;
            state->last_sequence = ev.event_sequence;
            if (state->user_cb) state->user_cb(ev);
        };

        state->sub = client_.subscribe(kMethodSubscribeTimeState,
                                       kEventTimeStateChanged, "{}",
                                       std::move(event_handler));

        // 订阅完成后再查询快照，以 epoch/sequence 对账
        SnapshotResult base = getTimeSnapshot();
        {
            std::lock_guard<std::mutex> lk(state->mu);
            if (base.error == ClientError::kOk ||
                base.error == ClientError::kTimeUnavailable) {
                state->has_baseline = true;
                state->last_daemon_epoch = base.snapshot.daemon_epoch;
                state->last_sequence = base.snapshot.event_sequence;
                // 投递缓冲中 sequence > 基线的事件
                std::vector<TimeStateEvent> to_deliver;
                for (auto& ev : state->pending) {
                    if (ev.daemon_epoch == state->last_daemon_epoch &&
                        ev.event_sequence > state->last_sequence) {
                        to_deliver.push_back(ev);
                        state->last_sequence = ev.event_sequence;
                    }
                }
                state->pending.clear();
                if (state->user_cb) {
                    for (auto& ev : to_deliver) state->user_cb(ev);
                }
            } else {
                // 查询失败：缓冲保留，等待重新对账（调用方可通过再次订阅/查询收敛）
                state->pending.clear();  // 无基线则无法对账，丢弃缓冲
            }
        }

        // RAII 句柄：析构时取消订阅。持有 state（共享所有权），
        // 指针非空以便调用方判断订阅已建立（handle != nullptr）。
        std::shared_ptr<void> handle(state.get(), [state](void*) {
            std::lock_guard<std::mutex> lk(state->mu);
            state->user_cb = {};
            state->sub.cancel();
        });
        return handle;
    }

    std::string socket_path_;
    fwipc::Client client_;
};

TimeClient::TimeClient(std::string socket_path)
    : impl_(std::make_unique<Impl>(std::move(socket_path))) {}

TimeClient::~TimeClient() = default;

bool TimeClient::connect() { return impl_->connect(); }
void TimeClient::disconnect() { impl_->disconnect(); }
bool TimeClient::isConnected() const { return impl_->isConnected(); }

SnapshotResult TimeClient::getTimeSnapshot() { return impl_->getTimeSnapshot(); }

std::shared_ptr<void> TimeClient::subscribeStateChanged(StateChangedCallback cb) {
    return impl_->subscribeStateChanged(std::move(cb));
}

}  // namespace time
}  // namespace tbox
