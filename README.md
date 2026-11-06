# iov-vehicle-tbox-time

车联网车端 TBOX 的统一时间源管理与系统校时服务（TBOX-TIME）。

## 定位

单实例 TIME 服务 + 只读 client SDK：本机时间源管理、选源、校时、持续监测、
只读查询（快照）与状态变化通知（事件）。逻辑唯一校时责任方（TBOX-TIME-DSN-CR-001/002）。

## 核心能力

- **状态机**：UNSYNCHRONIZED / SYNCHRONIZING / SYNCHRONIZED / HOLDOVER / FAULT；
  `synchronized` 只能由状态推导。
- **system-observe 观测形态**（CR-002，Orin 目标）：只读 `adjtimex(modes=0)` +
  D-Bus `org.freedesktop.timesync1`（systemd-timesyncd）观测，TIME 不写钟、不需要
  CAP_SYS_TIME；保守不确定度（maxerror/根距离取大 + tolerance 增长）与真实测量新鲜度
  （DestinationTimestamp + BOOTTIME 计龄）；观测事实驱动状态映射；`kChronyObserve`
  退役为 `kSystemObserve` 废弃别名。
- **真实网络授时**（RFC 5905 NTP v4，Wi-Fi/以太网承载，linux 后备形态）：真实取时、
  误差上界（RTT/2 + 排队预算）、BOOTTIME 计龄、服务器重启代次检测；普通 NTP 明确标记
  `unverified`（非安全同步），TIME 不输出安全 Trusted 语义。
- **Linux 校时后端**（唯一写入方，linux 后备形态）：`clock_settime` / `adjtimex` /
  读回验证 / 竞争写入者检测（chronyd/ntpd/systemd-timesyncd）。
- **HOLDOVER 保持**：U(t) = U0 + drift_ppm×elapsed/1e6；超龄/超误差/漂移未知 → 不可用。
- **只读 IPC + client**：`getTimeSnapshot` / `subscribeTimeStateChanged`；
  版本化快照 + `time.state_changed` 事件（daemon_epoch/sequence 对账）；
  无通用 setTime 入口。
- **配置纯校验**：yaml schema，production 拒绝 mock；`system-observe` 下写钟专属键与
  自采源拒绝；chrony 已退役拒绝并提示；NTS/蜂窝/GNSS 协议如实拒绝或登记缺口，不占位标完成。

## 构建与测试

```bash
source ../iov-vehicle-tbox-framework/scripts/tbox-env.sh
cmake -B build -DCMAKE_PREFIX_PATH="$TBOX_PREFIX;/opt/homebrew"
cmake --build build -j
cd build && ctest          # 单元 + 集成测试
```

## 文档

- `docs/wire_registry.md`：MethodId/EventId/消息格式登记（含 CR-002 新增 TimeReason 16/17/18）
- `docs/config_schema.md`：配置 schema 与默认值登记（含 system-observe 段）
- `docs/implementation_notes.md`：实现说明、ORIN 待盘点、缺口登记（含 CR-002 跨仓登记）
- `docs/acceptance_tracking.md`：AT-01～15 验收证据映射

## 状态

CR-002（TBOX-TIME-DSN-CR-002）system-observe 观测形态核心实现完成（Level A）；
真实 Linux 校时、system-observe 观测（D-Bus/adjtimex）与实网 NTP 待 ORIN 实机验收（Level B）；
SEC 配对（AT-08/09/11，Level C）并行推进。
