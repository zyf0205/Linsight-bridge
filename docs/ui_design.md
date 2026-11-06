# LinSight UI 设计（320×240 / LVGL 8.3）

实现：`main/ui.c`；接口：`main/ui.h`。硬件基线：ST7789 320×240、FT6336 触摸、GPIO0。
本文只记录设计约束与布局；API 以头文件为准，不在文档里复制声明。

## 1. 设计目标与红线

- 仪器级可读性：8 行 trace，每行 4 个固定 X 坐标 Label（ID/L/DATA/CNT），20 Hz 不抖列。
- 采集不阻塞：LVGL 不遍历网络 ring，只读捕获侧 512 条 UI history 与 stats 快照。
- 零动态内存：控件一次创建；刷新只用静态缓冲 + `lv_label_set_text_static()`。
- 两页、无滚动、无 shadow/渐变/半透明叠加；draw buffer 常驻内部 DMA（320×40×2）。
- 断网/无主机时本地采集、过滤、触发、统计照常工作。

## 2. 色板

| 角色 | 值 | 用途 |
|---|---|---|
| 画布 | `0x000000` | 背景 |
| 栏 / 卡 | `0x161618` / `0x1C1C1E` | 顶底栏、Settings 卡片 |
| 斑马行 | `0x0B0B0E` / `0x141419` | trace 偶数/奇数行 |
| 按压 / 选中 | `0x2C2C2E` | 行、按钮按压 |
| 主 / 次文本 | `0xF8FAFC` / `0x8E8E93` | 数据、表头/单位 |
| 标准 / 扩展 ID | `0x0A84FF` / `0xBF5AF2` | blue / purple |
| 正常 / 告警 / 故障 | `0x30D158` / `0xFF9F0A` / `0xFF453A` | ACTIVE、PASSIVE/PAUSED、BUS-OFF/OVF |

## 3. Monitor 布局

```text
+----------------------------------------------------------------+ 0
| [ACTIVE] 500K   RUN        ● HOST 3ms                   [gear] | 26
+----------------------------------------------------------------+ 26
| ID          L  DATA (HEX)                                 CNT  | 16
+----------------------------------------------------------------+ 42
| 00000123    8  02 01 0C 55 AA 00 00 00                   14.2k | 21
| ... 8 行，斑马纹，点击行锁定该 ID ...                          |
+----------------------------------------------------------------+ 210
| FPS 4330      LOAD 82%      ERR 0/0       OVF 0                | 30
+----------------------------------------------------------------+ 240
```

- 顶栏：总线胶囊（ACTIVE/PASSIVE/BUS-OFF/STOPPED）、比特率（`500K`，只听加 ` LO`）、
  运行态（RUN/PAUSED/TRIG'D/STOP）、主机链路（`WIFI`→`LINK`→`HOST <rtt>ms`，
  断线重试显示 `RETRY`）、设置按钮。
- Trace：`ID` 标准帧 3 位/扩展帧 8 位十六进制；`L` 为 DLC，RTR 显示 `R`；
  `DATA` 最多 23 字符；`CNT` 为按 ID 计数（128 槽），超 10000 缩写为 `14.2k`。
  锁定行左侧有 3 px 蓝色竖条；无匹配帧时覆盖提示 `Waiting for CAN frames /
  check wiring / bitrate`。
- 底栏：`FPS`、`LOAD`（≥75% 橙色）、`ERR TEC/REC`（≥128 红）、`OVF`（>0 红）。
- 操作反馈：比特率/模式切换、Clear、Recover、锁定 ID 在底栏上方弹出 1.4 s toast。

## 4. Settings 布局

```text
+----------------------------------------------------------------+
| < Monitor                                      LS-CAN v2.0.0    | 28
+----------------------------------------------------------------+
| [125K] [250K] [500K] [1M]                         [Normal]      | 52
+----------------------------------------------------------------+
| FILTER  [All IDs] [Std] [Ext] [0x---]                          |
| TRIG    [OFF] [Error/BOff] [Locked ID]                         | 76
+----------------------------------------------------------------+
| [ Pause ]          [ Clear ]          [ Recover ]              | 60
+----------------------------------------------------------------+
```

- 比特率点击调用 `twai_capture_restart()`；当前档位不重启。模式胶囊在 Normal 与
  Listen 间切换（Selftest 仅 menuconfig 配置，点击会切回 Normal）。
- FILTER：All/Std/Ext/锁定 ID；点击 trace 行锁定该 ID，再点同一行取消。锁定 ID
  同时是 TRIG 的候选目标。
- TRIG：OFF / Error-BOff / Locked ID，单次触发；命中后显示 `TRIG'D` 并冻结画面，
  若在 Settings 页会自动跳回 Monitor。再按 Pause/GPIO0 只解除画面冻结。
- 动作：`Clear` 清设备统计与本地视图；`Recover` 调 `twai_capture_recover()`；
  `Pause/Resume` 语义见 §5。

## 5. 输入与主机联动

| 触发 | 行为 |
|---|---|
| GPIO0 短按 | 息屏时先亮屏；亮屏时 Pause/Resume（按住 LVGL 锁执行） |
| GPIO0 长按 | Monitor ↔ Settings |
| 左右滑 / 齿轮 / 返回 | 切页 |
| 点击 trace 行 | 锁定/取消该 ID，并同步过滤与触发目标 |
| 主机 CONTROL | `pause/resume/clear/set_config` 生效后回 ACK；UI 显示 `REMOTE SYNC` 1.5 s |

- `sync_control()` 预留 `filter_id` 参数；当前 `net.c` 对所有 CONTROL 传 `0xFFFFFFFF`，
  不改变过滤，只刷新 REMOTE SYNC 与统计。

- 主机 pause 是真正的采集暂停（`twai_capture_set_paused`），恢复时由同一按钮解除；
  本地 Pause 只冻结画面（`s_frozen`），采集与网络流式继续。
- `twai_capture_set_paused` / `linsight_ui_set_conn` / `sync_control` 只写无锁邮箱；
  20 Hz tick 在 LVGL 任务内消费。GPIO 按键处理会取 `lvgl_port_lock`，不得从 net_task 调用。

## 6. 刷新与性能

- LVGL 定时器 50 ms（20 Hz），每 tick 最多扫描 512 条 UI history；只更新内容变化的
  Label/颜色；静态文本缓冲，无堆分配。
- 目标：单帧渲染 <8 ms，触摸响应 <100 ms，trace 20 fps；绘制缓冲区双 320×40。
- 计数与缩写用整数运算；禁止 `%f` 格式化。
