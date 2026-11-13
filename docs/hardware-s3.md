# ESP32-S3 板卡实测记录（2026-11-13）

到货后第一轮芯片级体检的实测结果、命令与结论。本文只记录**实测过的事实**和**尚未验证的部分**，不替代原理图。

## 1. 结论

板子在芯片级工作正常：供电、晶振、Flash 读写通路、原生 USB 枚举、CH343 调试串口与自动复位烧录、MAC 烧录、出厂固件运行都已实测通过。

**未验证**（都需要先有固件，见第 6 节）：PSRAM 的实际读写、Wi-Fi/BLE 射频、USB 复合设备（UAC 麦克风 + HID 键盘 + 配置 HID）的枚举与端点。

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

环境：macOS 15.3.1 (arm64)；esptool 4.13.0（取自 `~/.espressif/python_env/idf5.5_py3.11_env`）；ESP-IDF v5.5.3 已安装于 `~/esp/esp-idf-v5.5.3`，但**只有 RISC-V 工具链（C3 用），S3 需要的 Xtensa 工具链尚未安装**。

本轮实际执行的命令（可直接复跑）：

```bash
PORT=/dev/cu.usbmodemXXXX   # 优先用 COM 口（CH343）的节点：可自动复位
esptool --chip esp32s3 --port $PORT --baud 921600 --before default_reset --after no_reset chip_id
esptool --chip esp32s3 --port $PORT --baud 921600 --before no_reset --after no_reset flash_id
esptool --chip esp32s3 --port $PORT --baud 921600 --before no_reset --after no_reset read_mac
esptool --chip esp32s3 --port $PORT --baud 921600 --before default_reset --after no_reset \
  read_flash 0x8000 0x1000 partition-table.bin      # 分区表
```

未验证项及其前置条件：

| 未验证 | 为什么现在验不了 | 验证办法 |
| :--- | :--- | :--- |
| PSRAM 是否真好用、容量与线宽 | eFuse 只声明 8 MB，ROM 无读写测试命令 | 烧自检固件：按 octal 配置初始化，全片写读校验 |
| Wi-Fi / BLE 射频与天线 | 需要驱动起来才能扫描 | 自检固件里做一次 Wi-Fi 扫描，看能否扫到周围 AP |
| USB 复合设备（UAC + HID×2） | 需要自写描述符与端点 | DESIGN.md 第 2 阶段 USB 原型 |
| 供电余量、发热 | 需带负载长时间观察 | 全链路跑起来后按第 7 节验收基线测 |

## 7. 已知硬件限制与操作注意

1. **调试 UART 用 COM 口，不必外接 USB-TTL。** DESIGN.md 5.3 要求「正式固件不增加 CDC，调试输出使用独立的 UART 方案」，板上 CH343 桥（COM 口）正好承担这条通道。注意正式固件要占用的是芯片 UART0（默认 GPIO43/44），引脚是否从排针引出需对着实物/原理图确认；桥只解决「电脑侧怎么接」，不解决「芯片侧引到哪」。
2. **USB-Serial/JTAG 连续大批量读取约 6 MB 后会挂死，CH343 不会。** 走原生 USB 口时两轮实测分别在 6.09 MB / 6.08 MB 处停住（进程不退、日志不再增长），带一次复位重连即可恢复；走 COM 口单次连续读取 7 MB 用时 91.3 s 正常完成。说明这是 USB-Serial/JTAG 通道的特性，不是 Flash 故障。**大镜像烧录和整片备份走 COM 口更稳**。
3. **读取/烧录速度以 COM 口为基准**（CH343 受 UART 线速限制）：1 MB 读取在 460800 baud 下约 24 s，921600 baud 下约 13 s。再高的波特率未测。
4. **进下载模式分两种情况**（原因见第 3 节）：走 COM 口可自动复位，无需按键；走原生 USB 口必须手动 BOOT + RST。自动化脚本应以 COM 口为首选，并把失败口径与手动步骤写清楚。
5. `nvs` 目前是空白分区，首次烧写后才会出现配对/映射数据；恢复出厂时擦除该分区即可。

## 8. 下一步

1. 安装 Xtensa 工具链（`idf_tools.py install --targets=esp32s3`，约 250 MB，写入 `~/.espressif`），解锁 S3 固件编译。
2. 写一支自检固件，补齐第 6 节的 PSRAM / 射频两项未验证项，并把结果回填本文。
3. 进入 DESIGN.md 第 6 节第 2 阶段：USB 复合设备原型。
