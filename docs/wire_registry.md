# TBOX-TIME wire 登记（TBOX-TIME-DSN-CR-001）

> 设计 §3/§6：落地时核对现有框架并登记 wire/MethodId/EventId。本表为本地登记，
> 与 BUILD 统一 wire 登记（TBOX-BUILD 工程）的合并为待办（见 implementation_notes.md）。

## IPC 传输

- 复用 FW `tbox::fw::ipc`（AF_UNIX + JSON 载荷，Request/Response/Event 帧）。
- socket 路径：默认 `/tmp/tbox-time.sock`（配置 `time.ipc.socket_path`）。

## MethodId

| MethodId | 名称 | 请求 params | 响应 |
|----------|------|-------------|------|
| `0x0001` | `getTimeSnapshot` | `{"contract_version": N}`（可省略） | `{status, snapshot}` |
| `0x0002` | `subscribeTimeStateChanged` | `{"contract_version": N}` | 握手响应 = 当前快照（对账基线），随后推送 `time.state_changed` 事件 |

业务只读：无通用 setTime 方法（AT-03）。未知方法返回 `{"status":"unsupported"}`。

## EventId

| EventId | 名称 | 载荷 |
|---------|------|------|
| `0x0001` | `time.state_changed` | `TimeStateEvent` JSON（daemon_epoch/event_sequence/source_epoch/old_state/new_state/reason/质量摘要） |

订阅注册：服务端在 subscribe 握手时显式 `add_subscription(client_fd, 0x0001)`（FW Server 不自动注册）。

## 响应状态（业务层）

| status | 语义 |
|--------|------|
| `ok` | 服务可达且当前有有效时间（SYNCHRONIZED / HOLDOVER 提供 UTC 估计） |
| `time_unavailable` | 服务可达但当前无有效时间（UNSYNCHRONIZED / SYNCHRONIZING / FAULT），快照字段明确 unknown |
| `unsupported` | 版本/方法不支持 |
| `invalid` | 请求数据非法 |
| `internal_error` | 快照校验失败（不应出现） |

## 快照 JSON 字段

见 `src/ipc/time_json.cpp` 序列化实现；语义对应设计 §5 表格。
时间戳统一 UTC epoch 毫秒；unknown 不写零。

## 对账语义（client）

- 先订阅（FW 内部完成握手）→ 缓冲事件 → `getTimeSnapshot` 建立基线（daemon_epoch/event_sequence）。
- daemon_epoch 改变 → 旧缓存不可用，重新查询；event_sequence <= 基线 → 丢弃（乱序/重复）。
- 支持乱序、重复、丢失；绝不承诺 exactly-once（设计 §6）。
