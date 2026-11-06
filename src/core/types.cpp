#include "tbox/time/types.h"

namespace tbox {
namespace time {

const char* timeStateName(TimeState s) noexcept {
    switch (s) {
        case TimeState::kUnsynch: return "UNSYNCHRONIZED";
        case TimeState::kSyncing: return "SYNCHRONIZING";
        case TimeState::kSynced: return "SYNCHRONIZED";
        case TimeState::kHoldover: return "HOLDOVER";
        case TimeState::kFault: return "FAULT";
    }
    return "UNKNOWN";
}

const char* timeReasonName(TimeReason r) noexcept {
    switch (r) {
        case TimeReason::kNone: return "none";
        case TimeReason::kInitial: return "initial";
        case TimeReason::kQualifiedCandidate: return "qualified_candidate";
        case TimeReason::kAlignmentVerified: return "alignment_verified";
        case TimeReason::kAlignmentLost: return "alignment_lost";
        case TimeReason::kSourceLost: return "source_lost";
        case TimeReason::kSourceChanged: return "source_changed";
        case TimeReason::kHoldoverEntered: return "holdover_entered";
        case TimeReason::kHoldoverExpired: return "holdover_expired";
        case TimeReason::kBackendFault: return "backend_fault";
        case TimeReason::kRollbackDetected: return "rollback_detected";
        case TimeReason::kGenerationStale: return "generation_stale";
        case TimeReason::kNoSourceAvailable: return "no_source_available";
        case TimeReason::kBackendUnsupported: return "backend_unsupported";
        case TimeReason::kCompetingWriter: return "competing_writer";
        case TimeReason::kPolicyViolation: return "policy_violation";
        case TimeReason::kObservedSynchronized: return "observed_synchronized";
        case TimeReason::kObserverInterfaceUnavailable:
            return "observer_interface_unavailable";
        case TimeReason::kObserverEvidenceUnavailable:
            return "observer_evidence_unavailable";
    }
    return "unknown";
}

const char* authStatusName(AuthStatus a) noexcept {
    switch (a) {
        case AuthStatus::kUnknown: return "unknown";
        case AuthStatus::kUnverified: return "unverified";
        case AuthStatus::kVerified: return "verified";
        case AuthStatus::kFailed: return "failed";
    }
    return "unknown";
}

const char* sourceKindName(SourceKind k) noexcept {
    switch (k) {
        case SourceKind::kNone: return "none";
        case SourceKind::kNtpNetwork: return "ntp_network";
        case SourceKind::kCellular: return "cellular";
        case SourceKind::kGnss: return "gnss";
        case SourceKind::kRtc: return "rtc";
    }
    return "unknown";
}

}  // namespace time
}  // namespace tbox
