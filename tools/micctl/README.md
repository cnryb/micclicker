# micctl

按需配置 CLI：读 S3 配置 HID 状态、读/写/恢复快捷键映射。不参与音频或键盘转发。

线格式见 [docs/config-hid.md](../../docs/config-hid.md)。VID:PID `303a:4014` 为原型标识。

## 依赖

- Python 3.9+
- `hidapi==0.15.0`（`import hid`；不要装纯 ctypes 的 `hid` 包，它还要系统里另有 `libhidapi`）

```bash
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
```

## 命令

```bash
./run.sh status
./run.sh map get
./run.sh map set --up 40:00 --down 10:00 --ok 00:28
./run.sh map reset
```

主机侧编解码单测（无需设备）：

```bash
./run.sh test
```

## 真机验收（macOS）

烧录产品固件后：

1. `./run.sh status` 可见 paired / link_up 与计数。
2. `./run.sh map get` 得到当前映射。
3. `./run.sh map set` 改为非默认 → 重启 S3 → `map get` 仍为新值。
4. 按 C3 的 UP/OK，主机 HID 跟随新映射。
5. `./run.sh map reset` 恢复默认。

证据写入仓库根 `.tmp/config-hid-YYYYMMDD-HHMM/`（不入库）。
