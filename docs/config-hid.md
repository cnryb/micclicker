# MicClicker 配置 HID 线格式

> 权威位置：本文。实现对照 `firmware/s3/main/usb_prototype.c`。  
> 本文件只描述 **S3 vendor 配置 HID**（状态查看与快捷键映射），不描述 ESP-NOW 空口（见 [protocol.md](protocol.md)）。

## 1. 传输

| 项 | 值 |
| :--- | :--- |
| VID:PID | `303a:4014`（原型，非正式量产 ID） |
| 接口 | vendor-defined HID（复合设备第 4 个接口） |
| Report ID | `0`（无编号前缀；payload 正好 64 字节） |
| 端点 | OUT `0x03` / IN `0x83` |
| 写命令 | HID Output / SET_REPORT，整帧 64 字节入有界队列（深度 8） |
| 读状态 | HID Input / GET_REPORT；**不是** Feature |

非法长度、未知 opcode 或非法映射只增加拒绝计数并返回错误结果，不改变按键状态、不落盘。

## 2. 命令

`buffer[0]` 为 opcode；其余字节按命令解释。保留 stage-2 自测 `0x01–0x04`。

| opcode | 名称 | SET 载荷 | 行为 |
| :---: | :--- | :--- | :--- |
| `0x01` | PING | 仅 opcode | 递增 `ping_count` |
| `0x02` | LEFT_ALT | 仅 opcode | 经当前映射自测 UP 键按/放 |
| `0x03` | ENTER | 仅 opcode | 经当前映射自测 OK 键按/放 |
| `0x04` | ALL_KEYS_UP | 仅 opcode | 释放全部按键 |
| `0x10` | READ_MAP | 仅 opcode | 置门控：下一次 GET 的字节 48–63 为 map blob |
| `0x11` | WRITE_MAP | `[0x11]` + 16 字节 blob + pad | 校验后 `apply_map`（内部 all-keys-up + NVS）；非法 → `INVALID`，不落盘 |
| `0x12` | RESET_MAP | 仅 opcode | 恢复默认映射并持久化 |

### 2.1 结果码（`last_result`，状态字节 3）

| 值 | 含义 |
| :---: | :--- |
| `0` | OK |
| `1` | INVALID |
| `2` | BUSY（已入队，任务未完成） |
| `3` | QUEUE_FULL |
| `4` | NOT_READY |

入队成功后立即置 `BUSY`；任务结束后改为 `OK` / `INVALID` / `NOT_READY`。

### 2.2 读映射门控

- 普通 GET：字节 48–63 为 stage-4 链路/缓冲计数（见下表）。
- `READ_MAP` 成功后（`last_result != BUSY`）、**下一次** GET：字节 48–63 改为 16 字节 map blob；该次 GET 后清门控。`BUSY` 期间的轮询 GET 不消费门控。
- 其他命令入队时取消未消费的门控。
- blob 布局与 `firmware/components/key_map` 的 NVS 编码一致：

| 偏移 | 内容 |
| :---: | :--- |
| 0–3 | version `u32 LE = 1` |
| 4–5 | UP：`modifier`, `keycode`（6–7 为 pad） |
| 8–9 | DOWN |
| 12–13 | OK |

默认：UP = Right Alt（`modifier=0x40`, `keycode=0`）、DOWN = 无输出、OK = Enter（`keycode=0x28`）。

## 3. GET 状态报告（64 字节）

字节 0–47 为 stage-2 布局，**语义不得更改**；48–63 为后续扩展（普通 GET 为链路计数；门控读 map 时被覆盖）。

| 偏移 | 类型 | 语义 |
| :---: | :--- | :--- |
| 0 | u8 | 状态版本 `=1` |
| 1 | u8 | flags（见下） |
| 2 | u8 | `last_command` |
| 3 | u8 | `last_result` |
| 4 | u32 LE | ping_count |
| 8 | u32 LE | alt_count（CMD `0x02` 次数） |
| 12 | u32 LE | enter_count |
| 16 | u32 LE | all_up_count |
| 20 | u32 LE | rejected_count |
| 24 | u32 LE | audio_produced_frames |
| 28 | u32 LE | audio_short_writes |
| 32 | u32 LE | audio_usb_packets |
| 36 | u32 LE | audio_usb_bytes |
| 40 | u32 LE | audio_empty_packets |
| 44 | u32 LE | audio_alt_transitions |
| 48 | u32 LE | `audio_rx \| (audio_gap << 16)`（或 map blob 前 4 字节） |
| 52 | u32 LE | `audio_dup \| (audio_late << 16)` |
| 56 | u32 LE | `underruns \| (overruns << 16)` |
| 60 | u32 LE | `fill_ms` |

### 3.1 flags（字节 1）

| bit | 含义 |
| :---: | :--- |
| `0x01` | USB mounted |
| `0x02` | UAC streaming |
| `0x04` | 自测 Alt 曾按下（stage-2 观测位） |
| `0x08` | 自测 Enter 曾按下 |
| `0x10` | config_error |
| `0x20` | paired（`mic_session_get().paired`） |
| `0x40` | link_up（与产品侧链路存活一致：`!link_lost`） |

旧工具可忽略 `0x20` / `0x40`。

## 4. 宿主工具

按需 CLI：`tools/micctl/`（Python + hidapi）。不做常驻转发，不参与音频或键盘 HID。

本批范围：读状态、读/写/恢复映射。发起配对与解除绑定**尚未**进入命令集。

## 5. 兼容规则

1. 字节 0–47 字段偏移与既有含义保持不变。
2. 新命令只追加 opcode，不改写 `0x01–0x04`。
3. 读 map 不得永久替换 48–63；门控单次有效。
4. 写映射必须先校验；失败不落盘、不留下半更新。
