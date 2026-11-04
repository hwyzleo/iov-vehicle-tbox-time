#pragma once

//
// TBOX-TIME-DSN-CR-001：真实 NTP v4 网络授时来源（RFC 5905）。
//
// Wi-Fi/以太网为网络承载，NTP 服务器为时间源本身。
// 未认证 NTP（无 NTS/对称密钥）→ auth_status=unverified，仅用于明确标识的
// 非安全同步；认证机制（NTS/对称密钥）为扩展点，登记缺口（docs）。
//
// 传输/排队延迟计入不确定度：误差上界 = RTT/2 + 配置排队预算。
// 计龄使用 BOOTTIME 语义单调钟（含休眠经过时间）。
//

#include <cstdint>
#include <string>
#include <thread>

#include "tbox/time/source_adapter.h"
#include "tbox/time/source_sample.h"

namespace tbox {
namespace time {

struct NtpSourceConfig {
    std::string source_id = "ntp0";
    int priority = 10;             ///< 选源优先级（越小越优先）
    std::string host;              ///< 服务器（host 或 IP）
    uint16_t port = 123;
    int64_t timeout_ms = 3000;     ///< 单次采样超时
    int64_t poll_interval_ms = 30000;  ///< 轮询间隔
    int64_t max_age_ms = 300000;   ///< 样本最大年龄
    int64_t queueing_budget_ms = 50;   ///< 排队/抖动误差预算
    bool require_authenticated = false; ///< NTS/对称密钥要求（扩展点）
    std::string auth_mode = "none";     ///< none | nts(扩展点) | symmetric(扩展点)
    int64_t drift_upper_ppm = 100;      ///< 漂移上界（HOLDOVER 模型）
};

/// NTP v4 客户端（RFC 5905，mode 3 client / mode 4 server）。
class NtpClient {
public:
    NtpClient(NtpSourceConfig cfg, int64_t (*boot_ms)() = nullptr);
    ~NtpClient();

    /// 同步采样：发送请求并解析响应。
    /// 成功返回 true 并填充 sample（utc/误差/年龄均有效）。
    /// 失败返回 false 并填充健康/故障原因。
    bool query(SourceSample& out);

    static int64_t defaultBootMs();

private:
    NtpSourceConfig cfg_;
    int64_t (*boot_ms_)();
    int sock_ = -1;
    int64_t last_poll_boot_ms_ = 0;
    int64_t last_ref_ms_ = 0;   ///< 上次服务器 ref_time（重启检测）
    uint64_t next_epoch_ = 2;   ///< 来源代次自增（服务器重启）
};

/// NTP 网络授时来源适配器（单线程轮询）。
class NtpSourceAdapter final : public SourceAdapter {
public:
    explicit NtpSourceAdapter(NtpSourceConfig cfg);
    ~NtpSourceAdapter() override;

    const std::string& sourceId() const override;
    SourceKind kind() const override { return SourceKind::kNtpNetwork; }
    bool start(SampleCallback cb) override;
    void stop() override;
    SourceCapability capability() const override;

private:
    void pollLoop();

    NtpSourceConfig cfg_;
    NtpClient client_;
    SampleCallback cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<SourceCapability> cap_{SourceCapability::kDisabled};
};

}  // namespace time
}  // namespace tbox
