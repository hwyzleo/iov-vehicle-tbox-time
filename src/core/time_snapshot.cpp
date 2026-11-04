#include "tbox/time/time_snapshot.h"

namespace tbox {
namespace time {

bool TimeSnapshot::validate() const noexcept {
    if (contract_version == 0) return false;
    if (daemon_epoch == 0) return false;
    // 数值范围
    if (freshness_age_ms < 0 || freshness_age_ms > 86400000) return false;
    if (uncertainty_ms < 0 || uncertainty_ms > 86400000) return false;
    if (remaining_validity_ms < 0) return false;
    if (drift_bound_ppm < 0 || drift_bound_ppm > 1000000) return false;
    if (alignment_error_ms < -86400000 || alignment_error_ms > 86400000) return false;
    if (utc_valid) {
        if (utc_epoch_ms < 946684800000LL || utc_epoch_ms > 3471298560000LL) return false;
    }
    // unknown 不写零伪装：uncertainty_valid=false 时 uncertainty_ms 必须为 0
    if (!uncertainty_valid && uncertainty_ms != 0) return false;
    if (!utc_valid && utc_epoch_ms != 0) return false;
    // 派生布尔一致性
    if (synchronized != isSynchronized(state)) return false;
    if (auth_verified != (auth_status == AuthStatus::kVerified)) return false;
    // UTC 有效时要求误差可界定（否则无保守误差承诺）
    if (utc_valid && !uncertainty_valid) return false;
    return true;
}

bool TimeStateEvent::validate() const noexcept {
    if (daemon_epoch == 0) return false;
    if (event_sequence == 0) return false;
    // 允许 old_state == new_state（来源切换/质量越界等事实变化事件）；
    // 事件真实性由核心保证。
    if (utc_valid) {
        if (utc_epoch_ms < 946684800000LL || utc_epoch_ms > 3471298560000LL) return false;
    }
    if (uncertainty_ms < 0 || uncertainty_ms > 86400000) return false;
    return true;
}

}  // namespace time
}  // namespace tbox
