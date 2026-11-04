#include "ipc/time_json.h"

#include <nlohmann/json.hpp>

namespace tbox {
namespace time {

using json = nlohmann::json;

namespace {

void fillSnapshotJson(json& j, const TimeSnapshot& s) {
    j["contract_version"] = s.contract_version;
    j["capabilities"] = s.capabilities;
    j["daemon_epoch"] = s.daemon_epoch;
    j["event_sequence"] = s.event_sequence;
    j["state"] = timeStateName(s.state);
    j["synchronized"] = s.synchronized;
    j["reason"] = timeReasonName(s.reason);
    if (!s.reason_detail.empty()) j["reason_detail"] = s.reason_detail;
    j["source_id"] = s.source_id;
    j["source_kind"] = sourceKindName(s.source_kind);
    j["source_epoch"] = s.source_epoch;
    j["utc_valid"] = s.utc_valid;
    if (s.utc_valid) j["utc_epoch_ms"] = s.utc_epoch_ms;
    j["system_clock_aligned"] = s.system_clock_aligned;
    j["alignment_error_ms"] = s.alignment_error_ms;
    j["freshness_age_ms"] = s.freshness_age_ms;
    j["uncertainty_valid"] = s.uncertainty_valid;
    if (s.uncertainty_valid) j["uncertainty_ms"] = s.uncertainty_ms;
    j["remaining_validity_ms"] = s.remaining_validity_ms;
    j["drift_bound_ppm"] = s.drift_bound_ppm;
    j["authentication_status"] = authStatusName(s.auth_status);
    if (!s.evidence_ref.empty()) j["evidence_ref"] = s.evidence_ref;
    j["policy_version"] = s.policy_version;
}

}  // namespace

std::string snapshotToJson(const TimeSnapshot& snap, IpcStatus status) {
    json j;
    switch (status) {
        case IpcStatus::kOk:
            j["status"] = "ok";
            break;
        case IpcStatus::kTimeUnavailable:
            j["status"] = "time_unavailable";
            break;
        case IpcStatus::kUnsupported:
            j["status"] = "unsupported";
            break;
        case IpcStatus::kInvalid:
            j["status"] = "invalid";
            break;
        case IpcStatus::kInternalError:
            j["status"] = "internal_error";
            break;
    }
    json s;
    fillSnapshotJson(s, snap);
    j["snapshot"] = s;
    return j.dump();
}

std::string eventToJson(const TimeStateEvent& ev) {
    json j;
    j["daemon_epoch"] = ev.daemon_epoch;
    j["event_sequence"] = ev.event_sequence;
    j["source_epoch"] = ev.source_epoch;
    j["old_state"] = timeStateName(ev.old_state);
    j["new_state"] = timeStateName(ev.new_state);
    j["reason"] = timeReasonName(ev.reason);
    j["source_id"] = ev.source_id;
    j["source_kind"] = sourceKindName(ev.source_kind);
    j["utc_valid"] = ev.utc_valid;
    if (ev.utc_valid) j["utc_epoch_ms"] = ev.utc_epoch_ms;
    j["uncertainty_valid"] = ev.uncertainty_valid;
    if (ev.uncertainty_valid) j["uncertainty_ms"] = ev.uncertainty_ms;
    j["authentication_status"] = authStatusName(ev.auth_status);
    return j.dump();
}

bool parseRequestParams(const std::string& params_json, uint32_t* contract_version,
                        std::string* error) {
    if (params_json.empty()) {
        *contract_version = kContractVersion;
        return true;
    }
    try {
        const json j = json::parse(params_json);
        if (j.contains("contract_version")) {
            *contract_version = j["contract_version"].get<uint32_t>();
        } else {
            *contract_version = kContractVersion;
        }
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

}  // namespace time
}  // namespace tbox
