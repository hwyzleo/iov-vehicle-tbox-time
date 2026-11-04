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

// chrony 复用为登记扩展点：拒绝（未实现，不占位标完成）。
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

}  // namespace time
}  // namespace tbox
