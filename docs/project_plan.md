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
| Linux 系统 | epoll/signalfd/timerfd、非阻塞 socket、Unix socket、写线程 + 有界 ring、二进制日志、HTTP/SSE | M4 P0、M5、M6 soak |
| 工程化 | 共享纯 C 协议、fuzz、ctest/ASan、台架工具、文档单一事实源 | M3.0、M4 CI |

非目标：CAN FD（S3 不支持）、多通道、多设备集群、云端；Web 仅作可视化层（单文件、无框架/构建）。

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
                  └─ 触摸 / GPIO0        linsightd ─ Unix/TCP → CLI · HTTP/SSE → 浏览器
```

ring（约 52k 条）供网络排空，UI history 独立；主机追数据时 trace 不会被抽空。

## 3. 现状与缺陷

M0–M2 已跑通：采集/UI/流式/CONTROL-ACK/daemon/CLI/`.lsc`/导出；ctest 8/8、ASan/UBSan 通过，
集成测试覆盖 CLI、`.lsc`、HTTP/SSE、Web 控制与 1 MB 分卷轮转。ESP32 自测 30000 帧启动
突发 4330 fps、96% 负载零丢失。M3.1：60 万帧/10.9 min（917 fps）计数器 0..599999 连续、
零丢帧；M3.4：1 Mbps/60 s 54k 帧零丢、无复位/WDT（825 fps，~10% 负载）；M3.5：堆 +1.9 KB。

| 缺陷 | 影响 | 修复 |
|---|---|---|
| ring 满时先前进 head | 丢第二旧记录、保留最旧记录 | 满时原地覆盖 head，`dropped++` |
| 记录结构体 24 B，ring 按 20 B 分配 | 越界、槽位重叠、随机错帧 | 字段重排为 20 B + `_Static_assert` |
| `set_config` 缺字段时静默改模式 | 只改比特率却切走 NORMAL | 缺失保持、非法整体拒绝、同配置不重启 |
| `twai` restart/recover 无互斥 | 并发重启、paused 丢失、状态提前提交 | 重启锁；成功后才提交 bitrate/mode/state；保留 paused |
| PCAN 假活无检测 | 高速灌帧后必须拔插 USB | 速率建议、单帧探测、stats 新鲜度、失败锁存 |

M4 P0 已实现：`config` ACK、`replay`（40 帧 p99 0.30 ms）、`compare`、`.lsc` 校验、ASan/UBSan + CI。
待修：epoll 循环内阻塞 `send_all`、writer 丢帧计数未上报、ASC 格式需 can-utils 对照。

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

Windows 定时或 PCAN-USB（经 usbip 约 1 ms/帧）达不到 3500 fps 时降级为 1000 fps ×10 min
严格对照 + 60 s 高负载，并写明实际速率（本轮实测 917/825 fps）。

## 5. M4 Linux 工具链

P0（已实现，见 `~/linsight/host`）：

- `linsight-cli config`：只发显式字段，等待 ACK；非法值显示 `BAD_ARG`。
- `replay`：AF_CAN/CAN_RAW + `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`，p99 误差 <5 ms。
- `compare`：按 `(id, flags, dlc, data)` FIFO 匹配，报告 missing/duplicate/order；不做跨设备延迟分位。
- `.lsc` 魔数 + 版本校验；ASan/UBSan + ctest 接入 CI；删除随机索引（顺序回放不需要）。

P1：systemd（`DynamicUser` + journald）、`--log-max-mb` 分卷与 DBC 基础解码（非复用信号）已实现。
物理 PCAN 的 SocketCAN 对照已打通（usbipd + `peak_usb` + `can0`，50–917 fps 零丢帧；更高节拍受适配器 ~1 ms/帧限制）。

## 6. M5 CLI 深化与 Web 可视化

CLI：`live` 过滤/着色、`stats --watch` 周期刷新、`export` 时间与 ID 过滤；全部通过
Unix socket / `--ctl-port` 复用 daemon。验收：1 h 连续 `live` 不卡；3000 帧/s 可抽样核对。

Web：`src/http.c` 在同一 epoll 循环提供最小 HTTP/1.1 + SSE（无第三方库）；
`host/web/index.html` 单文件展示统计 tile、实时帧表、fps/负载曲线、Top IDs 与控制。
SSE 每事件 ≤64 条、发送非阻塞（慢浏览器丢事件）；`/api/control` 白名单转发不等 ACK。
验收：页面、`/api/stats`（含 `load_permille`）、SSE、暂停/恢复均由集成测试覆盖。

## 7. M6 Soak 与交付

24 h soak：800 fps 约 1.4 GB；`tools/soak24h.py` 已启动后台长跑，报告在 `~/can-logs/soak-latest`。
交付：`host/README.md`、接线图、CHANGELOG、tag v0.2.0；简历数字按实测回填。

## 8. 工程约定

- 固件只在 Windows 构建：
  `. 'C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1'; idf.py build`。
- 上位机只在 WSL `~/linsight` 开发/构建/运行；Windows 不构建 `host/`：
  `cd ~/linsight/host && cmake -S . -B build && cmake --build build -j"$(nproc)"`。
- 本仓库只提交固件；上位机在 `~/linsight` 单独提交；`protocol/`、`docs/` 改动两边同步。
- 台架用 WSL `can0`（见 [hardware.md](hardware.md) §5）；Python 台架工具留在上位机仓。
- 台架 Wi-Fi/主机地址集中在 `main/net.c` 的 `BENCH_*` 宏（只重编一个文件，已提交）；生产部署改回 menuconfig/私密配置。Wi-Fi 使用 CN 1–13 信道。
- 不提交 `build/`、`sdkconfig`、`managed_components/`；文档只放 `docs/`；提交信息用中文。

## 9. 风险与简历

风险：3.3 V 收发器/120 Ω/共地必须确认；高负载只在两节点台架；Wi-Fi 抖动与写盘
阻塞靠本地 ring + 丢弃计数兜底；M3 验收前不扩功能。

> **LinSight CAN — 无线 CAN 总线分析仪（ESP32-S3 + Linux）**
> - TWAI 采集 + PSRAM ring + 时间戳；LVGL 触摸屏 20 fps 实时 trace/负载/错误。
> - Linux C 守护进程：epoll + 非阻塞 TCP + 写线程 + 二进制日志/ASC 导出 + CLI。
> - 10.9 min 连续 60 万帧零丢帧（计数器逐帧校验）、1 Mbps 稳定、bus-off 自愈；24 h 长跑中。
> - 共享协议解析器 fuzz + ctest/ASan CI；SocketCAN 回放与双路对照（M4）。
