# LinSight 硬件

> 立创·实战派 ESP32-S3（无电池 / 无摄像头）；来源：官方 wiki + `08-lcd_lvgl` BSP。更新：2026-11-08。

## 1. 板卡

| 项 | 参数 |
|---|---|
| 模组 | ESP32-S3-WROOM-1-N16R8（16MB Flash + 8MB Octal PSRAM） |
| 屏幕 | 2.0" IPS **320×240**，ST7789，SPI3 |
| 触摸 / IMU | FT6336 / QMI8658，I2C0 |
| USB | Type-C → CH334F HUB → CH340K 串口 + 原生 USB |
| 按键 / 本版无 | RESET + GPIO0 用户键；无 GC0308 摄像头、无电池 |

## 2. 引脚

| GPIO | 用途 | 备注 |
|---|---|---|
| 0 | BOOT / 用户键 | strapping，低有效 |
| 1 / 2 | I2C0 SDA / SCL | 触摸/PCA9557/IMU/音频共用 |
| 3–9, 15–18, 46 | 摄像头 | 本版本无，保留；46 必须低，3 也是 strapping |
| 10 / 11 | **多功能扩展口** | 唯一自由 GPIO |
| 13 / 14 / 38 / 45 | I2S WS/BCK/MCLK/DO | 音频；45 是 strapping |
| 19 / 20 | USB D-/D+ | 保留 |
| 21 / 47 / 48 | TF D0 / CLK / CMD | 1-SD 模式 |
| 26–37 | Flash / Octal PSRAM | **保留**（35/36/37 明确被 PSRAM 用） |
| 39 / 40 / 41 / 42 | LCD DC / MOSI / CLK / 背光 | 背光低电平点亮 |
| 43 / 44 | UART0 TX / RX | CH340K |

- 除扩展口外没有引出其他 GPIO（GPIO12、22–25 需焊测试点）；所有 IO 为 **3.3V，不耐 5V**。
- TF 卡：板载 1-bit SDMMC（CLK/CMD/D0 = 47/48/21），无卡检测脚；FAT32、8.3 名 `recNNNNN.lsc`，每 5 s 重试挂载。

## 3. 显示与触摸

| 项 | 参数 |
|---|---|
| LCD CS | **PCA9557 BIT0 @0x19**，不是 GPIO |
| LCD RST | 未连接 |
| SPI | Mode 2，80MHz，`trans_queue_depth=10`，16bpp |
| 面板 | `invert_color=true`，`swap_xy=true`，`mirror_x=true` |
| 触摸 | 地址 0x38，INT/RST 未接，轮询；`x_max=240, y_max=320, swap_xy=1, mirror_x=1` |
| 组件版本 | LVGL 8.3.0 + esp_lvgl_port 1.4.0 + esp_lcd_touch_ft5x06 1.0.6 |

**初始化顺序（漏了就黑屏）**：

```text
bsp_i2c_init() → pca9557_init() → panel_reset() → lcd_cs(0) → panel_init()
```

**LVGL**：官方基线 320×20、单缓冲、PSRAM；本项目用 320×40、双缓冲、内部 DMA RAM。`CONFIG_LV_COLOR_16_SWAP=y`。

## 4. PCA9557 与 I2C

PCA9557 地址 **0x19**，寄存器：`0x00` 输入、`0x01` 输出、`0x02` 极性、`0x03` 配置（1=输入，0=输出）。

| Bit | 信号 | 上电默认 |
|---|---|---|
| 0 | LCD_CS | 1 |
| 1 | PA_EN | 0 |
| 2 | DVP_PWDN | 1 |

初始化：输出寄存器写 `0x05`，配置寄存器写 `0xF8`（低 3 位输出）。

设备：PCA9557 `0x19`、FT6336 `0x38`、QMI8658 `0x6A`、ES7210 `0x41`、ES8311 `0x18`。

## 5. 扩展口（HC-GH-5PWT，5P）

```text
多功能 J2：5V  3V3  GND  IO10  IO11
           5    4    3    2     1
I2C   J1：5V  3V3  GND  SDA   SCL
           5    4    3    2     1
```

- 多功能口：1=**GPIO11**，2=**GPIO10**；可 GPIO/UART/I2C/CAN/PWM/ADC。GPIO10=ADC1_CH9，GPIO11=ADC2_CH0（Wi-Fi 下受限）。
- I2C 口：1=**GPIO2/SCL**，2=**GPIO1/SDA**；与板载设备共用总线，地址不能冲突。
- 线序用万用表确认（引脚 1 以连接器小三角和丝印为准）；5V 只在 USB 供电时存在。

### CAN 接线（M0 台架已验证）

```text
USB-CAN(PCAN-USB) CAN_L ── 收发器 CANL
USB-CAN            CAN_H ── 收发器 CANH
收发器 VCC ── 3V3 (J2 引脚4)   收发器 GND ── GND (J2 引脚3)
收发器 TXD ── GPIO10 (J2 引脚2)  收发器 RXD ── GPIO11 (J2 引脚1)
```

- 3.3V 收发器（SN65HVD230 类）；两端 120Ω 终端、共地。仅 CAN 2.0A/B，默认 500k；固件默认 `LISTEN_ONLY`（不 ACK），台架单对节点需切 `NORMAL`，否则发送方收不到 ACK 会重发。

| 比特率 | 8B 标准帧理论上限 | 80% 负载 | 台架建议 |
|---|---:|---:|---:|
| 500k | ~3800 fps | ~3000 fps | ≤2000 fps |
| 1M | ~7600 fps | ~6000 fps | ≤4000 fps |

- 高速连续发送会让部分 PCAN-USB 假活（`CAN_Write` OK、status 0x0，回显/上行停摆）；
  软复位无效，只能拔插或管理员 `pnputil /restart-device`。
- 台架改用 **WSL2 SocketCAN**：管理员 `usbipd bind --busid 2-1` 一次；每次拔插/WSL 重启后 `usbipd attach --wsl --busid 2-1`。WSL 内 `sudo modprobe peak_usb`、
  `sudo ip link set can0 type can bitrate 500000 && sudo ip link set can0 up`；
  `cangen can0 -n 500 -g 20 -I 0x123 -L 8` 发帧，以设备 `total` 为判据（不依赖回显）。
  实测 50 fps/10 s +500；PCAN-USB 经 usbip 约 1 ms/帧，持续上限约 830–920 fps，`missed/rxq_full/dropped` 全 0。

## 6. sdkconfig 必改

| 项 | 改为 |
|---|---|
| `CONFIG_COMPILER_OPTIMIZATION_DEBUG` | `CONFIG_COMPILER_OPTIMIZATION_PERF=y`（-O2） |
| CPU / RTOS | 240 MHz，`CONFIG_FREERTOS_HZ=1000` |
| TCP 窗口 / 发送缓冲 / RECVMBOX | 32768 / 32768 / 32 |
| `CONFIG_ESPTOOLPY_FLASHMODE_DIO` | QIO（先验证启动） |
| `CONFIG_LV_COLOR_16_SWAP` | y |
| FATFS | `LFN_NONE=y`、`ALLOC_PREFER_EXTRAM=y`（SD 黑盒，8.3 文件名） |
| 分区表 | 加 64KB `coredump` 分区 |

注意：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 会让 >16KB 的 `malloc` 落 PSRAM；draw buffer 必须 `heap_caps_malloc(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`。

## 7. 上电自检

- [ ] `esp_psram_get_size()` = 8MB；I2C 扫描到 0x19 / 0x38 / 0x6A
- [ ] 纯色全屏正常（红/绿）
- [ ] 背光 0% / 100% 可控，触摸四角坐标准确
