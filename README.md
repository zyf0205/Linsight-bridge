# LinSight 桥接服务

LinSight 无线 CAN 分析仪的 Linux 上位机。`linsightd` 是基于单线程 `epoll` 的轻量守护进程，接收设备 TCP 会话，跳过重连积压，并按设备时间戳把记录 1:1 转发到内核 SocketCAN。抓包、文本流与波形分析直接交由 Linux 生态工具（`candump`、SavvyCAN、PlotJuggler、`tcpdump`）处理。

```text
设备 ──Wi-Fi/TCP──> linsightd ──> --vcan IFACE  → SavvyCAN / PlotJuggler / candump / tcpdump
```

设备使用 v2.1 协议，解析器在 `protocol/`。

## 构建与运行

```bash
cmake -S . -B build && cmake --build build -j"$(nproc)"

sudo ip link add vcan0 type vcan 2>/dev/null || true
sudo ip link set vcan0 up
sudo ./build/linsightd --port 8888 --vcan vcan0
```

`--vcan` 需要 `CAP_NET_RAW`。

## 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--port N` | `8888` | 设备 TCP 监听端口 |
| `--discovery-port N` | `8889` | UDP 设备发现端口，`0` 关闭 |
| `--vcan IFACE` | 无 | 把记录转发到 SocketCAN 接口 |
| `--no-pace` / `--pace 0` | 关（默认 `1` 节流） | 关闭节流，收到即突发转发到 SocketCAN |
| `--version` | | 打印版本号 |

## 转发与节流

- 设备时间通过 HELLO 的 `uptime_ms` 对齐本机 `CLOCK_MONOTONIC` 单调时钟；旧固件没有该字段时，用首条记录的时间对齐。
- 默认按设备时间戳 1:1 到点转发，SavvyCAN / PlotJuggler 里看到的周期就是真实周期。重连积压的目标时间已过，立即发出，不做追赶式休眠。
- `--no-pace`（或 `--pace 0`）收到即发。
- 队列 262144 条，满了丢最旧。每轮主循环最多转发 4096 条或耗时 5 ms，积压不会拖住 PING/PONG。
- EXT/RTR/ERR 记录分别映射到 `CAN_EFF_FLAG`/`CAN_RTR_FLAG`/`CAN_ERR_FLAG`；接口队列满时写失败计入 `vcan_dropped`。

## 去重与会话

- 同时只接受一个设备连接，新连接顶掉旧连接。
- 按 `ts_ms`/`seq` 记标记，设备重连后重发的积压被跳过；`boot` 计数变化（设备重启）时清空标记。
- 设备每 5 s 发 PING，30 s 无数据则断开。

## 监控与统计

stderr 每秒打印一行统计：

```text
linsightd: recs=37366 (+120) vcan=37366 vcan_dropped=0 paceq=0
```

`recs` 为收到的有效记录，其余为 SocketCAN 转发帧数、写失败数和节流队列长度。

## 设备发现

设备上电或重连后向 UDP 8889 广播 `LINSIGHT?1`，`linsightd` 从来源地址单播回复 `LINSIGHT!1 <tcp_port>\n`，设备改用回复中的 TCP 端口连接。主机 IP 或端口变化不需要重烧固件。

- `--discovery-port 0` 关闭发现，设备回退到固件里的静态 `CONFIG_LINSIGHT_HOST_IP/PORT`。
- 广播必须能到达主机：WSL 使用 mirrored 网络模式，NAT 模式收不到局域网广播。
- 启用防火墙时放行 TCP 8888 和 UDP 8889（Windows 防火墙与 Hyper-V 防火墙）。

## 开机自启

```bash
sudo cmake --install build
sudo systemctl daemon-reload
sudo systemctl enable --now vcan0.service linsightd
```

重新构建后：

```bash
sudo cmake --install build
sudo systemctl restart linsightd
```

安装 `/usr/local/bin/linsightd`、两个 systemd unit 和 `/usr/local/etc/default/linsightd`（参数默认 `--port 8888 --vcan vcan0`）。`vcan0.service` 先创建并启用 vcan0；`linsightd.service` 使用 `DynamicUser`、`ProtectSystem=strict`，只授予 `CAP_NET_RAW`。

日志：`journalctl -u linsightd -f`。

## 分析工具

| 需求 | 工具 | 用法 |
|---|---|---|
| 波形、DBC 解码、信号曲线 | SavvyCAN | 连接 SocketCAN `vcan0` |
| 实时曲线 | PlotJuggler | 订阅 `vcan0` |
| 终端查看 | can-utils | `candump vcan0` |
| 抓包留档 | tcpdump / Wireshark | `sudo tcpdump -i vcan0 -w cap.pcapng` |

## 仓库结构

```text
src/main.c  单文件 epoll 守护进程（TCP/UDP/signalfd/timerfd/SocketCAN、会话去重、单调时钟节流）
protocol/   共享 v2.1 解析器（与固件仓库同步）
systemd/    linsightd.service、vcan0.service、参数模板
```
