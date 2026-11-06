//
// TBOX-TIME-DSN-CR-001：配置纯校验单元测试（设计 §8 / AT-12）。
// 类型/范围/冲突校验、production 拒绝 mock、扩展点（chrony/NTS）拒绝、
// 重复 source_id 拒绝；校验过程无副作用（不调钟、不联网）。
//

#include <gtest/gtest.h>

#include "config/time_config.h"

namespace tbox {
namespace time {

using namespace tbox::time;

namespace {

const char* kValidYaml = R"(
time:
  profile: production
  ipc:
    socket_path: /tmp/tbox-time.sock
    event_queue_limit: 512
  backend:
    mode: linux
    allow_competing_writers: false
  core:
    max_source_age_ms: 300000
    max_source_error_ms: 500
    step_threshold_ms: 1000
    slew_threshold_ms: 50
    alignment_tolerance_ms: 50
    holdover_max_ms: 3600000
    holdover_max_error_ms: 2000
    drift_bound_ppm: 100
  sources:
    ntp:
      enabled: true
      servers:
        - id: ntp0
          host: pool.ntp.org
          port: 123
          timeout_ms: 3000
          poll_interval_ms: 30000
    cellular:
      enabled: true
    gnss:
      enabled: true
    rtc:
      enabled: true
      require_provisioned: true
)";

}  // namespace

// 合法配置：解析成功且字段正确。
TEST(Config, LoadValidConfig) {
    TimeConfig cfg;
    std::string err;
    ASSERT_TRUE(loadTimeConfig(kValidYaml, &cfg, &err)) << err;
    EXPECT_EQ(cfg.profile, RunProfile::kProduction);
    EXPECT_EQ(cfg.backend_kind, BackendKind::kLinuxSystemClock);
    EXPECT_EQ(cfg.ipc_socket_path, "/tmp/tbox-time.sock");
    EXPECT_EQ(cfg.ntp_servers.size(), 1u);
    EXPECT_EQ(cfg.ntp_servers[0].host, "pool.ntp.org");
    EXPECT_EQ(cfg.ntp_servers[0].port, 123);
    EXPECT_EQ(cfg.core.step_threshold_ms, 1000);
    EXPECT_TRUE(cfg.rtc.require_provisioned);
}

// production 拒绝 mock 后端（AT-12 fake 隔离）。
TEST(Config, ProductionRejectsMockBackend) {
    const std::string yaml = R"(
time:
  profile: production
  backend:
    mode: mock
)";
    TimeConfig cfg;
    std::string err;
    EXPECT_FALSE(loadTimeConfig(yaml, &cfg, &err));
    EXPECT_NE(err.find("mock"), std::string::npos);
}

// test profile 允许 mock（测试 fixture 隔离）。
TEST(Config, TestProfileAllowsMock) {
    const std::string yaml = R"(
time:
  profile: test
  backend:
    mode: mock
)";
    TimeConfig cfg;
    std::string err;
    ASSERT_TRUE(loadTimeConfig(yaml, &cfg, &err)) << err;
    EXPECT_EQ(cfg.backend_kind, BackendKind::kMock);
}

// chrony 复用为登记扩展点：拒绝并给出迁移提示（CR-002：改用 system-observe）。
TEST(Config, RejectsChronyExtensionPoint) {
    const std::string yaml = R"(
time:
  backend:
    mode: chrony
)";
    TimeConfig cfg;
    std::string err;
    EXPECT_FALSE(loadTimeConfig(yaml, &cfg, &err));
    EXPECT_NE(err.find("chrony"), std::string::npos);
    EXPECT_NE(err.find("system-observe"), std::string::npos);  // 迁移提示
}

// 范围校验：非法阈值拒绝。
TEST(Config, RejectsInvalidRange) {
    const std::string yaml = R"(
time:
  core:
    step_threshold_ms: 0
)";
    TimeConfig cfg;
    std::string err;
    EXPECT_FALSE(loadTimeConfig(yaml, &cfg, &err));
    EXPECT_NE(err.find("step_threshold_ms"), std::string::npos);
}

// NTS/对称密钥认证为扩展点：拒绝（认证机制待实现/验证）。
TEST(Config, RejectsUnimplementedAuthMode) {
    const std::string yaml = R"(
time:
  sources:
    ntp:
      servers:
        - id: ntp0
          host: pool.ntp.org
          auth_mode: nts
)";
    TimeConfig cfg;
    std::string err;
    EXPECT_FALSE(loadTimeConfig(yaml, &cfg, &err));
    EXPECT_NE(err.find("extension point"), std::string::npos);
}

// 重复 source_id 拒绝。
TEST(Config, RejectsDuplicateSourceId) {
    const std::string yaml = R"(
time:
  sources:
    ntp:
      servers:
        - id: s1
          host: a.ntp.org
        - id: s1
          host: b.ntp.org
)";
    TimeConfig cfg;
    std::string err;
    EXPECT_FALSE(loadTimeConfig(yaml, &cfg, &err));
    EXPECT_NE(err.find("duplicate"), std::string::npos);
}

// yaml 语法错误拒绝。
TEST(Config, RejectsMalformedYaml) {
    TimeConfig cfg;
    std::string err;
    EXPECT_FALSE(loadTimeConfig("time: [broken", &cfg, &err));
}

// 校验纯副作用：直接构造非法结构（不经过 yaml），validateTimeConfig 拒绝。
TEST(Config, ValidateDirectRejects) {
    TimeConfig cfg;
    cfg.profile = RunProfile::kProduction;
    cfg.backend_kind = BackendKind::kMock;  // production + mock
    std::string err;
    EXPECT_FALSE(validateTimeConfig(cfg, &err));
}

// ---- CR-002：system-observe 配置校验 ----

// 合法观测配置：backend.mode=system-observe + observer 段，自采源全部禁用。
TEST(Config, LoadValidSystemObserve) {
    const std::string yaml = R"(
time:
  profile: production
  backend:
    mode: system-observe
    observer:
      driver: timesyncd
      poll_interval_ms: 1000
      dbus_timeout_ms: 1000
  core:
    max_source_age_ms: 300000
    max_source_error_ms: 500
  sources:
    ntp: {enabled: false}
    cellular: {enabled: false}
    gnss: {enabled: false}
    rtc: {enabled: false}
)";
    TimeConfig cfg;
    std::string err;
    ASSERT_TRUE(loadTimeConfig(yaml, &cfg, &err)) << err;
    EXPECT_EQ(cfg.backend_kind, BackendKind::kSystemObserve);
    EXPECT_EQ(cfg.observer.driver, "timesyncd");
    EXPECT_EQ(cfg.observer.poll_interval_ms, 1000);
    EXPECT_EQ(cfg.observer.dbus_timeout_ms, 1000);
}

// system-observe 下显式写钟专属键拒绝（设计 §4.2：仅适用于 backend.mode=linux）。
TEST(Config, SystemObserveRejectsWriteSpecificCoreKeys) {
    const std::string yaml = R"(
time:
  backend:
    mode: system-observe
  core:
    step_threshold_ms: 1000
)";
    TimeConfig cfg;
    std::string err;
    EXPECT_FALSE(loadTimeConfig(yaml, &cfg, &err));
    EXPECT_NE(err.find("backend.mode=linux"), std::string::npos);
}

// system-observe 下显式 allow_competing_writers 拒绝（避免默认 competing_processes
// 中的 systemd-timesyncd 被误判为竞争写入者）。
TEST(Config, SystemObserveRejectsWriteSpecificBackendKeys) {
    const std::string yaml = R"(
time:
  backend:
    mode: system-observe
    allow_competing_writers: true
)";
    TimeConfig cfg;
    std::string err;
    EXPECT_FALSE(loadTimeConfig(yaml, &cfg, &err));
    EXPECT_NE(err.find("backend.mode=linux"), std::string::npos);
}

// system-observe 下启用自采源独立候选拒绝（设计 §2：未驱动系统钟的样本不能证明对齐）。
TEST(Config, SystemObserveRejectsSelfCollectedSources) {
    const std::string yaml = R"(
time:
  backend:
    mode: system-observe
  sources:
    ntp:
      enabled: true
      servers:
        - id: ntp0
          host: pool.ntp.org
)";
    TimeConfig cfg;
    std::string err;
    EXPECT_FALSE(loadTimeConfig(yaml, &cfg, &err));
    EXPECT_NE(err.find("system-observe"), std::string::npos);
}

// 观测 driver：仅 timesyncd；其他值 unsupported 并拒绝。
TEST(Config, SystemObserveRejectsUnknownDriver) {
    const std::string yaml = R"(
time:
  backend:
    mode: system-observe
    observer:
      driver: chronyd
)";
    TimeConfig cfg;
    std::string err;
    EXPECT_FALSE(loadTimeConfig(yaml, &cfg, &err));
    EXPECT_NE(err.find("timesyncd"), std::string::npos);
}

// 观测轮询/超时范围校验。
TEST(Config, SystemObserveRejectsOutOfRange) {
    {
        const std::string yaml = R"(
time:
  backend:
    mode: system-observe
    observer:
      poll_interval_ms: 50
)";
        TimeConfig cfg;
        std::string err;
        EXPECT_FALSE(loadTimeConfig(yaml, &cfg, &err));
        EXPECT_NE(err.find("poll_interval_ms"), std::string::npos);
    }
    {
        const std::string yaml = R"(
time:
  backend:
    mode: system-observe
    observer:
      dbus_timeout_ms: 9000
)";
        TimeConfig cfg;
        std::string err;
        EXPECT_FALSE(loadTimeConfig(yaml, &cfg, &err));
        EXPECT_NE(err.find("dbus_timeout_ms"), std::string::npos);
    }
}

// production：max_source_age_ms 不得大于 300000（SEC 300s 窗口，设计 §6）。
TEST(Config, ProductionRejectsMaxSourceAgeAbove300s) {
    const std::string yaml = R"(
time:
  profile: production
  core:
    max_source_age_ms: 301000
)";
    TimeConfig cfg;
    std::string err;
    EXPECT_FALSE(loadTimeConfig(yaml, &cfg, &err));
    EXPECT_NE(err.find("max_source_age_ms"), std::string::npos);
}

// BackendKind 废弃别名：kChronyObserve == kSystemObserve（同值，不新增行为）。
TEST(Config, ChronyObserveAliasEqualsSystemObserve) {
    EXPECT_EQ(static_cast<uint8_t>(BackendKind::kChronyObserve),
              static_cast<uint8_t>(BackendKind::kSystemObserve));
    EXPECT_EQ(static_cast<uint8_t>(BackendKind::kSystemObserve), 2u);
    EXPECT_EQ(static_cast<uint8_t>(BackendKind::kLinuxSystemClock), 1u);
    EXPECT_EQ(static_cast<uint8_t>(BackendKind::kMock), 3u);
    EXPECT_EQ(static_cast<uint8_t>(BackendKind::kUnsupported), 4u);
}

}  // namespace time
}  // namespace tbox
