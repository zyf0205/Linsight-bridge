# LinSight 桥接服务

LinSight 无线 CAN 分析仪的 Linux 桥接服务。`linsightd` 用单线程 epoll
循环接收设备的 TCP 会话，对重连积压去重，再按设备时间戳把有效记录转发到
SocketCAN（`--vcan`）或输出 candump 文本（`--dump`）。服务本身不维护采集
文件：需要文本落盘时用 `--dump`，完整采集由设备 SD 卡或 tcpdump/SavvyCAN
负责。

设备使用 v2.1 协议，解析器位于 `protocol/`。

```text
设备 ──Wi-Fi/TCP──> linsightd ──> --vcan IFACE  → SavvyCAN / candump
                         └──────> --dump PATH|-  → candump 文本
```

## 快速开始

```bash
cmake -S . -B build && cmake --build build -j"$(nproc)"

sudo ip link add vcan0 type vcan 2>/dev/null || true
sudo ip link set vcan0 up
sudo ./build/linsightd --port 8888 --vcan vcan0
```

`--vcan` 需要 `CAP_NET_RAW`；`--dump` 不需要 root。

## 参数

| 参数 | 说明 |
|---|---|
| `--port N` | 设备 TCP 端口（默认 `8888`） |
| `--vcan IFACE` | 把有效记录转发到 SocketCAN |
| `--pace SPEED` | 节流倍速，`0` = 不节流（默认 `1.0`） |
| `--pace-replay` | 按历史间隔 1:1 回放，而不是压缩到实时 |
| `--dump PATH\|-` | 输出 candump 文本（`(sec.usec) can0 123#AABB...`）到文件或 stdout |

转发按设备时间戳节流，SavvyCAN 里看到的周期时间是真实的。积压记录过了
目标时间就立即补发，不做追赶式休眠；接口队列满时计入 `vcan_dropped`。
EXT/RTR/ERR 记录分别映射到
`CAN_EFF_FLAG`/`CAN_RTR_FLAG`/`CAN_ERR_FLAG`。

设备每 5 s 发一次 PING，静默 30 s 后断开。1 Hz 统计行输出
`recs/dump/vcan/vcan_dropped/paceq`。

## 服务（开机自启）

```bash
sudo cmake --install build
sudo systemctl daemon-reload
sudo systemctl enable --now vcan0.service linsightd
sudo systemctl restart linsightd   # 重新构建后使用
```

安装 `linsightd`、两个 systemd unit 和 `<prefix>/etc/default/linsightd`
（默认 `--port 8888 --vcan vcan0`）。`vcan0.service` 先创建虚拟 CAN
接口，`linsightd.service` 再启动；守护进程使用 `DynamicUser` 和
`CAP_NET_RAW`。

日志：`journalctl -u linsightd -f`。

## 分析工具

把现有工具指向 `vcan0`：

| 需求 | 工具 | 用法 |
|---|---|---|
| 实时波形、DBC 解码、信号曲线 | [SavvyCAN](https://www.savvycan.com/) | SocketCAN → `vcan0` |
| 终端快速查看 | can-utils | `candump vcan0` |
| 抓包留档 | Wireshark | `sudo tcpdump -i vcan0 -w cap.pcapng` |

## 仓库结构

```text
src/        linsightd（main.c）+ 会话去重与 pace 调度
protocol/   共享 v2.1 解析器
systemd/    linsightd.service、vcan0.service 和参数模板
```
