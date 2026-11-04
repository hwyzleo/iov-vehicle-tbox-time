# TBOX-TIME 实现说明与缺口登记（TBOX-TIME-DSN-CR-001）

## 实现范围（本次落地）

按设计 §8 实施顺序，本次完成：
1. **TIME 核心**：5 状态状态机（UNSYNCHRONIZED/SYNCHRONIZING/SYNCHRONIZED/HOLDOVER/FAULT）、
   选源（筛选→优先级→稳定窗口/滞回→失效立即撤销）、校时（冷启动 step/运行期 slew/
   回拨先撤销再重同步/读回验证 alignment_error_ms）、保持模型（U(t)=U0+drift×elapsed/1e6）。
2. **真实网络授时**（本次实际落地）：RFC 5905 NTP v4 UDP 客户端（真实 socket/报文/offset 计算/
   误差上界=RTT/2+排队预算/BOOTTIME 计龄/服务器重启→source_epoch 递增）。Wi-Fi/以太网为网络
   承载，NTP 服务器为时间源本身。**普通未认证 NTP → auth_status=unverified，仅用于明确标识的
   非安全同步**；TIME 不输出 security_trusted 语义（AT-08）。
3. **Linux 校时后端**（唯一写入方）：`clock_settime`（step）/`adjtimex(ADJ_OFFSET)`（slew）/
   读回验证；竞争写入者检测（chronyd/ntpd/systemd-timesyncd/openntpd → 默认拒绝执行 → FAULT）。
   非 Linux 平台 capability=Unsupported（如实标记）。
4. **只读 IPC + client SDK**：`getTimeSnapshot`/`subscribeTimeStateChanged`；事件
   `time.state_changed`（daemon_epoch/sequence 对账、先订阅缓冲再查询、乱序/重复/丢失）；
   无通用 setTime 方法；错误分类（不可达/超时/不支持/数据非法/时间不可用）。
5. **配置**：yaml 纯校验（类型/范围/冲突、production 禁 mock、扩展点拒绝），不调钟不联网。
6. **生命周期**：FW Application 编排；停机撤销状态/停来源/释放后端；事件有界队列不阻塞校时。

## 待盘点（ORIN 实机，未交付能力如实标记）

| 项 | 状态 | 说明 |
|----|------|------|
| ORIN 系统校时组件盘点 | 待盘点 | 开发机（macOS）无 chrony/ntpd；ORIN 上需确认已有校时进程并按 `allow_competing_writers` 治理，保证唯一写入方 |
| 真实 Linux 后端联调 | 待实机 | `LinuxSystemClockBackend` 已真实实现；在 ORIN 验证 step/slew/读回/权限失败/竞争进程 |
| 真实 NTP 服务器联调 | 待实机 | 本地 UDP mock NTP 已验证取时路径；公网/Wi-Fi 实网验证待 ORIN |
| 冷启动引导 | 待实机 | 错误初始时钟下的独立引导（AT-11）：NTP 不经认证即可同步（非安全路径），认证引导需另行验证 |
| 休眠唤醒 | 待实机 | BOOTTIME 计龄已实现（含休眠）；唤醒失效旧样本逻辑待 ORIN 验证 |
| 权限模型 | 待实机 | daemon 以最小校时权限运行、IPC 消费者与适配器来源受授权（设计 §6）——部署编排待工程实现 |

## 缺口登记（保留扩展点，不占位标完成）

| 缺口 | 现状 | 后续 |
|------|------|------|
| 蜂窝时间协议 | `CellularSourceAdapter` 检测模块存在性；无模块→unsupported；有模块但厂商时间协议（AT+CCLK/CMT 等）未登记→如实 unsupported 并注明缺口 | 厂商协议契约到齐后实现 |
| GNSS 驱动/协议 | `GnssSourceAdapter` 检测设备；NMEA 协议契约未登记 | 驱动/协议契约到齐后实现 |
| RTC provision/电池健康证据 | `RtcSourceAdapter` 真实读取 /dev/rtc0（RTC_RD_TIME）；`require_provisioned=true` 时无证据接口→默认拒绝作为后备源 | 平台 provision 证据接口落地后接入 |
| NTP 认证（NTS/对称密钥） | `auth_mode=none`；配置 `nts`/`symmetric` 直接拒绝 | 认证机制实现并验证冷启动引导条件 |
| chrony 复用后端 | `backend.mode=chrony` 配置拒绝（扩展点） | 若 ORIN 盘点决定复用 chrony：需提供本规范所需事实（offset/RTT/误差），不能凭 synchronized 标志 |
| SEC 配对 | TIME 输出事实；`PlatformTimeAdapter` 适配/可信 IPC 属 SEC 配对 CR（并行推进，AT-09 不以此处单测代替） | SEC 配对 REQ/DSN CR |
| BUILD 集成/SDK 安装 | 本 CMake 仅 HOST 构建；client SDK 安装包（TboxTimeClient）与 Orin 交叉编译待接入 BUILD | 后续 CR |

## 与兄弟项目的关系

- FW：复用 `tbox::fw::ipc`/`application`/`log`；FW 不依赖 TIME client（设计 §2）。
- SEC：TIME 输出同步/来源证据/新鲜度/误差/对齐事实；SEC 保留 300s/2s 与回拨保护
  （设计 §7，本 CR 不修改）。`CompositeTrustedTimeProvider` 的 `PlatformTimeAdapter`
  需要平台 getTrustedUtc() 等价事实——即本服务快照。
- MQTT：只消费 SEC，不直接调用 TIME（MQTT-DSN-CR-018 约束，本 CR 不改）。

## 测试证据

- 单元测试：状态机/选源/保持/快照/后端/配置（tests/）。
- NTP 真实取时：本地 UDP mock NTP 服务器（真实 RFC 5905 报文）验证
  报文构造/解析/offset/误差/认证状态/服务器重启代次（tests/test_ntp_client.cpp）。
- 集成：daemon 全链路（UDP NTP → 核心 → 后端 → IPC → client）覆盖
  无源→同步→断网 HOLDOVER→超限失效、事件送达、多消费者并发只读
  （integration/test_daemon_e2e.cpp）。
- 校时后端为测试 profile 的 MockBackend（自动流逝模拟系统墙钟）；
  真实 Linux 后端在 ORIN 实机验收（见上表）。

## 运行

```bash
# HOST 构建（依赖：TboxFramework/GTest at ~/.local，yaml-cpp/nlohmann_json via homebrew）
source ../iov-vehicle-tbox-framework/scripts/tbox-env.sh
cmake -B build -DCMAKE_PREFIX_PATH="$TBOX_PREFIX;/opt/homebrew"
cmake --build build -j
cd build && ctest            # 全部测试

# 冒烟（test profile + 模拟后端；production 用真实 NTP + Linux 后端）
cp ../config/time.example.yaml config/time.yaml
./build/tbox_time
```
