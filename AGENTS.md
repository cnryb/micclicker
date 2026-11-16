# MicClicker 工程协作指南

本文件约束仓库内的开发、验证和文档工作。开始修改前先阅读 [docs/DESIGN.md](docs/DESIGN.md)；子目录若有更具体的 `AGENTS.md`，同时遵循对应目录的规则。

## 1. 项目目标与当前状态

MicClicker 是一套一对一无线语音输入与快捷键设备：

- AI Passport（ESP32-C3）采集麦克风 PCM、检测 UP / DOWN / OK 按键，进行 G.722 编码，并通过 ESP-NOW 发送到 ESP32-S3。
- ESP32-S3 接收并解码音频，在电脑上枚举为免驱 USB 复合设备：UAC 麦克风、标准 HID 键盘、独立 vendor-defined HID 配置接口。
- 正常使用不需要专用驱动或常驻桥接软件。按需配置工具只负责状态查看、配对管理和快捷键配置，不参与音频或 HID 转发。

当前仓库已完成第 1 阶段底层自检、第 2 阶段 macOS USB 原型、第 3 阶段编码与无线分测、**第 4 阶段音频全链路（macOS）**，以及**第 5 阶段按键链路（macOS）**；尚无跨平台配置工具源码与完整的第 5 阶段验收记录（1000 次按键、20 次强制断链、音频并发判分仍待做）。S3 板卡的芯片级实测、工具链和已知限制见 [docs/hardware-s3.md](docs/hardware-s3.md)；AI Passport（C3）的硬件事实、已验证项与仍缺的验证见 [docs/hardware-c3.md](docs/hardware-c3.md)。两端芯片级备份已完成；S3 的八线 PSRAM、Wi-Fi 和运行态 BOOT 长按，以及 C3 的 Wi-Fi、四键态 ADC 分布均已真机通过。UAC2 麦克风、键盘 HID 和独立配置 HID 已在 macOS 上完成并发录音、成对键事件和配置通信。第 3 阶段的 G.722 部分已完成：`firmware/components/g722/` 内嵌 WebRTC/SpanDSP 公有领域实现，与 ffmpeg 逐比特一致，C3 实机自检通过（3.26 ms/帧，20 ms 预算的 16%）；**ITU 官方向量仍未验证**（需 ITU 账号，见 `firmware/tests/g722/README.md`），不能据此宣称通过 ITU 一致性。第 3 阶段的 ESP-NOW 部分已完成协议层与真机分测：`firmware/components/link_proto/`（帧编解码、序号规则）、`link_radio/`（传输封装）、`link_bind/`（配对绑定持久化）、`link_probe/`（两端探针）；协议规范见 [docs/protocol.md](docs/protocol.md)，实测数据与边界见 [firmware/tests/espnow/README.md](firmware/tests/espnow/README.md)。真机（C3→S3，信道 1，1 米直视）四轮零丢包、突发 406–507 kbit/s、RTT 平均 5.7–8.2 ms、20 按键事件全应用且 40 次重传全抑制、配对与加密单播往返通过。其中「重启后按旧绑定自动重连」已在第 4 阶段由产品固件补上（见下）。**仍未做**：配对过程防中间人（当前配对请求明文）、距离/穿墙/干扰。

第 4 阶段的音频全链路已在 macOS 打通（2026-11-15）：C3 经 ES8311+I2S 采集 16 kHz/16 bit/单声道，每 20 ms 编码一帧 G.722 并一帧一包加密单播；S3 解码后经**自适应环形缓冲**（`firmware/components/audio_jitter/`，高水位丢最旧帧、低水位复帧、缺口遮蔽，240 605 项主机侧检查）输出到 UAC2 端点，主机枚举为 16 kHz 单声道麦克风并录到真实音频。产品侧链路 bring-up 在 `firmware/components/link_session/`，与探针的关键差别是**正常开机绝不清除绑定**；C3 未绑定时作为发起方配对，S3 未绑定时常驻监听。判分脚本 `firmware/tests/audio_link/check.sh`，实测与边界见 [firmware/tests/audio_link/README.md](firmware/tests/audio_link/README.md)。

**第 4 阶段仍未做**：ASR 效果与同环境有线麦基线对比、端到端延迟分解、Windows / Linux、距离/穿墙/干扰、多语速长稳。**特别注意漂移补偿的长期行为尚未在真机压到**：本对 C3/S3 实测相对漂移约 0.00017%，30 分钟运行只触发 3 次 1 ms 补偿，缓冲全程 4–82 ms、零溢出零欠载，但这只能说明这对板卡漂移小，不能说明控制器在大漂移下也收敛（主机侧「快/慢 0.42% 各模拟一小时不发散」是模型证据，非真机）。按键链路已接入产品固件（第 5 阶段）：C3 经 `components/key_scan`（ADC 窗口分类 + 去抖）与 `components/key_tx`（同 seq 重传/ACK/去重）发送 KEY 帧并与音频并发；S3 经 `components/key_map`（映射校验与 HID 报告编码）与 `components/key_store`（映射 NVS 持久化，独立命名空间 `mickeys`）驱动 `mic_link_keys` 状态机输出 HID 报告，KEY_ACK 在报告被主机接收后才回。心跳 1 Hz、S3 断链 3 s 超时 all-keys-up、会话变更/USB 重置/挂起均触发释放。真机首轮 68 事件（34 对 UP/OK）零丢弃零过期，主机经 IOHIDManager 观察到成对 Alt/Enter；**完整判分（1000 次按键、强制断链、并发音频判分）与 Windows/Linux 未做**，实测边界见 `firmware/tests/key_link/README.md`。Windows / Linux、热插拔、休眠恢复和功耗仍须验证，不能把当前状态写成全平台通过。

目录与构建入口：

- `firmware/s3/`：S3 工程。默认是第 4 阶段音频全链路接收端（`main/audio_link.c` 收包解码进环形缓冲，`main/usb_prototype.c` 的音频任务从缓冲取数写 UAC2 端点）；`main/mic_led.c` 负责板载 RGB 灯（数据脚 GPIO48 的单线可寻址灯，开机发黑帧熄灭，按住 BOOT 时点亮，见 hardware-s3.md 第 7 节）；`MICCLICKER_STAGE1_SELFTEST=y` 时是 PSRAM / Wi-Fi / BOOT 自检，`--probe` 时是探针接收端。
- `firmware/c3/`：C3 工程。默认是第 4 阶段音频全链路发送端（`main/audio_task.c` 采集编码发送，`main/capture.c` 驱动 ES8311+I2S）；`MICCLICKER_ROLE_SELFTEST=y` 时是 Wi-Fi / ADC 自检，`--probe` 时是探针发送端。板级差异集中在 `main/board.h`。
- `firmware/components/wifi_scan/`：两端共享扫描组件，不记录 SSID/BSSID，不自动擦除 NVS。
- `firmware/components/g722/`：G.722 编解码组件（公有领域，非 LGPL），帧长固定 320 样本 / 160 字节。
- `firmware/components/link_proto/`：ESP-NOW 应用层帧编解码与序号跟踪，对应 `docs/protocol.md`。
- `firmware/components/link_radio/`、`link_bind/`：ESP-NOW 传输封装与配对绑定持久化。
- `firmware/components/link_probe/`：第 3 阶段两端探针的收发角色与阶段调度。
- `firmware/components/link_session/`：两端共享的产品侧链路 bring-up（读绑定、按存储信道起 radio、未绑定时才开配对窗口）。与探针的关键差别是**正常开机绝不清除绑定**。
- `firmware/components/audio_jitter/`：接收端 PCM 环形缓冲与时钟漂移补偿（有界、溢出丢最旧、欠载遮蔽、高/低水位自适应）。
- `firmware/components/key_scan/`：C3 侧 ADC 电阻梯窗口分类与去抖状态机（确认 40 ms / 保持 120 ms），主机侧可测。
- `firmware/components/key_tx/`：C3 侧按键可靠投递状态机（同 seq 重传、按序号匹配 ACK、被拒发送不消耗尝试、有界队列），主机侧可测。
- `firmware/components/key_map/`：按键映射表校验、NVS blob 编解码与 HID 报告编码（默认 UP=Right Alt、DOWN=保留、OK=Enter），主机侧可测。
- `firmware/components/key_store/`：S3 侧映射持久化（NVS 命名空间 `mickeys`，单 blob，非法表拒绝写入），仅真机验证。
- `firmware/tests/g722/`：G.722 主机侧验证，`./firmware/tests/g722/run.sh` 不需要 IDF 或硬件。
- `firmware/tests/link_proto/`：无线协议主机侧验证，`./firmware/tests/link_proto/run.sh` 不需要 IDF 或硬件。
- `firmware/tests/espnow/`：ESP-NOW 真机探针的判分脚本与实测记录，`check.sh <发送端日志> <接收端日志>`。
- `firmware/tests/audio_jitter/`：环形缓冲与漂移控制器的主机侧验证，`./firmware/tests/audio_jitter/run.sh` 不需要 IDF 或硬件。
- `firmware/tests/audio_link/`：第 4 阶段全链路真机验收的判分脚本与实测记录，`check.sh <C3 日志> <S3 日志> [期望秒数]`。
- `firmware/tests/key_scan/`、`key_tx/`、`key_map/`：按键三个纯逻辑组件的主机侧验证，`./firmware/tests/<名>/run.sh` 不需要 IDF 或硬件。
- `firmware/tests/key_link/`：第 5 阶段按键链路真机判分（`check.sh <C3 日志> <S3 日志> <观察器日志>` 与 macOS IOHIDManager 观察器 `key_observer`），实测与边界见该目录 README。
- `firmware/README.md`：锁定的依赖、烧录影响、实际命令和自检判据。
- `docs/ROADMAP.md`：实施进度与下一步排序（现状映射，不新增设计基线）。开工前先看它确定当前该做哪条线，再回到 `docs/DESIGN.md` 核对阶段判据。

ESP-IDF 固定为 v5.5.3；在仓库根目录使用 `./firmware/build.sh s3` 和 `./firmware/build.sh c3`（产品配置），探针配置加 `--probe`。这两条产品命令已于 2026-11-14 实际构建通过；同日加入 G.722 组件后再次构建通过，加入 ESP-NOW 组件后产品与探针两种配置均再次通过；2026-11-15 加入第 4 阶段音频全链路后两种产品配置均再次通过；同日加入第 5 阶段按键链路（key_scan/key_tx/key_map/key_store 及两端接线）后产品与探针两种配置均再次通过。新增源码目录、构建入口或测试命令时，同步更新本文件和 `firmware/README.md`。

首版不包含扬声器、多发送端、后台全局热键或 CDC。普通 HID 只作用于系统当前焦点，不得把快捷键描述为可保证送达后台 ASR 应用。

## 2. 设计基线不可静默变更

以下内容是实现约束，不是可随意调整的示例：

- 音频为 16 kHz、16-bit 有符号小端 PCM、单声道；每帧 20 ms，即 320 个采样。
- G.722 使用 64 kbit/s，编码后为 160 字节/20 ms；候选库必须验证接口、许可证和标准测试向量。
- ESP-NOW 按 v1.0 的 250 字节兼容上限设计，音频固定一帧一包，不经过 Wi-Fi 路由器。
- USB 为 UAC 麦克风 + HID 键盘 + 独立配置 HID；正式固件不增加 CDC/MIDI，调试日志使用独立 UART。
- 无线公共字段使用小端序。接收端必须校验版本、类型和长度，并丢弃不合法输入。
- 无线公共 `seq` 为 4 字节，各类型独立递增；ACK 负载回送被确认的按键事件序号，重传按键事件不得改变事件序号。
- 配对通过物理操作和限定时间窗完成；绑定信息持久化到 NVS，正常重连不得清除绑定。正式语音和按键流量使用加密单播，广播只用于无敏感数据的发现或调试。
- 快捷键映射保存在 S3 NVS。默认 UP 为 Right Alt、DOWN 保留、OK 为 Enter；非法映射不得写入。

需要改变上述基线时，先说明原因、兼容影响与验证方案，经用户确认后同步更新 `docs/DESIGN.md`、协议定义、测试和相关文档，避免只改一侧实现。

## 3. 硬件与依赖

- 不以开发板名称或常见接法代替原理图。AI Passport 的 BSP、麦克风接口、ADC 电阻梯阈值、LED/配对控件和供电方式，以及 S3 的 Flash/PSRAM、原生 USB 与引脚，都必须先确认再固化。AI Passport 的**原理图、PCB 源文件、硬件版本号与咪头型号已确认上游不公开**（[hardware-c3.md](docs/hardware-c3.md) 第 2 节），只剩开壳读丝印或向供应商索取两条路，不要当作可查资料再查一遍。
- 不得仅凭 GPIO0 推断 ADC 按键电路，也不得把 GPIO19/20 当作所有 S3 板卡的既定 USB 引脚。
- 将板级引脚、阈值和硬件差异集中在明确的配置层，避免散落到协议、音频和 USB 业务逻辑中。
- 锁定 ESP-IDF/Arduino、BSP、TinyUSB、编解码库及配置工具依赖的版本，并记录实际可复现的安装、构建和烧录步骤。
- 第三方库“支持某功能”不能代替原型验证；尤其是 Audio + 两类 HID 的组合描述符、端点和目标 OS 兼容性。
- LMK、PMK、个人配置及其他秘密不得写入仓库、示例配置或测试报告。

## 4. 固件实现要求

### 实时性与资源

- ESP-NOW、USB 和驱动回调只执行必要的投递工作，不做编解码、阻塞等待或耗时日志。
- 队列和音频缓冲必须有界。拥塞时丢弃最旧音频帧并计数，禁止无界积压；按键与 ACK 可以优先排队，但不能抢占正在发送的包。
- S3 使用有界环形缓冲；按序号去重和识别缺口，欠载输出静音，溢出丢弃最旧帧，并记录 PLC、欠载、溢出和队列高水位等统计。
- 必须处理 C3 采样时钟与 USB 主机时钟的长期漂移，不能把固定缓冲当作长期同步方案。
- 外部数据先校验再使用；显式处理整数宽度、字节序、长度和溢出，不通过直接映射内存结构体代替协议编解码。

### 按键与故障恢复

- C3 只对去抖后的状态变化生成按下/释放事件，不依赖电脑端自动重复。
- 音频不重传迟到帧；按键事件使用相同序号重传，直到 ACK 或达到重试上限，S3 按序号去重。
- S3 维护完整按键状态，全部释放时发送全零键盘报告。
- 掉线、链路超时、S3 重启、新会话和 USB 重置时必须执行 all-keys-up；修改映射前也先释放全部按键。
- 配对、解除绑定、恢复默认和映射修改都要考虑异常中断后的 NVS 一致性，不留下半更新状态。

### 配置工具

- 只通过独立 vendor-defined HID 与 S3 通信，不复用键盘报告，不引入常驻转发服务。
- 对设备返回的数据进行版本、长度、取值范围和状态校验；配置失败不得以界面成功状态掩盖。
- 普通 HID 与配置 HID 的行为和测试要分开描述，避免把配置通道当成全局热键通道。

## 5. 验证要求

按 `docs/DESIGN.md` 的阶段推进，不在底层能力未验证时直接做全链路联调：

1. 锁定硬件、引脚、ADC 阈值、BSP 和依赖版本。
2. 用已知 PCM/正弦波验证 S3 的 USB 音频、成对 HID 事件和配置 HID。
3. 独立验证 G.722 标准向量、协议编解码、丢包/乱序/重复、配对、心跳和无线统计。
4. 验证 C3 采集到 S3 USB 输出的完整音频链路。
5. 加入运行时映射、ACK、去重、状态显示和故障注入，验证并发与恢复。

与改动相匹配的测试应尽量自动化，至少覆盖适用的协议边界、环形缓冲、序号回绕、重试去重、映射校验和异常输入。没有运行测试时必须明确说明未运行及原因，不得把静态检查描述成硬件验证。

硬件操作与验收还需遵守：

- 烧录、复位、擦除或修改 NVS 前确认目标设备、串口和影响，不在设备身份不明时盲目操作。
- 每次记录硬件版本、串口、依赖版本、实际命令和结果；逐条检查退出码与日志，脚本执行完毕不等于探针或烧录成功。
- Windows、macOS、Linux 分别验证枚举、录音权限、设备选择、热插拔和休眠恢复；音频、键盘 HID 与配置 HID 必须同时工作。
- 首版验收保留至少 30 分钟连续录音、1000 次按键按下/释放、20 次强制断链及资源统计记录。
- 不预设传输距离、低延迟或 ASR 效果；报告实测条件和数据，并与同环境有线麦克风基线比较。

## 6. 文件、文档与提交前检查

- 一次性探针、验证脚本、截图、日志和报告放在仓库根目录 `.tmp/` 的独立子目录中，使用带时间戳或哈希的新路径，不覆盖既有取证文件，不放进业务源码目录。
- 可复用的产品代码、协议定义和必要测试才进入正式工程目录；临时验证工具不得冒充长期测试设施。
- 注释描述当前行为与约束，不写“原实现已删除”等修改考古，也不在注释中保存决策史；重要决策写入设计文档。
- 修改协议、固定参数、硬件配置或使用方式时，同步更新 `docs/DESIGN.md` 和相关说明。新增源码目录、构建入口或测试命令时，同步更新本文件。
- 交付前核对两端协议版本和字段定义一致，检查秘密与临时产物未混入交付物，并如实列出已验证项、未验证项和硬件依赖。
