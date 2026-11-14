# ESP32-S3 板卡实测记录（2026-11-14 更新）

到货后第一轮芯片级体检的实测结果、命令与结论。本文只记录**实测过的事实**和**尚未验证的部分**，不替代原理图。

## 1. 结论

板子在芯片级工作正常：供电、晶振、Flash 读写通路、原生 USB 枚举、CH343 调试串口与自动复位烧录、MAC 烧录、出厂固件运行都已实测通过。

2026-11-14 自检固件进一步通过：8 MB octal PSRAM 初始化及两轮写读、Wi-Fi STA 扫描、运行态 GPIO0 / BOOT 长按 3 秒。结果与边界见第 6 节。

同日 **macOS USB 复合设备原型通过**：UAC2 麦克风、标准键盘 HID 和独立 vendor 配置 HID 已并发完成录音、成对 Left Alt / Enter 与配置通信，见第 6.2 节。

**仍未验证**：Windows / Linux、热插拔与休眠恢复、长时带负载供电余量与发热。BLE 不在首版范围内，未做专项验证。

## 2. 设备身份

| 项目 | 实测值 | 来源 |
| :--- | :--- | :--- |
| 板卡丝印 | `ESP32-S3-N16R8` | 用户目视（非 Espressif 官方 WROOM 模组丝印，属第三方模组/板卡） |
| 芯片 | ESP32-S3 (QFN56)，revision **v0.2** | `chip_id` |
| 封装内 PSRAM | **8 MB**，esptool 报告 `Embedded PSRAM 8MB (AP_3v3)` | `chip_id`（eFuse 声明，未做读写校验） |
| 晶振 | 40 MHz | `chip_id` |
| Flash | **16 MB**，厂商 ID `0x68`，device `0x4018`，四线（quad），eFuse 定压 3.3 V | `flash_id` |
| MAC | `c4:9e:7e:3c:d5:d4`（已出厂烧录，USB-Serial/JTAG 的序列号即派生自此） | `chip_id` / `read_mac` |
| USB（丝印 `USB` 口） | 原生 USB，Full Speed 12 Mb/s；应用态与 ROM 下载态枚举成不同设备，见第 3 节 | `system_profiler SPUSBDataType` |
| COM（丝印 `COM` 口） | UART 桥 **CH343**，VID:PID `1a86:55d3`（「USB Single Serial」），节点 `/dev/cu.usbmodem<芯片序列号>`；macOS 内置驱动，不需装厂商驱动 | 同上 + `/dev` 节点 |

丝印的 N16R8 与实测（16 MB Flash + 8 MB PSRAM）一致。注意 `N16R8` 里的 R8 是 **octal** PSRAM，后续 IDF 配置须按八线 PSRAM 设置，不能沿用 quad 默认值。

两个 Type-C 口的分工：

| 口 | 用途 | 供电 |
| :--- | :--- | :--- |
| `USB` | 设备功能本身（UAC 麦克风 + HID），也就是交付形态要用的口 | 可单独供电 |
| `COM` | 烧录、UART0 控制台日志、自动复位 | 可单独供电 |

两口同时插通常安全（各自 VBUS 经防倒灌器件汇入 5 V 轨，同一台主机供电电压相同），开发期建议都插：USB 口跑设备功能，COM 口烧录看日志。只插 COM 口也能烧录和复位，但此时原生 USB 不枚举。**线材必须是数据线**——本轮实测踩过：一根只有 VBUS 的充电线插 COM 口时，USB 层完全看不到 CH343。

## 3. USB 枚举的三种身份

板上有两套独立的 USB 通路，枚举出的身份可用于判断芯片当前状态：

| 状态 | VID:PID | 序列号 | 串口节点 | 说明 |
| :--- | :--- | :--- | :--- | :--- |
| 正常运行（跑应用固件） | `303a:4001` | `123456` | `/dev/cu.usbmodem1234561` | 应用自己实现的 TinyUSB CDC，**不是** ROM 下载口，esptool 连不上 |
| 下载模式（ROM） | `303a:1001` | MAC 派生 | `/dev/cu.usbmodem11301` | USB-Serial/JTAG，esptool 可正常连接 |
| COM 口（与上面两种无关） | `1a86:55d3` | `5D4D044996` | `/dev/cu.usbmodem5D4D0449961` | CH343 桥，只要 COM 口供电就在，与芯片跑什么无关 |

烧录通道的选择：

- **走 COM 口（推荐）**：esptool 用 DTR/RTS 能自动把芯片拉进下载模式，`--before default_reset` 直接可用，不需要按键。本轮实测通过。
- **走原生 USB 口**：出厂固件是 CDC 应用，esptool 的自动复位无法把它推进 ROM 下载态，**首次烧录必须手动按键**：按住 BOOT → 点一下 RST（或 EN）→ 松开 BOOT。等自家固件占用了 USB-OTG 后，USB-Serial/JTAG 与它争用同一个 PHY，这条路上仍然只能手动进下载态。
- 长传输稳定性也以 COM 口更好，见第 7 节。

## 4. 出厂固件

| 项目 | 实测值 |
| :--- | :--- |
| 框架 | ESP-IDF **v4.4.1**，构建时间 `Jun 18 2022 15:17:49`，`project_name` / `version` 为空 |
| 判断 | 与 Arduino-ESP32 2.0.x（内核 IDF 4.4.1）的编译特征一致，应为一支厂商自带的 RGB 灯演示 sketch |
| 运行表现 | USB CDC 上循环打印 `50%B` / `50%R` / `50%G`，对应板载 RGB 灯按蓝→红→绿、50% 亮度循环 |
| 分区表 | `nvs` 24 KB @0x009000 · `phy_init` 4 KB @0x00F000 · `factory` app 1984 KB @0x010000 · `vfs` data 6144 KB @0x020000 |
| NVS | 整片 `0xFF`，从未写入 → 板子是全新的，没有历史配对/配置残留 |

结论：这只是到货验证用的演示，不是本项目固件，后续会被覆盖；覆盖前已做备份（第 5 节）。

## 5. 备份产物

整片 16 MB 里实际被分区占用的只有 0x0 ~ 0x800000（8 MB），备份按此范围取，未含 8–16 MB 的空白区。

备份属于一次性产物，不进仓库，因此本文只记录校验值、不记录存放路径。要长期保留，应转存到仓库之外的位置；换机或清理后这份校验值就没有对应文件了。

| 项目 | 值 |
| :--- | :--- |
| 大小 | 8 388 608 B（0x0 ~ 0x800000） |
| sha256 | `8b355b094fa3829e13eee64fb0e092d02c1cad10cbebd1e360149c6f3ff009f3` |
| 内容 | bootloader、分区表、`factory` 应用、`vfs` 数据区、`nvs`（实测为空白） |

恢复厂商演示（`<备份文件>` 换成实际路径）：

```bash
esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 460800 \
  --before default_reset --after hard_reset \
  write_flash 0x0 <备份文件>
```

## 6. 验证方式与未验证项

环境：macOS 15.3.1 (arm64)；ESP-IDF v5.5.3 安装在 `~/esp/esp-idf-v5.5.3`；python 环境只剩 `~/.espressif/python_env/idf5.5_py3.13_env`（自带 `esptool.py` 4.12.0）。

**Xtensa 工具链已于 2026-11-13 装好**：`~/.espressif/tools` 下现有 `xtensa-esp-elf 14.2.0_20251107`、`xtensa-esp-elf-gdb 16.3_20250913`、`esp32ulp-elf 2.38`，与原有 RISC-V 工具链并存。直连 GitHub 只有几百 KB/s（几分钟才下到 8%），改用 Espressif 国内镜像后一次装完：

```bash
IDF_PATH="$HOME/esp/esp-idf-v5.5.3" IDF_TOOLS_PATH="$HOME/.espressif" \
IDF_GITHUB_ASSETS="dl.espressif.cn/github_assets" \
"$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python" \
  "$HOME/esp/esp-idf-v5.5.3/tools/idf_tools.py" install --targets=esp32s3
```

`idf.py` 还不能直接调：`export.sh` 会挑到 `/usr/bin/python3`（3.9.6），报 `idf5.5_py3.9_env` 不存在。先指定 venv 再导出即可（已实测输出 `ESP-IDF v5.5.3`）：

```bash
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.13_env"
source ~/esp/esp-idf-v5.5.3/export.sh
```

本项目现已有可复现构建入口 `./firmware/build.sh s3`。IDF 锁定为 v5.5.3、commit `2c211b236707889e8400c4dc5644dd5c4ee071e0`；脚本同时检查编译器 `14.2.0_20251107`，并每次从 `sdkconfig.defaults` 重新生成配置，避免旧选择残留。

本轮实际执行的命令（可直接复跑）：

```bash
PORT=/dev/cu.usbmodemXXXX   # 优先用 COM 口（CH343）的节点：可自动复位
esptool --chip esp32s3 --port $PORT --baud 921600 --before default_reset --after no_reset chip_id
esptool --chip esp32s3 --port $PORT --baud 921600 --before no_reset --after no_reset flash_id
esptool --chip esp32s3 --port $PORT --baud 921600 --before no_reset --after no_reset read_mac
esptool --chip esp32s3 --port $PORT --baud 921600 --before default_reset --after no_reset \
  read_flash 0x8000 0x1000 partition-table.bin      # 分区表
```

### 6.1 MicClicker 自检固件实测（2026-11-14）

目标设备再次核对为 ESP32-S3 rev v0.2、MAC `c4:9e:7e:3c:d5:d4`，串口为 CH343 `/dev/cu.usbmodem5D4D0449961`。完整烧录只写 `0x0` bootloader、`0x8000` 分区表、`0x10000` factory；后续探针更新只用 `app-flash` 写 factory，没有整片擦除或 merged-bin。

```bash
./firmware/build.sh s3
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.13_env"
source ~/esp/esp-idf-v5.5.3/export.sh
idf.py -C firmware/s3 -p /dev/cu.usbmodem5D4D0449961 -b 460800 flash
idf.py -C firmware/s3 -p /dev/cu.usbmodem5D4D0449961 monitor -b 115200
```

| 项目 | 实测结果 | 证据与边界 |
| :--- | :--- | :--- |
| PSRAM 初始化 | **通过**：AP 厂商、64 Mbit、8 MB、octal 80 MHz；IDF 启动 memtest OK | 启动日志与 `esp_psram_get_size()` 一致 |
| PSRAM 应用写读 | **通过**：物理/堆容量均 8 388 608 B，最大连续空闲块 **8 257 536 B**，两种地址相关种子各完整写入后读回，堆前后完整 | 不是物理全 8 MiB 每字节覆盖；其他堆块、碎片和保留区不在这两轮范围内 |
| Wi-Fi 射频 | **通过**：首轮扫描 15 AP，最强 RSSI −43 dBm；后续多次扫描也成功 | 仅证明 STA 扫描与天线基本可用，不是 ESP-NOW、吞吐或距离验证 |
| 调试 UART | **通过**：启动日志明确 `GPIO 44 and 43 are used as console UART I/O pins`，CH343 持续可读 | 电脑到芯片 UART0 的通路已确认 |
| BOOT 运行态输入 | **通过**：独立复测中 GPIO0 从 1 变 0，连续 3 秒为 0，`S3_BOOT: PASS hold_ms=3000` | 只由工具复位，用户仅长按 BOOT；**没有执行 BOOT+RST 下载模式交叉验证**，不列为已验项 |

首轮 PSRAM / Wi-Fi 日志：`.tmp/s3-selftest-20261114-151506/monitor.log`。BOOT 独立复测日志：`.tmp/s3-selftest-20261114-160215/monitor.log`。最终工程修正版另于 `.tmp/s3-selftest-20261114-160647/build.log` 构建通过；修正内容是无输入超时和构建配置复用，不改变上述已测电平或容量结论。

仍未验证：

| 项 | 下一步 |
| :--- | :--- |
| 其他 OS、热插拔、休眠恢复 | macOS 首轮已通过，见第 6.2 节；继续补 Windows / Linux 与恢复测试 |
| 供电余量、发热 | 全链路运行后按第 7 节验收基线做带负载长测 |
| BLE | 首版不使用，不列为当前阻塞项 |

### 6.2 macOS USB 复合设备原型（2026-11-14）

依赖锁定为 `espressif/esp_tinyusb 2.0.0` + `espressif/tinyusb 0.21.0~6`，见 `firmware/s3/dependencies.lock`。原型使用 `303a:4014`，序列号由 S3 MAC 生成；该 PID 仅用于当前原型，正式产品标识仍需另行确认。

| 功能 | 实现与实测 |
| :--- | :--- |
| UAC2 麦克风 | 16 kHz、16-bit 小端 PCM、单声道、1 kHz 已知正弦波；等步 IN `0x81`，最大包 34 B，应用每毫秒写 32 B，FIFO 272 B |
| 标准键盘 HID | 独立 instance 0 / USB 接口 2，中断 IN `0x82`；Left Alt 使用 modifier，Enter 使用 `0x28`，按下/释放成对，all-keys-up 遇忙保留 pending 并重试 |
| vendor 配置 HID | 独立 instance 1 / USB 接口 3，Report ID 0、64 B payload，OUT `0x03` / IN `0x83`；配置命令有界入队，GET_REPORT 返回版本、状态和计数，非法命令返回错误 |
| 调试日志 | 继续使用独立 CH343 UART，不增加 CDC/MIDI |

实测结果：

1. `system_profiler SPUSBDataType` 显示 `303a:4014`、Full Speed 和预期序列号；AVFoundation 列出 `MicClicker Microphone`。
2. 配置 ping 的版本、计数和结果校验通过。Left Alt 与 Enter 各观察到一次真实按下/释放，all-keys-up 成功；主机日志为 `.tmp/usb-host-prototype-20261114/host-test.log`。
3. 10 秒录音得到 160000 个 16-bit/mono/16 kHz 样本，主频 **999.95 Hz**、RMS **8484.58**、最大连续零样本 1，见 `.tmp/usb-prototype-20261114-171405/mic-10s.wav` 与 `analysis.log`。
4. 录音期间发送 Left Alt 并执行配置 ping：6 秒并发录音主频 **1000.00 Hz**、RMS **8484.58**，Alt 成对、配置成功，见 `.tmp/usb-prototype-20261114-171850/mic-concurrent.wav`、`analysis.log` 和 `.tmp/usb-host-prototype-20261114/concurrent-alt.log`。
5. alt 0 下生产和实际 USB 包计数均为 0，不再后台写 FIFO；实际发送量由 `tud_audio_tx_done_isr` 轻量计数。短写单独记录，但 TinyUSB EP FIFO 可覆盖旧数据，**短写不等于溢出或丢帧计数**，后续环形缓冲仍需单独统计。

本轮完成的是 macOS 首轮原型，不代表所有平台验收完成。Windows / Linux、热插拔、休眠恢复、至少 30 分钟连续录音、功耗与异常主机行为仍待验证。固定 Clock Source 下合规主机不会 SET_CUR 采样率；若需防御异常 SET_CUR，后续应在驱动层前置校验，不能只依赖应用回调拒绝。是否改用 UAC1 由目标 Windows 版本决定。

## 7. 已知硬件限制与操作注意

1. **调试 UART 用 COM 口，不必外接 USB-TTL。** DESIGN.md 5.3 要求「正式固件不增加 CDC，调试输出使用独立的 UART 方案」，板上 CH343 桥（COM 口）正好承担这条通道。2026-11-14 自检已通过实际启动日志确认 GPIO43/44 到 CH343 的通路；其他排针是否另行引出不影响该结论。
2. **USB-Serial/JTAG 连续大批量读取约 6 MB 后会挂死，CH343 不会。** 走原生 USB 口时两轮实测分别在 6.09 MB / 6.08 MB 处停住（进程不退、日志不再增长），带一次复位重连即可恢复；走 COM 口单次连续读取 7 MB 用时 91.3 s 正常完成。说明这是 USB-Serial/JTAG 通道的特性，不是 Flash 故障。**大镜像烧录和整片备份走 COM 口更稳**。
3. **读取/烧录速度以 COM 口为基准**（CH343 受 UART 线速限制）：1 MB 读取在 460800 baud 下约 24 s，921600 baud 下约 13 s。再高的波特率未测。
4. **进下载模式分两种情况**（原因见第 3 节）：走 COM 口可自动复位，无需按键；走原生 USB 口必须手动 BOOT + RST。自动化脚本应以 COM 口为首选，并把失败口径与手动步骤写清楚。
5. `nvs` 目前是空白分区，首次烧写后才会出现配对/映射数据；恢复出厂时擦除该分区即可。

## 8. 下一步

1. ~~安装 Xtensa 工具链~~ **已完成（2026-11-13）**，版本、安装命令与 `idf.py` 的环境变量坑见第 6 节。
2. ~~写自检固件补齐 PSRAM / 射频~~ **已完成（2026-11-14）**，运行态 BOOT 输入也已通过，见第 6.1 节。
3. ~~进入 DESIGN.md 第 2 阶段 USB 原型~~ **macOS 首轮已通过（2026-11-14）**，见第 6.2 节。
4. **下一步**：进入第 3 阶段 G.722 与 ESP-NOW 分测；另按平台补做 USB 热插拔、休眠和长时验收。
