# G.722 编解码验证

第 3 阶段（`docs/DESIGN.md` 第 6 节）要求「独立验证 G.722 标准向量」。本目录是这个验证的入口，覆盖**协议层面**的编解码正确性；ESP-NOW 无线分测是独立的另一条线。

## 结论与边界（先说不能证明什么）

| 项目 | 状态 |
| :--- | :--- |
| 与 ITU-T 官方测试向量的逐比特比对 | **未完成** |
| 与 ffmpeg（独立 ITU 派生实现）的逐比特一致 | **已通过**（主机 + 真实 C3） |
| 64 kbit/s、160 B/20 ms 帧格式 | **已通过** |
| 编解码往返音质（1 kHz / 300 Hz / 4 kHz） | **已通过** |
| C3 单帧编码耗时 | **实测 3.26 ms / 20 ms 预算**（16%） |

**ITU 官方向量未验证。** ITU-T 的 `G722p2_configuration-*.zip` 测试向量现在需要 ITU 账号登录才能下载，SpanDSP 等开源仓库本身也不含这些文件。因此本轮用 **ffmpeg 生成的黄金向量**作为互操作基线：ffmpeg 的 G.722 同样源自 ITU 参考实现，逐比特比对能发现位率/选项错误、定点算术偏差和工具链差异，但它**不等于**通过了 ITU 一致性判定。拿到官方向量后的接入方式见文末。

## 实现选择

`firmware/components/g722/` 内嵌 **WebRTC `third_party/g722`**，即 SpanDSP 的 G.722 实现：

- **许可证**：SpanDSP 作者明确将该文件放入 **公有领域**（public domain），见 `LICENSE-upstream.txt`。这比 ffmpeg / SpanDSP 主干的 LGPL 2.1 更适合不公开全部源码的产品固件。
- **来源**：<https://webrtc.googlesource.com/src/+/main/modules/third_party/g722/>，`README.chromium` 记录版本与更新方式。
- **改动**：只改了 `#include` 路径（`modules/third_party/g722/g722_enc_dec.h` → `g722_enc_dec.h`），代码主体与上游逐字节相同。
- **依赖**：纯 C99 + `stdint.h`，无动态内存、无浮点、无外部依赖，可在 C3/S3 上直接跑。

`mic_g722.c` 在其上加了一层帧封装，把 320 样本 ↔ 160 字节钉死：半帧或超长输入会被拒绝，而不是静默补齐，这样协议层的长度校验不会被编码器悄悄破坏。

### 一个易错点

codec 的 init 第二参数是**比特率**（48000/56000/64000），不是采样率。传 16000 不会报错，而是落进 `else` 分支静默取 8 bit —— 也就是 64 kbit/s 但语义错误。封装层固定传 `64000` 并注释了原因。

## 怎么跑

```bash
./firmware/tests/g722/run.sh              # 封装契约 + 黄金向量
./firmware/tests/g722/run.sh --itu DIR    # 附带上报 ITU 向量状态
```

主机侧测试不需要 ESP-IDF、不需要硬件。`run.sh` 会：

1. 用主机 `cc` 编译**固件同一份** `firmware/components/g722/src/*.c`；
2. 校验 `vectors/SHA256SUMS`，防止黄金向量被静默改动；
3. 跑封装契约（初始化、空指针、未初始化拒绝、帧尺寸、re-init 清状态）；
4. 跑三个黄金向量的逐比特比对与解码音质。

`hostshim/esp_err.h` 只是让 `mic_g722.c` 在没有 IDF 的主机上编译通过，固件构建解析的是真正的 IDF 头文件。

## 设备端自检

C3 自检固件在 `app_main` 里先跑 `micclicker_g722_selftest_run()`，不需要按键交互：

```text
G722_SELFTEST: ENCODE_BITEXACT bytes=160
G722_SELFTEST: DECODE_OK peak=12349
G722_SELFTEST: TIMING frames=100 us_per_frame=3263 budget_us=20000
G722_SELFTEST: PASS bitexact=1 decode=1 us_per_frame=3263
```

2026-11-14 实测（C3 rev v1.1，MAC `98:c3:77:f6:c6:44`，串口 `/dev/cu.usbmodem11301`）：三项全通过。`us_per_frame=3263` 表示 100 帧平均 3.26 ms，占 20 ms 帧预算的 16%，给采集、发送和其余任务留了余量。

`SELFTEST: PASS` 现在要求 `g722=1`。日志证据：`.tmp/g722-c3-20261114-175307/`。

## 黄金向量

`vectors/` 下每个音调一组 `.pcm`（16 kHz、16-bit 小端、单声道、3200 样本 = 10 帧）与 `.g722`（1600 B 参考码流），由 `make_vectors.sh` 用 ffmpeg 生成，`SHA256SUMS` 固定。

**只有刻意更换向量集时才重跑 `make_vectors.sh`**，并把新文件与更新后的 `SHA256SUMS` 一并提交；否则等于把失败改成通过。

## 续做 ITU 官方向量的方法

1. 取得 `G722p2_configuration-1.zip`（编码器测试）、`G722p2_configuration-2_1/2_2/2_3.zip`（解码器测试）。
2. 解压到任意目录，`./firmware/tests/g722/run.sh --itu <目录>`。
3. 当前 `--itu` 只上报状态、**尚未判分**：ITU 向量是 class T1/T2/T3 的二进制数值格式，且编码器测试要求关闭带分裂滤波器（codec 内的 `itu_test_mode` 路径），需要单独实现读取器与模式切换，属于独立工作量。
4. 实现后应在 `run.sh` 中把 ITU 结果纳入退出码，并同步更新本文表格。
