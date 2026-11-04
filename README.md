# iov-vehicle-tbox-time

车联网车端 TBOX 的统一时间源管理与系统校时服务（TBOX-TIME）。

## 定位

单实例 TIME 服务 + 只读 client SDK：本机时间源管理、选源、校时、持续监测、
只读查询（快照）与状态变化通知（事件）。逻辑唯一校时责任方（TBOX-TIME-DSN-CR-001）。

## 核心能力

- **状态机**：UNSYNCHRONIZED / SYNCHRONIZING / SYNCHRONIZED / HOLDOVER / FAULT；
  `synchronized` 只能由状态推导。
- **真实网络授时**（RFC 5905 NTP v4，Wi-Fi/以太网承载）：真实取时、误差上界
  （RTT/2 + 排队预算）、BOOTTIME 计龄、服务器重启代次检测；普通 NTP 明确标记
  `unverified`（非安全同步），TIME 不输出安全 Trusted 语义。
- **Linux 校时后端**（唯一写入方）：`clock_settime` / `adjtimex` / 读回验证 /
  竞争写入者检测（chronyd/ntpd/systemd-timesyncd）。
- **HOLDOVER 保持**：U(t) = U0 + drift_ppm×elapsed/1e6；超龄/超误差/漂移未知 → 不可用。
- **只读 IPC + client**：`getTimeSnapshot` / `subscribeTimeStateChanged`；
  版本化快照 + `time.state_changed` 事件（daemon_epoch/sequence 对账）；
  无通用 setTime 入口。
- **配置纯校验**：yaml schema，production 拒绝 mock，扩展点（chrony/NTS/蜂窝/GNSS 协议）
  如实拒绝或登记缺口，不占位标完成。

## 构建与测试

```bash
source ../iov-vehicle-tbox-framework/scripts/tbox-env.sh
cmake -B build -DCMAKE_PREFIX_PATH="$TBOX_PREFIX;/opt/homebrew"
cmake --build build -j
cd build && ctest          # 单元 + 集成测试
```

## 文档

- `docs/wire_registry.md`：MethodId/EventId/消息格式登记
- `docs/config_schema.md`：配置 schema 与默认值登记
- `docs/implementation_notes.md`：实现说明、ORIN 待盘点、缺口登记
- `docs/acceptance_tracking.md`：AT-01～12 验收证据映射

## 状态

CR-001（TBOX-TIME-DSN-CR-001）核心实现完成；真实 Linux 校时与实网 NTP 待 ORIN 实机验收；
SEC 配对（AT-09）并行推进。
