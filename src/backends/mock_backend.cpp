#include "backends/mock_backend.h"

namespace tbox {
namespace time {

MockBackend::MockBackend(int64_t initial_wall_ms,
                         std::function<int64_t()> boot_clock,
                         bool auto_advance)
    : wall_ms_(initial_wall_ms),
      boot_clock_(std::move(boot_clock)),
      auto_advance_(auto_advance) {
    if (boot_clock_) boot_anchor_ = boot_clock_();
}

BackendCapability MockBackend::capability() const {
    if (ctrl.competing_writer) return BackendCapability::kCompeting;
    if (ctrl.capability_fault) return BackendCapability::kFault;
    return BackendCapability::kReady;
}

int64_t MockBackend::effectiveWall() const {
    if (auto_advance_ && boot_clock_) {
        return wall_ms_ + (boot_clock_() - boot_anchor_);
    }
    return wall_ms_;
}

ClockRead MockBackend::readClock() {
    ClockRead r;
    if (ctrl.fail_read) {
        r.valid = false;
        r.detail = "mock read failure";
        return r;
    }
    r.valid = true;
    r.realtime_epoch_ms = effectiveWall();
    r.boot_monotonic_ms =
        boot_clock_ ? boot_clock_() : effectiveWall();
    return r;
}

BackendResult MockBackend::stepTo(int64_t utc_epoch_ms) {
    BackendResult res;
    if (ctrl.fail_step) {
        res.ok = false;
        res.errno_code = ctrl.fail_errno;
        res.detail = "mock step failure";
        return res;
    }
    wall_ms_ = utc_epoch_ms;
    if (boot_clock_) boot_anchor_ = boot_clock_();  // 重新锚定自动流逝
    pending_slew_ms_ = 0;
    res.ok = true;
    return res;
}

BackendResult MockBackend::slewBy(int64_t offset_ms) {
    BackendResult res;
    if (ctrl.fail_slew) {
        res.ok = false;
        res.errno_code = ctrl.fail_errno;
        res.detail = "mock slew failure";
        return res;
    }
    pending_slew_ms_ += offset_ms;
    if (ctrl.align_immediately) {
        wall_ms_ += pending_slew_ms_;
        if (boot_clock_) boot_anchor_ = boot_clock_();
        pending_slew_ms_ = 0;
    }
    res.ok = true;
    return res;
}

BackendResult MockBackend::verifyAlignment(int64_t utc_epoch_ms,
                                           int64_t error_upper_ms,
                                           int64_t* alignment_error_ms) {
    BackendResult res;
    if (ctrl.fail_verify) {
        res.ok = false;
        res.errno_code = ctrl.fail_errno;
        res.detail = "mock verify failure";
        return res;
    }
    const int64_t bias = pending_slew_ms_ + ctrl.verify_bias_ms;
    const int64_t alignment = (effectiveWall() + bias) - utc_epoch_ms;
    if (alignment_error_ms) *alignment_error_ms = alignment;
    res.ok = true;
    return res;
}

bool MockBackend::detectCompetingWriters(std::string* detail) {
    if (detail) *detail = ctrl.competing_writer ? "mock competing writer" : "";
    return ctrl.competing_writer;
}

int64_t MockBackend::wallClockMs() const { return effectiveWall(); }

void MockBackend::advanceWall(int64_t ms) { wall_ms_ += ms; }

}  // namespace time
}  // namespace tbox
