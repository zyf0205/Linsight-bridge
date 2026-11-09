# LinSight UI 设计（320×240 / LVGL 8.3）

实现：`main/ui.c`；接口：`main/ui.h`。硬件基线：ST7789 320×240、FT6336 触摸、GPIO0。
本文只记录设计约束与布局；API 以头文件为准，不在文档里复制声明。

## 1. 设计目标与红线

- 默认 Monitor 页是**总线健康概览**：健康计数 + 按最近 1 秒帧率排序的 Top 6 ID；原始帧
  列表降为次级 **Raw** 视图。概览回答“总线现在怎么样”，Raw 回答“刚才具体是什么帧”。
- 采集不阻塞：LVGL 不遍历网络 ring，只读捕获侧 512 条 UI history 与 stats 快照。
- 零动态内存：控件一次创建；刷新只用静态缓冲 + `lv_label_set_text_static()`。
- 两个物理页容器：Monitor 容器内放 Overview / Raw 两个互斥 group，Settings 为另一页；
  无滚动、无 shadow/渐变/半透明叠加；draw buffer 常驻内部 DMA（320×40×2）。
- 启动动画：`lv_layer_top()` 全屏遮罩，`LinSight` 品牌与 `CAN ANALYZER` 淡入，1.2 s
  后淡出并删除；期间遮罩吸收触摸。
- 断网/无主机时本地采集、过滤、触发、统计照常工作。

## 2. 色板

| 角色 | 值 | 用途 |
|---|---|---|
| 画布 | `0x000000` | 背景 |
| 栏 / 卡 | `0x161618` / `0x1C1C1E` | 顶底栏、健康条、Settings 卡片 |
| 斑马行 | `0x0B0B0E` / `0x141419` | Overview/Raw 偶数/奇数行 |
| 按压 / 选中 | `0x2C2C2E` | 行、按钮按压 |
| 主 / 次文本 | `0xF8FAFC` / `0x8E8E93` | 数据、表头/单位 |
| 标准 / 扩展 ID | `0x0A84FF` / `0xBF5AF2` | blue / purple（含活动条） |
| 正常 / 告警 / 故障 | `0x30D158` / `0xFF9F0A` / `0xFF453A` | ACTIVE、PASSIVE/PAUSED、BUS-OFF/OVF |

## 3. Monitor：Overview（默认）

```text
+----------------------------------------------------------------+ 0
| [ACTIVE] 500K RUN    ● HOST 3ms             [gear]             | 26
+----------------------------------------------------------------+ 26
|  MISS 0       QFULL 0       BERR 0       BOFF 0                | 24
+----------------------------------------------------------------+ 50
| ID                  RATE/s       PER      CNT                  | 16
+----------------------------------------------------------------+ 66
| 123                  1200        <1      14.2k              █  | 24
| ... 共 6 行，斑马纹，点击行锁定该 ID ...                        |
+----------------------------------------------------------------+ 210
| FPS 4330      LOAD 82%      ERR 0/0       OVF 0                | 30
+----------------------------------------------------------------+ 240
```

- 顶栏/底栏与旧版相同。健康条：`MISS`=`rx_missed`、`QFULL`=`rx_queue_full`、
  `BERR`=`bus_errors`、`BOFF`=`bus_off`；为 0 暗色，MISS/QFULL 橙色，BERR/BOFF 红色。
- 排名：每 tick 对 128 个计数槽做插入排序（无堆），键依次为 1 s 帧率降序 → 累计计数
  降序 → ID 升序 → 标准帧先于扩展帧。只显示 6 行。
- 列：`ID` 标准帧 `%03X` / 扩展帧 `%08X`（Montserrat 20，x=4 w=132）；`RATE/s`
  （Montserrat 20，右对齐 140..202）；`PER` 周期 ms（`rate>1000` 显示 `<1`）；
  `CNT` 为 Clear 后累计（`14.2k` 缩写）；活动条 x=311 w=6，高度 2..20 px，按本行
  速率相对榜首缩放，std 蓝 / ext 紫。
- 速率是**翻滚式 1 秒窗口**（约 1 Hz 更新）：`rate = win*1000/elapsed`，随后窗口清零。
  首个完整窗口前速率未知，按累计计数排序、RATE/PER 显示 `---`；之后只列 `rate>0` 的 ID。
- Overview **忽略 filter**；点击行仍锁定/解锁 ID（与 Raw 共用同一锁定状态）。
  无 ID 时提示 `Waiting for CAN frames / check wiring / bitrate`；有 ID 但最近 1 s
  无流量时提示 `No traffic in the last 1 s`。

## 4. Monitor：Raw（次级）

```text
+----------------------------------------------------------------+ 26
| ID          L  DATA (HEX)                                 CNT  | 16
+----------------------------------------------------------------+ 42
| 00000123    8  02 01 0C 55 AA 00 00 00                   14.2k | 21
| ... 8 行，斑马纹，点击行锁定该 ID ...                           |
+----------------------------------------------------------------+ 210
```

- 复用原 trace 的 8×21 px 行：`L` 为 DLC（RTR 显示 `R`），`DATA` 最多 23 字符，
  `CNT` 为按 ID 累计计数；锁定行左侧 3 px 蓝条。
- filter 只作用于本视图；错误帧始终可见。无匹配帧时覆盖同样的等待提示。

## 5. Settings 与状态

```text
+----------------------------------------------------------------+
| < Back                                         REMOTE SYNC      | 28
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
- FILTER：All/Std/Ext/锁定 ID；锁定 ID 同时是 TRIG 的候选目标。`Clear` 清设备统计
  与本地计数/速率/两种视图；`Recover` 调 `twai_capture_recover()`。
- TRIG 单次触发：Locked ID 命中 → 自动跳到 **Raw** 并冻结在触发帧；Error/BOff 命中
  → 自动跳到 **Overview**，健康计数与状态胶囊就是现场。再按 Pause/GPIO0 只解除冻结。
- 顶栏右侧状态：`REMOTE SYNC`（主机 CONTROL 后 1.5 s）/ `REC nF` / `SD READY` /
  `SD ERROR` / `NO SD`。

## 6. 输入与主机联动

| 触发 | 行为 |
|---|---|
| GPIO0 短按 | 息屏时先亮屏；亮屏时 Pause/Resume（按住 LVGL 锁执行） |
| GPIO0 长按 | 循环 Overview → Raw → Settings → Overview |
| 左滑 | Overview→Raw，Raw→Settings |
| 右滑 | Raw→Overview，Settings→进入前的视图 |
| 齿轮 / 返回 | 齿轮从任一 Monitor 视图进 Settings；Back 返回进入前的视图 |
| 点击行 | Overview 或 Raw 均可锁定/取消该 ID，并同步过滤与触发目标 |
| 主机 CONTROL | `pause`（可带 lease）/`resume`/`clear`/`mark`/`set_config` 生效后回 ACK；UI 显示 `REMOTE SYNC` 1.5 s |

- 主机 pause 是真正的采集暂停（`twai_capture_set_paused`，租约 5 s–30 min，超时或主机
  断开自动恢复）；本地 Pause 只冻结画面（`s_frozen`），采集与网络流式继续。
- `twai_capture_set_paused` / `linsight_ui_set_conn` / `sync_control` 只写无锁邮箱；
  20 Hz tick 在 LVGL 任务内消费。GPIO 按键处理会取 `lvgl_port_lock`，不得从 net_task 调用。

## 7. 刷新、性能与已知上限

- LVGL 定时器 50 ms（20 Hz），每 tick 最多扫描 512 条 UI history；只更新内容变化的
  Label/颜色；静态文本缓冲，无堆分配。目标单帧渲染 <8 ms，触摸响应 <100 ms。
- 冻结/触发/主机 pause 只冻结行渲染；健康条与底栏继续实时更新。主机 pause 期间不采样
  速率并每 tick 重置窗口起点，恢复后的第一个窗口不会被暂停时长稀释。
- 计数与缩写用整数运算；禁止 `%f` 格式化。`lv_tick_elaps()` 处理 tick 回绕。
- 上限：`SCAN_N=512`/50 ms 把 per-ID 计数限制在约 10k fps；更高突发下各行速率之和
  可能低于底栏全局 FPS。128 槽满时驱逐复用槽，会丢失该 ID 的累计计数与速率。
