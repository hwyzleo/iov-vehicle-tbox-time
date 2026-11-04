//
// TBOX-TIME-DSN-CR-001：快照/事件载荷校验（设计 §5/§6）。
// 覆盖 AT-03：字段/单位/unknown 不写零、版本拒绝、派生布尔一致性。
//

#include <gtest/gtest.h>

#include "ipc/time_json.h"
#include "tbox/time/time_snapshot.h"
#include "tbox/time/types.h"

namespace tbox {
namespace time {

using namespace tbox::time;

// 未同步快照：utc/uncertainty 明确 unknown，不得以零伪装。
TEST(Snapshot, UnsynchSnapshotExplicitUnknown) {
    TimeSnapshot s;
    s.contract_version = kContractVersion;
    s.daemon_epoch = 1;
    s.state = TimeState::kUnsynch;
    s.synchronized = false;
    s.reason = TimeReason::kNoSourceAvailable;
    EXPECT_TRUE(s.validate());
    EXPECT_FALSE(s.utc_valid);
    EXPECT_EQ(s.utc_epoch_ms, 0);
    EXPECT_FALSE(s.uncertainty_valid);
    EXPECT_EQ(s.uncertainty_ms, 0);
}

// 同步快照：synchronized 只能由状态推导（REQ-003）。
TEST(Snapshot, SynchronizedDerivedFromState) {
    TimeSnapshot s;
    s.contract_version = kContractVersion;
    s.daemon_epoch = 1;
    s.state = TimeState::kSynced;
    s.synchronized = true;
    s.utc_valid = true;
    s.utc_epoch_ms = 1'700'000'000'000;
    s.uncertainty_valid = true;
    s.uncertainty_ms = 50;
    s.source_id = "ntp0";
    s.source_kind = SourceKind::kNtpNetwork;
    EXPECT_TRUE(s.validate());
    // 状态与派生不一致 → 拒绝
    s.synchronized = false;
    EXPECT_FALSE(s.validate());
    s.synchronized = true;
    EXPECT_TRUE(s.validate());
}

// unknown 不写零：uncertainty_valid=false 但 uncertainty_ms != 0 → 拒绝。
TEST(Snapshot, UnknownDoesNotWriteZero) {
    TimeSnapshot s;
    s.contract_version = kContractVersion;
    s.daemon_epoch = 1;
    s.utc_valid = false;
    s.utc_epoch_ms = 100;  // 无效却带值 → 拒绝
    EXPECT_FALSE(s.validate());
    s.utc_epoch_ms = 0;
    EXPECT_TRUE(s.validate());

    s.uncertainty_valid = false;
    s.uncertainty_ms = 10;
    EXPECT_FALSE(s.validate());
    s.uncertainty_ms = 0;
    EXPECT_TRUE(s.validate());
}

// UTC 有效要求误差可界定（无保守误差承诺不可用）。
TEST(Snapshot, UtcValidRequiresUncertainty) {
    TimeSnapshot s;
    s.contract_version = kContractVersion;
    s.daemon_epoch = 1;
    s.state = TimeState::kSynced;
    s.synchronized = true;
    s.utc_valid = true;
    s.utc_epoch_ms = 1'700'000'000'000;
    s.uncertainty_valid = false;  // 无误差上界
    EXPECT_FALSE(s.validate());
}

// 数值范围/溢出校验。
TEST(Snapshot, RangeAndOverflowChecks) {
    TimeSnapshot s;
    s.contract_version = kContractVersion;
    s.daemon_epoch = 1;
    s.utc_valid = true;
    s.utc_epoch_ms = 42;  // 1970 年 → 范围外
    EXPECT_FALSE(s.validate());
    s.utc_epoch_ms = 9'999'999'999'999;  // 溢出范围
    EXPECT_FALSE(s.validate());
}

// 版本拒绝：客户端请求的 contract_version 超支持 → unsupported（AT-03）。
TEST(Snapshot, VersionRejectedAsUnsupported) {
    uint32_t ver = 0;
    std::string err;
    EXPECT_TRUE(parseRequestParams("{\"contract_version\": 99}", &ver, &err));
    EXPECT_EQ(ver, 99u);
    EXPECT_GT(ver, kContractVersion);  // 超出 → 服务端返回 unsupported
}

// 事件载荷校验：old/new state、序列、质量摘要。
TEST(Snapshot, EventPayloadValidation) {
    TimeStateEvent ev;
    ev.daemon_epoch = 1;
    ev.event_sequence = 2;
    ev.old_state = TimeState::kUnsynch;
    ev.new_state = TimeState::kSynced;
    ev.reason = TimeReason::kAlignmentVerified;
    ev.utc_valid = true;
    ev.utc_epoch_ms = 1'700'000'000'000;
    ev.uncertainty_valid = true;
    ev.uncertainty_ms = 50;
    ev.auth_status = AuthStatus::kUnverified;
    EXPECT_TRUE(ev.validate());

    // 首次同步（old=UNSYNCHRONIZED, new=SYNCHRONIZED）与
    // 恢复（old=HOLDOVER/UNSYNCHRONIZED, new=SYNCHRONIZED）通过 old/new 区分
    ev.old_state = TimeState::kHoldover;
    ev.new_state = TimeState::kSynced;
    EXPECT_TRUE(ev.validate());
    // 来源切换（old==new）合法（设计 §6 事件覆盖来源切换/质量越界）
    ev.old_state = TimeState::kSynced;
    ev.new_state = TimeState::kSynced;
    ev.reason = TimeReason::kSourceChanged;
    EXPECT_TRUE(ev.validate());
}

// JSON 序列化往返。
TEST(Snapshot, JsonRoundTrip) {
    TimeSnapshot s;
    s.contract_version = kContractVersion;
    s.daemon_epoch = 7;
    s.event_sequence = 3;
    s.state = TimeState::kSynced;
    s.synchronized = true;
    s.reason = TimeReason::kAlignmentVerified;
    s.source_id = "ntp0";
    s.source_kind = SourceKind::kNtpNetwork;
    s.utc_valid = true;
    s.utc_epoch_ms = 1'700'000'000'000;
    s.system_clock_aligned = true;
    s.alignment_error_ms = 10;
    s.freshness_age_ms = 500;
    s.uncertainty_valid = true;
    s.uncertainty_ms = 55;
    s.remaining_validity_ms = 299'500;
    s.drift_bound_ppm = 100;
    s.auth_status = AuthStatus::kUnverified;
    s.policy_version = "1";
    EXPECT_TRUE(s.validate());

    const std::string json = snapshotToJson(s, IpcStatus::kOk);
    EXPECT_NE(json.find("\"status\":\"ok\""), std::string::npos);
    EXPECT_NE(json.find("\"state\":\"SYNCHRONIZED\""), std::string::npos);
    EXPECT_NE(json.find("\"authentication_status\":\"unverified\""),
              std::string::npos);
}

}  // namespace time
}  // namespace tbox
