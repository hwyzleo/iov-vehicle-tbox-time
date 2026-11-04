# TBOX-TIME 配置 schema 与默认值登记（TBOX-TIME-DSN-CR-001）

> 设计 §8：配置必须纯校验类型/范围/冲突、禁止未知关键配置和 production fake，
> 不在配置校验时调钟或联网。名称与默认值在此登记。

## 顶层

```yaml
time:
  profile: production        # production | test（test 允许 mock 后端/模拟源）
  policy_version: "1"
  scheduler_interval_ms: 1000
  ipc:
    socket_path: /tmp/tbox-time.sock
    event_queue_limit: 1024   # 有界事件队列：溢出丢弃并计数（不阻塞校时线程）
  backend: ...
  core: ...
  sources: ...
```

## backend

| 键 | 默认 | 说明 |
|----|------|------|
| `mode` | `linux` | `linux`（自研唯一写入后端）｜`mock`（仅 test profile）｜`chrony`（扩展点，未实现，拒绝） |
| `allow_competing_writers` | `false` | 检测到 chronyd/ntpd 等竞争写入者时拒绝执行（FAULT） |
| `competing_processes` | `chronyd:ntpd:systemd-timesyncd:openntpd` | 竞争进程名列表 |

## core（阈值实际值登记）

| 键 | 默认 | 说明 |
|----|------|------|
| `max_source_age_ms` | 300000 | 候选最大年龄（300s）。**注**：SEC 的 300s 是安全接受上限，不自动成为本服务的运行策略默认值（设计 §4）。 |
| `max_source_error_ms` | 500 | 候选最大误差上界 |
| `require_authenticated_sources` | `false` | 未认证源可否用于校时（默认允许非安全同步，证据状态保持 unverified） |
| `stability_window_syncs` | 3 | 切源稳定窗口（连续合格采样数） |
| `switch_hysteresis_ms` | 2000 | 切源滞回时长 |
| `step_threshold_ms` | 1000 | |偏差| >= 此值 → step（冷启动/大偏差） |
| `slew_threshold_ms` | 50 | |偏差| <= 此值 → 视为已对齐（仅读回验证） |
| `alignment_tolerance_ms` | 50 | 读回验证容差 |
| `calibration_retry_limit` | 5 | 校准连续失败上限 → FAULT（含 slew 未收敛） |
| `holdover_max_ms` | 3600000 | 保持时长上限（1h） |
| `holdover_max_error_ms` | 1000 | 保持误差上界上限 |
| `drift_bound_ppm` | 100 | 漂移上界（U(t)=U0+drift×elapsed/1e6） |
| `monitor_interval_ms` | 1000 | 监测 tick 间隔 |
| `external_change_threshold_ms` | 200 | 外部改时/回拨判定阈值 |
| `fault_probe_interval_ms` | 5000 | FAULT 恢复探测间隔 |
| `synchronizing_timeout_ms` | 30000 | SYNCHRONIZING 校准超时 |
| `source_lost_age_ms` | 350000 | 源丢失判定（进入 HOLDOVER 的依据，实际由 max_source_age 触发） |
| `holdover_enter_min_age_ms` | 120000 | 最短保持时长（当前实现以 max_source_age 触发，保留字段） |
| `holdover_enabled` | `true` | 是否启用 HOLDOVER |
| `poll_interval_ms` | 30000 | 来源轮询间隔（调度层参考） |

## sources

### ntp（本次实际落地：Wi-Fi/以太网承载，NTP 服务器为时间源）

```yaml
sources:
  ntp:
    enabled: true
    servers:
      - id: ntp0
        host: pool.ntp.org      # 必填
        port: 123
        priority: 10            # 越小越优先
        timeout_ms: 3000
        poll_interval_ms: 30000
        queueing_budget_ms: 50  # 排队/抖动误差预算（计入误差上界）
        require_authenticated: false  # true 需要 auth_mode 实现（扩展点）
        auth_mode: none         # none | nts(扩展点) | symmetric(扩展点)
        drift_upper_ppm: 100
```

### cellular（协议为厂商契约，缺口登记）

```yaml
sources:
  cellular:
    enabled: true
    source_id: cell0
    priority: 30
    device: ""                 # 空 = 自动检测（无模块 → unsupported）
```

### gnss（驱动/协议契约缺口登记）

```yaml
sources:
  gnss:
    enabled: true
    source_id: gnss0
    priority: 20
    device: ""                 # 空 = 自动检测
```

### rtc（仅后备；需可证明健康/provision/误差可界定）

```yaml
sources:
  rtc:
    enabled: true
    source_id: rtc0
    priority: 40
    device: ""                 # 空 = /dev/rtc0
    require_provisioned: true  # provision 证据接口为缺口（无证据默认拒绝作为后备源）
```

## 校验规则（纯校验，无副作用）

- 类型/范围/冲突校验；`production` profile 拒绝 `backend.mode=mock`（AT-12 fake 隔离）。
- `chrony` 复用、NTS/对称密钥认证为登记扩展点：配置直接拒绝（不占位标完成）。
- 重复 `source_id` 拒绝；未知 `auth_mode` 拒绝；非法阈值（<=0/越界）拒绝。
