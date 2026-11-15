# 按键链路真机验收（第 5 阶段）

本目录是 [DESIGN.md](../../../docs/DESIGN.md) 第 5 阶段（按键与故障联调）的判分工具与实测记录。它判的是**物理按键 → C3 采样去抖 → 无线 → S3 映射 → HID 键盘报告 → 主机收到**这条完整路径，不是单端自检，也不是配置 HID 通道触发的按键。

## 1. 被测路径

```
C3 (AI Passport)                                    S3 (ESP32-S3-N16R8)                 主机 (macOS)
GPIO0 ADC 电阻梯                                     UAC2 + HID 键盘
  │ 10 ms 采样                                          ▲
  ├─ 窗口分类 (key_scan, 40/120 ms 去抖)                 │
  ├─ KEY 帧发送, 同 seq 重传 (key_tx) ── ESP-NOW 加密单播 ─┤ 按 seq 去重
  │        ▲ KEY_ACK（报告送达后回）                     ├─ 查映射表 (key_map/key_store, NVS)
  └─ 心跳 1 Hz ────────────────────────────────────────┤ 生成 8 B HID 报告
         └─ S3 断链超时 3 s → all-keys-up               └─ tud_hid_n_keyboard_report
```

两端均为产品固件（`./firmware/build.sh c3` 与 `./firmware/build.sh s3`）。默认映射：UP = Left Alt（仅修饰键）、DOWN = 保留（无输出）、OK = Enter；映射持久化在 S3 的 NVS（独立命名空间 `mickeys`，与配对绑定 `miclink` 互不干扰）。

## 2. 判分脚本

```bash
# 三个日志缺一不可：C3 串口日志、S3 串口日志、主机观察器输出。
./firmware/tests/key_link/check.sh <c3 日志> <s3 日志> <观察器日志> \
    [--expect-pairs N] [--expect-timeout] [--expect-allup]
```

| 检查 | 判据 | 理由 |
| :--- | :--- | :--- |
| key sampling | `read_err=0` | ADC 读失败时按键状态未知，后续全部不可测 |
| no event expired / dropped | 0 / 0 | 过期或丢弃意味着事件在到达主机前就死了 |
| queue drained (两端) | `pending=0` | 挂起的事件说明 ACK 没回来，是单向链路而非按键问题 |
| all events acknowledged | `ack >= submitted` | 干净运行里每个事件都应被确认 |
| retransmission rate | ≤ 50% 的传输 | 重传是正常机制，但比例过高说明链路撑不住 |
| heartbeat round trip | ≥ 90% 被回复 | 心跳同时是链路存活信号与 RTT 来源 |
| frames accounted for | `frames == applied + dup` | 每个到达的 KEY 帧要么被应用要么被判重，没有第三种 |
| reports emitted | > 1 | 每次状态变化一个报告，加上开机全零报告 |
| every ACK transmitted | `ack_fail=0` | ACK 丢失会被发送端重传补偿，但持续失败要暴露 |
| no stuck key at end | `down=0x00` | **验收红线**：任何非零 mask 就是卡键 |
| mapping loaded | gen 字段存在 | 映射必须从 NVS 或默认值成功加载 |
| link timeout observed | `timeouts>0`（加 `--expect-timeout` 时） | 断链故障注入必须有对应的超时与释放 |
| all-keys-up observed | `all_up>0`（加 `--expect-allup` 时） | 每个触发点都要有日志与全零报告 |
| host saw key pairs | ≥ N 或 > 0 | 主机观察器必须看到成对按下/释放 |
| press/release balance | `up ≤ down` | 释放多于按下说明有幽灵释放 |
| sender events reached host | C3 events>0 ⇒ host pairs>0 | 两端都报了事件而主机没收到 = USB 侧回归 |

判分脚本**不判**按键次数与延迟指标（不预设传输距离或延迟结论，按实测记录）。

## 3. 主机观察器

`key_observer <秒> [--expect-pairs N] [--quiet]` 用 IOHIDManager 以**非独占**方式监听 MicClicker 键盘接口（VID `303a` / PID `4014`），只观察、不发送任何配置命令。它逐条打印 `KEY_EVENT page=... usage=... value=...`，收尾输出 `KEY_SUMMARY down=N up=N pairs=N mods=0x.. keys=0x..`。

它故意**不经过配置 HID**：配置通道的 ALT/ENTER 自测命令走的是 S3 内部同一条按键状态机（见第 6 节），能验证「S3 → HID」这一段，但不能证明物理按键和无线链路。两者的测试必须分开描述（AGENTS.md 第 4 节）。

构建：`./firmware/tests/key_link/build_observer.sh`（仅 macOS，需要 IOKit 框架）。

## 4. 实测记录

### 4.1 首轮端到端（2026-11-15）

条件：C3 与 S3 各自接 USB 同一台 macOS 主机；ESP-NOW 信道 1，约 1 米直视；两侧 `app-flash` 烧录（未动 NVS，配对绑定保留，重启后自动重连 `paired=1`）。

已实测的部分：

| 项 | 结果 |
| :--- | :--- |
| 芯片身份核对 | C3 `98:c3:77:f6:c6:44` rev v1.1；S3 `c4:9e:7e:3c:d5:d4` rev v0.2（`esptool chip_id`） |
| S3 启动 | `S3_KEYS: READY`，映射从默认值加载（NVS 尚无记录），`gen=0` |
| C3 启动 | `C3_KEYS: READY sample_ms=10 confirm_ms=40 hold_ms=120 retry_ms=60 attempts=4 windows=150/447/1900mv` |
| ADC 通路 | 空闲 2918 mV 恒定（`mv=2918..2918`，与 hardware-c3.md 第 4 节 RELEASED 档一致），`read_err=0` |
| 心跳 | C3 每秒 1 发，`hb_ack == hb` 全部被回复，`hb_miss=0` |
| S3 → HID（配置通道触发） | 经配置 HID 的 ALT 自测命令走同一条按键状态机：`applied=2 reports=4`，主机观察器看到 `usage=226 value=1/0`，5/5 次成对 |
| 断链超时 | 烧录重启后的首次窗口内出现过一次 `timeouts=1`（S3 先于 C3 起来时不再误报，见 4.2 修正 ②） |

**物理按键首轮（2026-11-15，`.tmp/stage5-20261115-120910/phys3/`）**：34 对 UP/OK 按下/释放。C3 记录 68 事件、发送 79 帧（11 次重传）、ACK 68、零过期零丢弃；S3 收 79 帧，判重 10 次；主机观察器 `pairs=69`、`mods=0x04`、`keys=0x28`，成对 Alt/Enter 全部可见。**注意这轮是在会话号修复之前**（见 4.2 修正 ⑤）：S3 端 `rx=69 applied=49 dup=10` 存在一次重传被重复应用（seq=13 出现两次），并且会话翻转触发了 35 次多余的 all-keys-up。修复后的空窗口验证（phys4–phys6）`SESSION_NEW session=0` 为 0、all_up 为 0、心跳 100% 确认；**修复后带按键的判分记录仍待补**（需要人手按键）。

**待补**：修复后带按键的完整判分、1000 次按键、强制断链 20 次、与 50 fps 音频并发判分。这些不能由脚本替代，脚本只能观察。

### 4.2 首轮发现并当场修正的问题

1. **心跳回复没有回显 seq**。S3 原来在回复里固定 `echo_seq=0`，C3 侧因此无法把回复与自己的心跳对应起来。已改为 S3 记录待回复心跳的 `seq` 并在回复中回显（`protocol.md` 3.4 本来就要求回显，这是产品固件漏了这一条）。
2. **S3 开机即报一次假超时**。静默计时器原来从 boot 起算，S3 先于 C3 完成启动时必然先报一次 `LINK_TIMEOUT`。已改为「收到本会话第一帧后才起算」：没有链路时释放按键本来就是空操作，「还没连上」与「链路死了」不是同一种失败。
3. **`reports_retried` 语义修正**。原实现按等待毫秒数累加（一次被阻塞的报告能贡献 20），与「主机没在轮询」的本意不符。已改为每次报告成功时最多计 1。
4. **按键状态机只有一个写者**。第 2 阶段的配置 HID 自测命令原来绕过按键状态机直接 `tud_hid_n_keyboard_report`，与新的无线按键路径是两个写者，会竞态。已把自测命令改为 `mic_link_keys_self_test()`，与无线事件走同一条队列、状态机、报告路径（`seq=0` 表示本地事件，不产生 ACK）。
5. **KEY/HEARTBEAT 帧的 session 字段必须与音频共用**（首轮实测发现的最重要问题）。C3 按键任务最初用 `session=0` 发 KEY 与 HEARTBEAT，而音频任务用真实会话号；S3 每收到一帧按键就触发 `SESSION_NEW`，重置按键序号跟踪器并执行 all-keys-up——重传的 `seq=13` 因此不再被判重、被应用了两次，且 3 分钟内翻转触发了 35 次多余的 all-keys-up。修法：C3 端 `micclicker_c3_keys_start(session)` 传入音频的会话号，KEY/HEARTBEAT 都用它；S3 端 KEY_ACK 回显事件到达时的会话号（`protocol.md` 第 2 节「ACK 回送原值」同样适用）。修复后空窗口验证 `SESSION_NEW session=0` 为 0、心跳 100% 确认。

## 5. 复现步骤

```bash
# 1) 构建（仓库根目录）
./firmware/build.sh s3
./firmware/build.sh c3

# 2) 烧录前核对芯片身份（禁止凭 /dev 节点名判断设备）
esptool.py --chip esp32s3 --port /dev/cu.usbmodemXXXX chip_id
esptool.py --chip esp32c3 --port /dev/cu.usbmodemYYYY chip_id

# 3) 只烧应用分区（保留 NVS 绑定与映射）
idf.py -C firmware/s3 -B firmware/s3/build -p /dev/cu.usbmodemXXXX -b 460800 app-flash
idf.py -C firmware/c3 -B firmware/c3/build -p /dev/cu.usbmodemYYYY -b 460800 app-flash

# 4) 抓两端日志 + 主机观察器（同一次运行，时间窗覆盖按键动作）
python3 /tmp/serialcap.py /dev/cu.usbmodemYYYY c3.log 60 2 &
python3 /tmp/serialcap.py /dev/cu.usbmodemXXXX s3.log 60 2 &
./firmware/tests/key_link/build_observer.sh
./firmware/tests/key_link/key_observer 56 > observer.log &
# —— 此时按 C3 的 UP / OK ——
wait

# 5) 判分
./firmware/tests/key_link/check.sh c3.log s3.log observer.log
```

抓串口日志的注意事项与 `tests/audio_link/README.md` 相同：中途 attach 会回放积压 backlog 且头行被截断，采集脚本要先丢弃前 2–3 秒并跳到第一个完整行，否则判分不可信。

## 6. 与配置通道自测的关系

S3 配置 HID 的 `CMD_LEFT_ALT` / `CMD_ENTER` / `CMD_ALL_KEYS_UP`（stage-2 原型遗留）现在**通过** `mic_link_keys_self_test()` 进入与无线按键相同的队列和状态机，只是 `seq=0`、不产生 KEY_ACK。它能独立验证「映射表 → 报告 → 主机」这一段，在第 5 阶段真机判分里作为 USB 侧的对照；它不能替代物理按键路径，判分脚本要求的三个日志里 observer 那一份必须来自真实按键运行。

## 7. 已知限制（不得据此宣称已全面通过）

- **物理按键端到端尚无完整判分记录**（2026-11-15 首轮进行中，见 4.1 待补项）。
- **只测了 macOS** 的 HID 接收；Windows / Linux 的键盘枚举与按键行为未测。
- **映射修改 / 恢复默认 / 解除绑定**的配置命令集尚未实现（属工作线 C1），当前映射固定为默认值，写入路径 `mic_key_store_save` 已有校验但无外部入口。
- **按键延迟未测**：设计上是采样(10) + 去抖(40) + 链路(<20) + 报告轮询(≤1 ms 量级)，但没有实测数据，不预设结论。
- 心跳超时判据是单向的（C3 看回复、S3 看任意帧），两边阈值独立，没有做双向握手一致性验证。
