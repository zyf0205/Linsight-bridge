# LinSight 项目计划：无线 CAN 总线分析仪

> ESP32-S3 手持 CAN 分析仪（TWAI 采集 + LVGL 触摸 UI + Wi-Fi 流式）与 Linux 主机
> 工具链（epoll 守护进程 + `.lsc` 日志 + CLI）。接线见 [hardware.md](hardware.md)，
> UI 见 [ui_design.md](ui_design.md)，线格式以 `protocol/frame.h` 为准。

## 1. 定位与技能矩阵

面向嵌入式 / Linux 求职作品集：一台设备覆盖 MCU、LVGL 与 Linux 系统编程。

| 支柱 | 实现载体 | 验收证据 |
|---|---|---|
| MCU / RTOS | TWAI 采集、PSRAM ring、双子历史缓冲、Wi-Fi/TCP、任务优先级 | M3.4 / M3.5 |
| LVGL | 320×240 双页 UI、4 列固定坐标 trace、过滤/触发、跨任务邮箱 | M3.1 的 UI 项 |
| Linux 系统 | epoll/signalfd/timerfd、非阻塞 socket、Unix socket、写线程 + 有界 ring、二进制日志 | M4 P0、M6 soak |
| 工程化 | 共享纯 C 协议、fuzz、ctest/ASan、台架工具、文档单一事实源 | M3.0、M4 CI |

非目标：CAN FD（S3 不支持）、多通道、多设备集群、云端、Web/PySide6 产品 UI
（主机 UI 采用 CLI-first；`host/gui` 仅是台架测试台）。

## 2. 仓库与数据流

| 路径 | 内容 |
|---|---|
| `main/` | 固件：`twai.c` 采集、`can_ring.c` PSRAM ring、`net.c` 流式、`ui.c` LVGL、`bsp.c` 板级 |
| `protocol/` | `frame.h/.c` 协议 v2.0，纯 C、无 IDF/分配；固件仓与上位机仓各一份，改动需同步 |
| `tools/can_probe/` | M0 接线验证用 TWAI 探针 |
| `docs/` | 计划、硬件、UI；根目录只有 `CLAUDE.md` |
| `~/linsight`（WSL 上位机仓） | `host/`（daemon/CLI/tests）、`tools/bench_can.py`、协议与文档镜像 |

```text
CAN ── 收发器 ── ESP32-S3
                  ├─ twai_task → PSRAM ring → net_task ─Wi-Fi/TCP→ linsightd → .lsc
                  │            └→ 捕获侧 512 条 UI history → LVGL 20 Hz
                  └─ 触摸 / GPIO0
```

ring（约 52k 条）供网络排空，UI history 独立；主机追数据时 trace 不会被抽空。

## 3. 现状与缺陷

M0–M2 已跑通：采集/UI/流式/CONTROL-ACK/daemon/CLI/`.lsc`/导出。ESP32 自测
30000 帧启动突发 4330 fps、96% 负载零丢失（未触发 ring 溢出）；外部 PCAN 经
WSL2 SocketCAN 50 fps/10 s、1000 fps/10 s 零丢帧；主机 ctest 7/7、ASan/UBSan 通过。

| 缺陷 | 影响 | 修复 |
|---|---|---|
| ring 满时先前进 head | 丢第二旧记录、保留最旧记录 | 满时原地覆盖 head，`dropped++` |
| 记录结构体 24 B，ring 按 20 B 分配 | 越界、槽位重叠、随机错帧 | 字段重排为 20 B + `_Static_assert` |
| `set_config` 缺字段时静默改模式 | 只改比特率却切走 NORMAL | 缺失保持、非法整体拒绝、同配置不重启 |
| `twai` restart/recover 无互斥 | 并发重启、paused 丢失、状态提前提交 | 重启锁；成功后才提交 bitrate/mode/state；保留 paused |
| PCAN 假活无检测 | 高速灌帧后必须拔插 USB | 速率建议、单帧探测、stats 新鲜度、失败锁存 |

M4 P0 已实现：`config` ACK、`replay`（40 帧 p99 0.30 ms）、`compare`、`.lsc` 魔数/
版本校验、ASan/UBSan + CI。待修：epoll 循环内阻塞 `send_all`、writer 丢帧计数未上报、
ASC 格式需 can-utils 对照。

PCAN “假活”的复现与恢复见 [hardware.md](hardware.md) §5。

## 4. M3 台架健壮性

| 项 | 方法 | 通过标准 |
|---|---|---|
| M3.0 ring 溢出 | 连续 push 60k（>52k），排空后检查 `seq` | 模 65536 严格递增，`dropped` 正确；已有小容量单测 |
| M3.1 500k/80% ×10 min | PCAN 发送，payload 前 4 B 32 位计数器逐帧对照 | 3500 fps ±5%；设备 `rx_missed=0`、ring `dropped=0`、主机 writer `dropped=0` |
| M3.2 无 ACK 注入 | 运行中切 LISTEN_ONLY 或摘 CAN_H | 发送端 1.5 s 内停发并分类提示；恢复后无需重插 PCAN |
| M3.3 断线重连 | 杀 daemon，5 s 内重启 | 重连先补发断线积压（≤~52k 条），不是只发新帧 |
| M3.4 1 Mbps ×60 s | SELFTEST 自发或 PCAN 发送 | 允许 ring drop 但必须可见；无复位/WDT |
| M3.5 10 min 资源 | 1 Hz 日志补 `esp_get_free_heap_size()` / PSRAM free | 内部堆增量 <8 KB，PSRAM 增量 <64 KB |

Windows 定时达不到 3500 fps 时降级为 1000 fps ×10 min 严格对照 + 60 s 高负载，并写明实际速率。

## 5. M4 Linux 工具链

P0（已实现，见 `~/linsight/host`）：

- `linsight-cli config`：只发显式字段，等待 ACK；非法值显示 `BAD_ARG`。
- `replay`：AF_CAN/CAN_RAW + `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`，p99 误差 <5 ms。
- `compare`：按 `(id, flags, dlc, data)` FIFO 匹配，报告 missing/duplicate/order；不做跨设备延迟分位。
- `.lsc` 魔数 + 版本校验；ASan/UBSan + ctest 接入 CI。
- 删除 `.lsc` 随机索引（顺序回放不需要）。

P1（待做）：systemd/journald/logrotate、DBC（cantools）；物理 PCAN 的 SocketCAN 对照已打通
（usbipd + `peak_usb` + `can0`，50/1000 fps 零丢帧；3500 fps 待亚毫秒节拍）。

## 6. M5 CLI 深化

不新增 Web/PySide6/Tkinter 产品 UI：`live` 增加过滤/着色，`stats --watch` 周期刷新，
`export` 支持时间与 ID 过滤；全部通过 Unix socket / `--ctl-port` 复用 daemon。
验收：1 h 连续 `live` 不卡；3000 帧/s 时可抽样核对 CLI 输出。

## 7. M6 Soak 与交付

24 h soak（设备 + daemon + CLI）：1000 fps 约 1.7 GB、3500 fps 约 6.9 GB，必须配置
日志轮转。指标：RSS/句柄稳定、无复位、writer 与设备丢帧计数符合预期。
交付：README、接线图、3 min 演示、tag + changelog；简历数字按实测回填。

## 8. 工程约定

- 固件只在 Windows 构建：
  `. 'C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1'; idf.py build`。
- 上位机只在 WSL `~/linsight` 开发/构建/运行；Windows 不构建 `host/`：
  `cd ~/linsight/host && cmake -S . -B build && cmake --build build -j"$(nproc)"`。
- 本仓库只提交固件；上位机在 `~/linsight` 单独提交；`protocol/`、`docs/` 改动两边同步。
- 台架用 WSL `can0`（见 [hardware.md](hardware.md) §5）；Python 台架工具留在上位机仓。
- 文档只放 `docs/`；协议以 `protocol/frame.h` 为准，不复制表格。
- 台架 Wi-Fi/主机地址集中在 `main/net.c` 的 `BENCH_*` 宏（只重编一个文件，已提交）；生产部署改回 menuconfig/私密配置。Wi-Fi 使用 CN 1–13 信道。
- 不提交 `build/`、`sdkconfig`、`managed_components/`；提交信息用中文。

## 9. 风险与简历

风险：3.3 V 收发器/120 Ω/共地必须确认；高负载只在两节点台架；Wi-Fi 抖动与写盘
阻塞靠本地 ring + 丢弃计数兜底；M3 验收前不扩功能。

> **LinSight CAN — 无线 CAN 总线分析仪（ESP32-S3 + Linux）**
> - TWAI 采集 + PSRAM ring + 时间戳；LVGL 触摸屏 20 fps 实时 trace/负载/错误。
> - Linux C 守护进程：epoll + 非阻塞 TCP + 写线程 + 二进制日志/ASC 导出 + CLI。
> - 500 kbps/80% 10 min 零丢帧、bus-off 自愈、24 h soak 无泄漏（数字待实测回填）。
> - 共享协议解析器 fuzz + ctest/ASan CI；SocketCAN 回放与双路对照（M4）。
