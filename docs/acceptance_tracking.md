# TBOX-TIME 验收追踪（TBOX-TIME-DSN-CR-001）

> 需求 §4 与设计 §9 的 AT-01～AT-12 双向追踪。本表登记本次实施的证据与状态。
> 设计 §9：模拟单测覆盖算法，真源联调证明平台能力，SEC/MQTT 联调证明端到端安全前置；
> 三者不得互相替代。

| AT | 需求 | 本次状态 | 证据 |
|----|------|----------|------|
| AT-01 无源冷启动返回不可用；真实源独立同步 | 001/008 | ✅ 本地；真源联调待 ORIN | `test_time_state_machine.NoSourceColdStartUnavailable`；`integration/test_daemon_e2e.RealSourceSynchronizesOverIpc`（UDP NTP 独立同步，无 MQTT/TSP 依赖） |
| AT-02 唯一 writer/写失败/权限/slew 未收敛/读回验证 | 002 | ✅ 核心逻辑；真机权限待 ORIN | `test_time_state_machine`（StepFailure→FAULT、CompetingWriter→FAULT、LargeOffsetUsesStep、MediumOffsetUsesSlew）；`test_backend`（Mock 故障注入）；`LinuxSystemClockBackend` 真实实现待实机 |
| AT-03 快照字段/unknown/版本拒绝/未同步可查询/无 setTime | 003/007 | ✅ | `test_snapshot`（字段/unknown 不写零/版本/派生布尔/范围）；`test_time_client.QueryBeforeSyncReportsUnavailable`、`NoSetTimeMethod` |
| AT-04 首次同步/失步/恢复事件；订阅查询竞态、乱序/重复/缺口 | 004 | ✅ 核心路径 | `test_time_client.SubscribeReceivesFirstSyncEvent`；`integration/test_daemon_e2e.StateEventsDeliveredOverIpc`（首同步+HOLDOVER 事件）；client 对账逻辑（epoch/sequence 去重）实现 |
| AT-05 源丢失→HOLDOVER、误差增长、超限失效、恢复重新验证 | 005 | ✅ | `test_holdover`（模型增长/超限/禁用）；`test_time_state_machine.HoldoverEnterAndExpire`；`integration/test_daemon_e2e.DisconnectHoldsOverThenExpires`（真实断连路径） |
| AT-06 回拨/大偏差/外部改时撤销、旧 generation 拒绝 | 002/005 | ✅ | `test_time_state_machine`（RollbackRevokesBeforeResync、ExternalClockChangeRevokes、StaleGenerationRejected） |
| AT-07 daemon 重启/IPC 重连/休眠唤醒/跨 boot 单调/RTT/缓存超期 | 003/004/005 | ✅ 逻辑；唤醒/重启真机待 ORIN | `daemon_epoch` 重启递增；BOOTTIME 计龄实现；`test_time_client.ReconcileWithQueryBaseline`、`RttIncludedInValidity` |
| AT-08 未认证不成为 SEC Trusted；无证据不填 authenticated | 001/006 | ✅ | `test_source_selection.RejectsAuthFailed`；NTP 采样 auth_status=unverified（`test_ntp_client`）；快照无 trusted 字段（`test_snapshot`）；e2e 断言 `auth_status=unverified` |
| AT-09 SEC 适配 300s/2s 边界 | 006 | ⛔ 待 SEC 配对 CR | 不属于本 CR；TIME 已输出 SEC PlatformTimeAdapter 所需事实（同步/来源证据/新鲜度/误差/对齐） |
| AT-10 多消费者只读/无重复后端/缓存过期/SDK 超时重连 | 007 | ✅ | `test_time_client.QueryAfterSyncOk`（并发只读、同一事实）；`integration/test_daemon_e2e.ConcurrentReadOnlyConsumers` |
| AT-11 坏初始时钟真实认证引导 | 008 | ⛔ 待真实来源与认证机制 | 未认证 NTP 非安全路径可同步；认证引导（NTS 等）为缺口，不关闭校验 |
| AT-12 配置纯校验/fake 隔离/健康/停机清理 | 全部 | ✅ | `test_config`（production 禁 mock、扩展点拒绝、范围/冲突/重复 id）；daemon 停机撤销状态/停来源/释放后端（TimeApplication::cleanup） |

## 状态汇总

- **本次实现完成**：AT-01/02/03/04/05/06/07/08/10/12 的核心逻辑与模拟/本地真实路径验证。
- **待 ORIN 实机**：真实 Linux 校时（step/slew/读回/权限/竞争进程）、真实 NTP 实网、
  冷启动引导（AT-01/02/07/11 真源部分）、休眠唤醒。
- **待 SEC 配对 CR**：AT-09（300s/2s 边界与证书保守区间）。
- 三者不得互相替代：模拟单测已覆盖算法；真源联调证明平台能力（待 ORIN）；
  SEC/MQTT 联调证明端到端安全前置（待配对 CR）。

## 代码版本与证据

- 分支：`feature/tbox-time-dsn-cr-001`
- 测试：`cd build && ctest` → 9/9 通过（约 31s）
- 实现/缺口/配置/wire 登记：docs/ 下 implementation_notes.md、config_schema.md、wire_registry.md
