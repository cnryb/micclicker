# MicClicker 固件工程

当前目录包含第 1 阶段的可重复硬件自检和第 2 阶段的 USB 原型，不是完整产品固件；尚未实现 ESP-NOW、G.722 或 C3 音频采集。USB 原型以 macOS 为首个验证目标，Windows / Linux、热插拔和休眠恢复仍须单独验证。

## 目录

- `s3/`：ESP32-S3-N16R8 固件。默认构建 UAC2 麦克风 + 标准键盘 HID + vendor 配置 HID 原型；`MICCLICKER_STAGE1_SELFTEST=y` 时改为构建 PSRAM / Wi-Fi / BOOT 自检。音频为 16 kHz、16-bit 小端 PCM、单声道、1 kHz 已知正弦波。
- `c3/`：AI Passport / ESP32-C3 自检。执行 Wi-Fi 扫描后，依次采集 RELEASED、UP、DOWN、OK 四个键态的 ADC 数据，输出原始值、噪声、估算/校准电压、窗口裕量和误分类数。
- `components/wifi_scan/`：两端共享的最小 Wi-Fi 扫描组件，不记录 SSID/BSSID，不自动擦除 NVS。
- `components/g722/`：G.722 64 kbit/s 编解码组件，内嵌 WebRTC/SpanDSP 的公有领域实现，帧长固定为 320 样本 / 160 字节。
- `tests/g722/`：G.722 主机侧验证（封装契约 + 黄金向量逐比特比对），不依赖 ESP-IDF 或硬件，详见 `tests/g722/README.md`。
- `build.sh`：锁定 ESP-IDF v5.5.3 的构建入口。

板级引脚和 ADC 阈值集中在 `c3/main/board.h`。S3 的 Flash、PSRAM、控制台及分区配置在 `s3/sdkconfig.defaults` 和 `s3/partitions.csv`，不预设未经确认的原生 USB 引脚。

## 已验证的构建基线

- macOS 15.3.1 arm64
- ESP-IDF v5.5.3：`~/esp/esp-idf-v5.5.3`，commit `2c211b236707889e8400c4dc5644dd5c4ee071e0`
- Python 3.13 环境：`~/.espressif/python_env/idf5.5_py3.13_env`
- esptool 4.12.0
- `xtensa-esp-elf` / `riscv32-esp-elf` 14.2.0_20251107

无 Arduino、外部 BSP 二进制组件。**G.722 不使用外部库**：`components/g722/` 内嵌 WebRTC `third_party/g722`（SpanDSP，公有领域），只改过 include 路径，来源与许可证见该目录下的 `README.upstream.txt` 和 `LICENSE-upstream.txt`。S3 的 USB 依赖由 `s3/main/idf_component.yml` 和 `s3/dependencies.lock` 锁定：`espressif/esp_tinyusb 2.0.0`、`espressif/tinyusb 0.21.0~6`。AI Passport 引脚事实来自 MIT 许可的上游 FoloToy BSP，版本和证据边界见 `docs/hardware-c3.md`。

## 构建

在仓库根目录执行：

```bash
./firmware/build.sh s3   # 默认：USB 复合设备原型
./firmware/build.sh c3   # C3 自检（Wi-Fi + 四键态 ADC + G.722）
./firmware/tests/g722/run.sh   # G.722 主机侧验证，不需要 IDF 或硬件
```

复跑 S3 第 1 阶段自检时，先在 `firmware/s3/sdkconfig.defaults` 中加入 `CONFIG_MICCLICKER_STAGE1_SELFTEST=y`，再执行 `./firmware/build.sh s3`；恢复 USB 原型时移除该设置。构建脚本每次都会重新生成 `sdkconfig`，因此切换会完整生效。

脚本默认使用上述本机路径，也接受预先设置的 `IDF_PATH` 和 `IDF_PYTHON_ENV_PATH`，但 IDF 版本、提交和编译器版本必须与上表一致。每次构建都会删除可再生的 `firmware/<目标>/sdkconfig` 并从 `sdkconfig.defaults` 重新生成，避免旧选择静默残留；本地 menuconfig 改动不会被该入口保留。构建产物分别位于 `firmware/s3/build/` 和 `firmware/c3/build/`，均被 Git 忽略。

## 烧录前检查

烧录会替换设备原有应用；C3 上原有的 BLE 固件将不能继续运行，直到恢复备份。已有整片备份分别记录在 `docs/hardware-s3.md` 和 `docs/hardware-c3.md`。

1. 用数据线连接目标设备。S3 优先使用 CH343 的 `COM` 口；C3 使用原生 USB-C，并先按电源键开机。
2. 根据 `system_profiler SPUSBDataType` 和下面的只读命令核对芯片及串口，不能仅凭 `/dev/cu.usbmodem11301` 这个可复用节点名判断设备：

   ```bash
   export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.13_env"
   source ~/esp/esp-idf-v5.5.3/export.sh
   esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --before default_reset --after no_reset chip_id
   esptool --chip esp32c3 --port /dev/cu.usbmodemXXXX --before default_reset --after no_reset chip_id
   ```

   `--chip` 必须与实机一致；型号不符时停止，不要尝试烧录。
3. 只写构建输出的 bootloader、分区表和 factory 应用。`idf.py flash` 的 `flash_args` 已核对为 `0x0`、`0x8000`、`0x10000` 三段，不写 NVS 或 `phy_init`，也不要从 `0x0` 写入 merged-bin。
4. 自检组件遇到 NVS 满页或版本不兼容会失败退出，不会像 IDF 扫描示例那样主动擦除整片 NVS。Wi-Fi 初始化仍可能按驱动规则更新 `phy` 射频校准；2026-11-14 的 C3 实测已出现 `Saving new calibration data` 日志，烧录前必须有整片备份，不能把该操作描述为 NVS 完全只读。

确认设备、端口和影响后：

```bash
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.13_env"
source ~/esp/esp-idf-v5.5.3/export.sh
PORT=/dev/cu.usbmodemXXXX
idf.py -C firmware/s3 -p "$PORT" -b 460800 flash
idf.py -C firmware/s3 -p "$PORT" monitor

# C3 使用对应工程；两个操作不要对同一个端口混用目标：
idf.py -C firmware/c3 -p "$PORT" -b 460800 flash
idf.py -C firmware/c3 -p "$PORT" monitor
```

监视器退出键为 `Ctrl+]`。一次性烧录和监视日志放在 `.tmp/` 的新时间戳目录，不纳入版本库。

## 自检判据

S3 必须同时出现以下标记才算通过；只有 `idf.py flash` 成功不算：

```text
PSRAM_TEST: PASS ...
WIFI_SCAN: PASS ...
S3_BOOT: PASS hold_ms=...
SELFTEST: PASS psram=1 wifi=1 boot=1
```

PSRAM 测试覆盖启动后最大连续空闲堆块，并要求至少达到物理容量的 90%；其他堆块、碎片、保留区或已占用区域不计入这两轮覆盖。启动阶段的 IDF `SPIRAM_MEMTEST` 也只是补充抽样检查，不能合起来解释为物理全片逐字节验证。Wi-Fi 扫描结束后出现 `S3_BOOT: WAITING` 时，按住 S3 的 BOOT 键 3 秒；只在应用运行态测试 GPIO0，复位时不要按 BOOT。BOOT 等待最多 30 秒，超时或前置测试失败会输出最终 FAIL，不会无限等待。

C3 启动后先跑 G.722 自检（无需交互），要求 `G722_SELFTEST: PASS bitexact=1 decode=1`，其中 `us_per_frame` 必须低于 10000（20 ms 帧预算的一半）。2026-11-14 实测 C3 为 3263 µs。随后是四个 ADC 阶段。

C3 必须依次完成四个阶段。每个阶段等待目标键态最多 60 秒，连续检测到 3 次后才开始以 10 ms 间隔采样 400 次；看到阶段提示后按住对应键，直到阶段结果出现。超时会输出等待期间的原始读数诊断和最终 FAIL：

1. RELEASED：全部松开。
2. UP：按住 UP，直到该阶段结果出现。
3. DOWN：按住 DOWN，直到该阶段结果出现。
4. OK：按住 OK，直到该阶段结果出现。

最终需要每个 `ADC_PHASE: PASS` 和 `SELFTEST: PASS wifi=1 adc=1 adc_calibrated=1 g722=1`。若校准不可用，原始 ADC 数据仍有效，但 mV 仅是 12 位满量程近似值，最终只输出 `SELFTEST: PASS_LIMITED`，表示当前近似换算和窗口匹配，不能解释为电气电压精度验证通过。

## USB 复合设备原型

默认 S3 构建使用 `303a:4014`，四个接口依次为 UAC2 Audio Control、Audio Streaming、标准键盘 HID、vendor 配置 HID；没有 CDC/MIDI。音频为 UAC2、16 kHz、16-bit、单声道、1 kHz 正弦波。音频等步 IN 为 `0x81`，键盘 IN 为 `0x82`，配置 HID 为 OUT `0x03` / IN `0x83`。

vendor 报告使用 **Report ID 0 + 64 字节 payload**，不要换成编号报告，否则会增加一个字节并与端点缓冲冲突。当前命令：

| 命令 | 含义 |
| :--- | :--- |
| `0x01` | ping，递增计数 |
| `0x02` | Left Alt 按下，随后释放 |
| `0x03` | Enter 按下，随后释放 |
| `0x04` | 全部按键释放 |

输出命令经 HID Output/SET_REPORT 进入有界队列；状态经 GET_REPORT Input 读取，不能套用 Feature API。状态第 0 字节为版本，随后是状态标志、最后命令和结果。计数偏移：ping 4、Alt 8、Enter 12、all-up 16、拒绝 20、音频生产 24、短写 28、实际 USB 包 32、实际 USB 字节 36、空包 40、alt 变化 44，均为小端 `uint32_t`。非法命令只增加拒绝计数并返回错误，不改变按键状态；释放发送失败会保留 pending 并重试。

### macOS 首轮实测（2026-11-14）

- `system_profiler` 与 AVFoundation 均识别复合设备和 `MicClicker Microphone`。
- 配置 ping 成功；Left Alt 和 Enter 各记录到一次按下/释放，最后 all-keys-up 成功。原始主机日志：`.tmp/usb-host-prototype-20261114/host-test.log`。
- 10 秒录音：160000 样本，主频 **999.95 Hz**、RMS **8484.58**、最大连续零样本 1。证据：`.tmp/usb-prototype-20261114-171405/mic-10s.wav` 和 `analysis.log`。
- 录音中发送 Left Alt 并查询配置：6 秒并发录音主频 **1000.00 Hz**、RMS **8484.58**，Alt 成对、ping 成功。证据：`.tmp/usb-prototype-20261114-171850/mic-concurrent.wav`、`analysis.log` 和 `.tmp/usb-host-prototype-20261114/concurrent-alt.log`。
- alt 0 下 `produced=0`、`usb_packets=0`，不再持续生产；实际发送计数来自 `tud_audio_tx_done_isr`，短写单独统计。TinyUSB 的 EP FIFO 可覆盖旧数据，短写不等于溢出计数，不能把两者混为一谈。

上述主机探针位于 `.tmp/usb-host-prototype-20261114/`，是一次性 macOS 取证工具，不是跨平台产品配置工具。Windows / Linux、热插拔、休眠恢复、至少 30 分钟连续录音和异常采样率 SET_CUR 的驱动层前置校验仍未完成；UAC1 是否需要在 Windows 目标版本上采用也留待后续决定。
