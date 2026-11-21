# FakePod Custom（ESP32-S31 移植版）

本工程由 [FakePod_Custom](https://github.com/a505283042/FakePod_Custom)（ESP32-S3 版本）复制并移植到 **FAKE POD NANO PRO（ESP32-S31）V1 量产版**。业务层（音乐 / 视频 / 电子书 / NSF / 图片 / BLE / Wi-Fi / NAS）沿用原工程，板级、驱动与电源管理按 [硬件说明](hw_pod_nano_s31_v1_开发者硬件说明.md) 重写。

以 460×460 AMOLED 圆角屏为核心，全部操作围绕触摸、两个实体键、体感手势完成。

固件版本单一入口见 [src/system/app_build_info.h](src/system/app_build_info.h)，当前为 **R149.3**。

---

## 目录

- [功能特性](#功能特性)
- [硬件平台](#硬件平台)
- [引脚速查](#引脚速查)
- [目录结构](#目录结构)
- [快速开始](#快速开始)
- [构建环境](#构建环境)
- [版本基线](#版本基线)
- [相对原工程的移植改动](#相对原工程的移植改动)
- [完整固件打包与离线烧录](#完整固件打包与离线烧录)
- [TF 卡目录约定](#tf-卡目录约定)
- [视频子系统](#视频子系统)
- [工具](#工具)
- [主机回归测试](#主机回归测试)
- [开发约定](#开发约定)
- [常见问题](#常见问题)
- [相关文档](#相关文档)

---

## 功能特性

| 模块 | 能力 |
|---|---|
| 音乐播放 | MP3 / FLAC（最高验证 96kHz）/ WAV / Opus；音频解码在独立任务，播放时钟与进度条同步 |
| 曲库组织 | 总列表、一级列表、二级列表三种范围；歌曲 / 歌手 / 专辑 / 年代四类分组；拼音首字母快速搜索 |
| 播放控制 | 顺序、列表循环、单曲循环、随机；上一首 / 下一首 / 拖动进度 / 音量 / 静音 |
| 视觉呈现 | 歌词页、三种频谱样式、磁带模式（外壳跟随封面主色自动着色）、封面位置微调 |
| 媒体来源 | TF 卡 `/MUSIC`；NAS（WiFi + HTTP 流 + 目录缓存，可切换服务器） |
| 视频 | AVI（MJPEG + MP3）经 esp_extractor 解封装，BoundedSPI 直出 QSPI，支持手势快进/快退、双击暂停、FVIX 索引精确定位 |
| 电子书 | `/txt` UTF-8 TXT，分页、书签、全屏阅读、阅读位置记忆、页索引缓存 |
| 电子音流 | `/synth` NSF 播放，内置合成器与专用数字补偿 |
| 图片浏览 | 扫描 SD 卡内 JPG / PNG，翻页浏览 |
| 系统 | 锁屏 / AOD 息屏 / 全黑熄屏、亮度、自动熄屏、输出模式（普通耳机 / 高阻耳机 / 线路输出）、辅助键模式 |
| 交互 | 触摸手势、电源键（AXP2101 PWRKEY）、辅助键 K1、体感翻转与双击、BLE 遥控 / BLE 手柄 |
| 连接 | USB TinyUSB MSC + CDC；BLE 手机 GATT 遥控 + Classic A2DP Source 可同时运行（HID 手柄 Host 当前仍关闭） |
| 电源 | AXP2101 电池电压/电量采集、上下电时序、机身姿态检测、关机前保存现场 |

完整操作说明见 [FakePod 使用操作说明](FakePod_使用操作说明.md)，视频手势见 [视频播放操作说明](视频播放操作说明.md)。

---

## 硬件平台

| 项目 | 规格 |
|---|---|
| SoC | ESP32-S31，双核 RISC-V，320 MHz |
| 内存 | 512 KB 内部 SRAM + 16 MB 片内 PSRAM（Octal，200 MHz） |
| Flash | 16 MB 片内 Flash，DIO / 80 MHz |
| 屏幕 | CO5300 AMOLED，460×460，QSPI，80 MHz，TE 同步，X 偏移 10 |
| 触摸 | CST820 电容触摸，I²C0，地址 `0x15` |
| 姿态 | QMI8658 六轴 IMU，I²C0，地址 `0x6B`（无 INT 引脚，寄存器轮询） |
| 音频 | CS43131 DAC，I²C1 控制（`0x30`），I²S1 数据，板载 24.576 MHz 晶振 |
| 电源管理 | AXP2101，I²C0（`0x34`）；电源键直连 PWRKEY，电池采集内置 |
| RTC | PCF85063，I²C0（`0x51`） |
| 存储 | TF 卡，SDMMC 4-bit |
| USB | TinyUSB MSC（TF 卡）+ CDC（日志） |
| 麦克风 | PDM，I²S0 |

## 引脚速查

| 功能 | GPIO |
|---|---|
| I²C0 SDA / SCL | 48 / 47 |
| I²C1 SDA / SCL（CS43131） | 55 / 54 |
| LCD VCI_EN | 44 |
| LCD RST / CS / SCK | 36 / 37 / 38 |
| LCD D0–D3 | 39 / 40 / 42 / 43 |
| LCD TE | 35 |
| 触摸 INT / RST | 45 / 46 |
| PMU INT | 49 |
| DAC BCLK / LRCK / DIN / RST | 52 / 51 / 50 / 53 |
| PDM DATA / CLK | 2 / 6 |
| SDMMC CLK / CMD | 24 / 25 |
| SDMMC D0–D3 | 20 / 21 / 22 / 23 |
| 辅助键 K1 | 61（低电平按下） |

完整定义见 [src/board/board_pins.h](src/board/board_pins.h)。

---

## 目录结构

```text
.
├── CMakeLists.txt              # ESP-IDF 顶层工程
├── platformio.ini              # PlatformIO 环境定义（默认 fakepod_s31）
├── partitions.csv              # 16MB 分区表
├── sdkconfig.defaults          # 基础 sdkconfig 片段
├── sdkconfig.bluetooth.defaults # Bluedroid 双模：BLE GATT + A2DP SBC
├── boards/fakepod_s31.json     # esp32s31 板卡定义
├── platform/espressif32_s31/   # 本地补丁的 PlatformIO 平台（必须保留）
├── src/
│   ├── main.cpp
│   ├── app/                    # APP 层：音乐/视频/电子书/NSF/图片/设置/启动器
│   ├── audio/                  # 音频服务、解码器、音源、频谱快照、NSF 合成
│   ├── assets/                 # 内嵌图片与动画帧
│   ├── board/                  # 板级引脚与屏幕尺寸定义
│   ├── boot/                   # 启动状态与版本输出
│   ├── drivers/                # i2c / touch / imu / storage / display / audio / power(AXP2101)
│   ├── media/library/          # 曲库索引、元数据、封面、NAS 源
│   ├── player/                 # 播放列表、状态机、传输控制
│   ├── system/                 # 设置、持久化、BLE、WiFi、NAS、电源、锁屏
│   ├── text/                   # 文本编码
│   └── ui/                     # LVGL 界面、手势路由、歌词、频谱、控件
├── components/esp-dsp/         # 本地 vendored ESP-DSP（已适配 GCC 15）
├── managed_components/         # 组件管理器下载的依赖
├── tools/                      # 电脑端工具（视频转换、NAS、固件合并）
└── tests/host/                 # 主机回归脚本
```

---

## 蓝牙音频发射 + 手机遥控共存（实验分支）

### 修复双模蓝牙下 96 kHz FLAC 的 I2S DMA 初始化失败（2026-11-21）

实机确认：44.1kHz FLAC + MH-M18 蓝牙耳机 + BLE 手机遥控可以稳定工作，
此前 SDMMC 临时 DMA 分配失败已消失。96kHz FLAC 起播仍因 I2S DMA 内存不足失败。
原因：常规高采样率配置为 26×256 帧×8B = 53,248B DMA 数据区；
蓝牙双模运行期间剩余 DMA 堆不足以再分配，I2S 报 ESP_ERR_NO_MEM。

本分支增加高采样率蓝牙共存 I2S 档：

- A2DP Profile 已初始化且播放 96/176.4kHz 普通音乐时，优先尝试 16×256 帧，
  若仅因 NO_MEM 失败才回退 14×256、12×256；每档确保预留至少 4KB DMA 余量。
- 保持原始 DAC 采样率和 32-bit stereo PCM：96kHz 仍然输出 96kHz，
  仅缩短 I2S DMA 队列；A2DP 侧仍使用独立的 SBC 采样率转换。
- 关闭蓝牙仍沿用原始 96kHz 的 26×256 I2S 配置；44.1/48kHz 既有路径不变。
- 新增 I2S_BT_DMA: try/skip/ready/init failed 诊断，包括实际缓冲档、
  可用 DMA 内存和 DMA runway。

重要：16×256 在 96kHz 对应约 42.7ms 硬件 DMA runway，
小于原来的约 69.3ms。能分配成功不等于播放一定无爆音；
请结合实际试听与 PCM_FEED_PERF 观测确认。

重新烧录：

```powershell
git switch feat/a2dp-source-ble-remote
git pull --ff-only
pio run -e fakepod_s31 -t clean
pio run -e fakepod_s31 -t upload
pio device monitor -e fakepod_s31
```

先连接耳机，再播放同一首 96kHz FLAC；应出现 I2S_BT_DMA: ready rate=96000。
如失败，请提供 I2S_BT_DMA / A2DP_TX / AUDIO_FAULT 日志；
如有爆音，请说明有线耳机还是蓝牙耳机、是否仅在切歌或 UI 刷新时出现。

---
### 修复连接耳机后 FLAC 无法播放：SDMMC / I2S DMA 内存不足

实机确认耳机 MH-M18 可被搜索、A2DP Profile 和媒体流已启动、手机 BLE GATT 遥控同时连接成功。
但起播后 `sdmmc_cmd: allocate_dma_buf: not enough mem`、`diskio_sdmmc: sdmmc_read_blocks failed`、
`i2s_alloc_dma_desc: allocate DMA buffer failed`，故障时剩余 Internal 约 18.9KB，
DMA 只有约 3.8KB（而 PSRAM 仍有 15MB 以上）。

ESP-IDF 的 `sdmmc_read_sectors` 在 FAT 文件目标缓冲不支持直接 DMA 时，会临时尝试
`heap_caps_malloc(..., MALLOC_CAP_DMA)`，不断将容量减半直到 512B。
在双模 BT + I2S 模式下，内部 DMA 堆碎片或缺口导致 SD 文件读失败。
FLAC `ESP_ERR_INVALID_STATE` 是该读取失败的**后果**，并非文件损坏。

实验分支修复：

- 调整 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`：**16384 → 2048**。普通小内存申请更多偏向
  PSRAM，优先保留内部 RAM 给 BT 主机、SDMMC、I2S DMA 和必需的任务栈。
- `sdcard_init()` 挂载时就申请**专用 4096B DMA bounce 缓冲**，
  `host.dma_aligned_buffer` 指向它，同时限制单次非对齐读到 8 个扇区。
  已挂载期间整个 SDMMC Host 复用，避免播放期间反复临时分配。
  挂载失败/USB MSC 卸载成功后按所有权释放，保留原 USB 热切换流程。
- 新配置文件 `sdkconfig.fakepod_s31_bt_dma`，防止旧固件配置被继续复用；
  编译时检查 PSRAM malloc 阈值必须 ≤2048。
- 每 10 秒 `A2DP_TX` 日志附带 `DMA_free`、`DMA_largest`，用于判断是否留足连续 DMA 空间。

重新构建、确认新配置并烧录：

```powershell
git switch feat/a2dp-source-ble-remote
git pull --ff-only
pio run -e fakepod_s31 -t clean
pio run -e fakepod_s31
Select-String .\sdkconfig.fakepod_s31_bt_dma -Pattern 'CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL|CONFIG_BTDM_CTRL_MODE_BTDM'
pio run -e fakepod_s31 -t upload
pio device monitor -e fakepod_s31
```

正确应显示 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048`、
`CONFIG_BTDM_CTRL_MODE_BTDM=y`，启动期应有
`SDMMC持久DMA缓存：4096B`，后续 A2DP 播放日志有 `DMA_largest`。
请依次测试 **耳机连接 → FLAC 44.1kHz/16-bit 连续 2 分钟 → MP3 → 暂停/切歌 → 手机BLE遥控**。
若仍报 DMA 内存不足，不要继续盲目改 DMA desc 或 FLAC ring，请保留
`SDMMC持久DMA缓存`、`A2DP_TX` 和 `AUDIO_FAULT` 的 RAM 数字后再精确优化。

### 修复“启动·使能Host”卡住：底层控制器未启用双模

实机日志确定卡在 `BLE_START step=4 bluedroid_enable`，控制器初始化已成功。
此前配置仅打开 `CONFIG_BT_CLASSIC_ENABLED`（Host）与
`CONFIG_BT_A2DP_ENABLE`（Profile），但未设置独立的 **底层控制器工作模式**
`CONFIG_BTDM_CTRL_MODE_BTDM=y`。ESP-IDF 6.1 的控制器模式是独立的 Kconfig
choice，可能默认保持 BLE Only，结果 Bluedroid 同时启动 Classic 与 BLE 时卡在
`esp_bluedroid_enable()` 的同步等待。这是目前最强的根因假设，仍待实机验证。

已修复：在 `sdkconfig.bluetooth.defaults` 明确配置
`CONFIG_BTDM_CTRL_MODE_BTDM=y`，增加 `CONFIG_BT_CTRL_BREDR_ENABLE` 与
双模模式的编译期检查，并把本分支 sdkconfig 改为独立的
`sdkconfig.fakepod_s31_bt_dma`，确保从新的默认配置生成，不复用旧
`sdkconfig.fakepod_s31_a2dp` 或主线配置。

更新后的实机测试命令：

```powershell
git switch feat/a2dp-source-ble-remote
git pull --ff-only
pio run -e fakepod_s31 -t clean
pio run -e fakepod_s31
Select-String -Path .\sdkconfig.fakepod_s31_bt_dma -Pattern '^(CONFIG_BTDM_CTRL_MODE_BTDM=y|CONFIG_BT_CTRL_BREDR_ENABLE=y|CONFIG_BT_CLASSIC_ENABLED=y|CONFIG_BT_A2DP_ENABLE=y)'
pio run -e fakepod_s31 -t upload
pio device monitor -e fakepod_s31
```

正确生成配置必须同时包含 **四个 =y**。新日志应打印
`BT_MODE: controller=3 expect_dual=3, host_classic=1, controller_bredr=1`。
若仍卡在 `step=4`，需进一步检查 Bluedroid Host 的 BTU/BTC 任务、
内部 RAM 余量与 HCI 初始化响应；仅增加启动超时无法解决同步阻塞。

### 修复蓝牙模式反复显示“启动中”（诊断增强）

BLE 广播是异步流程。设备需依次完成 Controller 初始化、Bluedroid 初始化、
A2DP Profile 初始化、BLE GATT 属性表注册与广播启动，才会显示“遥控广播中”。
原版 5 秒超时可能在双模初始化期间过早重启；现已放宽为 15 秒，
并在设置页将“启动中”改为更具体的“启动·初始化控制器 / 使能Host /
初始化A2DP / 创建GATT属性 / 配置BLE广播”等阶段。
真正失败时还会显示“重试·启动步骤 0x错误码”，而不是仅有“启动中”。

可运行串口监视并复现一次：

```powershell
pio device monitor -e fakepod_s31
```

请保留以 `BLE_START`、`BT_AUDIO`、`BLE服务`、`BLE广播`、
`蓝牙控制器`、`Bluedroid`、`GATT`、`BLE启动15秒` 开头/包含这些关键词的日志。
如果有连续 15 秒未出现 `BLE广播已启动`，请反馈最后显示的
`BLE_START step`，以及附近的第一条 WARN/ERROR；据此可判断是
控制器内存不足、A2DP Profile 初始化异常，还是 GATT 广播回调没有完成。
此阶段改动已提交，但没有实机连接测试，不能视为已确认根因。

### 修复“蓝牙音频发射：不支持”（2026-11-21）

原因是先前在 `main` 上生成的 `sdkconfig.fakepod_s31` 中只启用了 BLE，
切换到实验分支之后，PlatformIO 仍然复用了这个持久化配置：
即使新增了 `sdkconfig.bluetooth.defaults`，配置也不会自动被覆盖。
这样 Android BLE 仍能正常“广播中”，但 A2DP Source 宏被编译为关闭，
UI 就会显示“不支持”。

本分支已将 `platformio.ini` 的 sdkconfig 文件切换为 **`sdkconfig.fakepod_s31_bt_dma`**。
第一次重新构建会从 `sdkconfig.defaults;sdkconfig.bluetooth.defaults` 生成新的配置，
不触碰主线原有的 `sdkconfig.fakepod_s31`，因此切回 `main` 也不影响原设置。
同时增加编译期强制检查，若 Classic / A2DP / 内置 SBC 没有生效，编译会报错，
不会再默默烧录一个显示“不支持”的固件。

更新与编译命令（Windows PowerShell）：

```powershell
git switch feat/a2dp-source-ble-remote
git pull --ff-only
pio run -e fakepod_s31 -t clean
pio run -e fakepod_s31
```

确认实际 SDK 配置：

```powershell
Select-String -Path .\sdkconfig.fakepod_s31_bt_dma -Pattern '^(CONFIG_BT_(BLUEDROID_ENABLED|CLASSIC_ENABLED|A2DP_ENABLE|BLE_ENABLED|GATTS_ENABLE)=y|# CONFIG_BT_A2DP_USE_EXTERNAL_CODEC is not set)'
```

**正确结果应同时包含** `CONFIG_BT_CLASSIC_ENABLED=y`、`CONFIG_BT_A2DP_ENABLE=y`，
且 external codec 没有开启。编译并烧录更新后的固件后，连接设置中的
“蓝牙模式：遥控广播中”表示手机 GATT 等待连接，“蓝牙音频发射”应变为
“点击搜索耳机”；如果卡在 “需先开启蓝牙”，再检查 `BT_AUDIO` 启动日志。

在 `feat/a2dp-source-ble-remote` 分支上，蓝牙 Host 由 `ble_remote_service` **唯一初始化/停止**；
A2DP Source 与原手机 GATT 遥控共用 Bluedroid。音源解码后从 AudioTask 的 PCM
旁路复制到独立 PSRAM 环形缓存；蓝牙回调取 S16 立体声数据交给 ESP-IDF 的
**内置 SBC 编码器**。已有的 I²S → CS43131 有线 DAC 播放链不改变。

### 使用步骤

1. 「设置 → 连接 → 蓝牙模式」选择 **手机遥控**，保持 BLE GATT 广播；手机仍可连接并控制播放。
2. 让蓝牙耳机/蓝牙音箱进入**传统蓝牙配对/可发现模式**。
3. 点击「蓝牙音频发射」一次，发起约 5 秒扫描；再次点击连接搜索到的**信号最强**音频设备。设备名称显示在同一行。
4. 成功连接并协商 SBC 采样率后，音乐开始同时由本机有线 DAC 和蓝牙发送；再点击可断开蓝牙耳机（手机 BLE 继续连接）。
5. 关闭「蓝牙模式」则会同时关闭 BLE GATT 和 Classic A2DP，切换 Wi-Fi 时也如此。

### 已实现及限制

- 手机 **BLE GATT 遥控与 A2DP Source 可同时连接**；这不等于当前 HID 手柄主机也已恢复。
- A2DP 使用经典蓝牙 SBC，**不是**通过 BLE Audio / LE Audio 发声；发射默认按 16-bit
  PCM 重采样至耳机协商的 44.1/48 kHz（支持其余 SBC 协商档），96 kHz/24-bit
  FLAC 仍经主解码器输出，再在旁路中降采样。当前采用轻量采样保持（无高阶 SRC），
  音质与低延迟有待实机调校。
- PCM 旁路覆盖音乐、AVI MP3 和 NSF。当前是**有线+蓝牙镜像输出**，不是独占的蓝牙输出模式；
  DAC 用户音量会转换成蓝牙软件增益，蓝牙耳机本身也可调整音量。
- 32 KB PSRAM 蓝牙环形缓存，最大堆积限制约 120ms；蓝牙回调缺数据时补零，积压时只丢弃蓝牙
  旧帧，**不阻塞 I²S**。蓝牙耳机固有延时可能明显高于有线，视频口型同步尚未校准。
- 目前扫描结果自动保留信号最强的 **Rendering** 类设备；**没有设备列表**。如搜索到错误的
  音箱，可将目标耳机靠近设备并重试。蓝牙自动重连、记忆设备、AVRCP 耳机按键均未实现。
- 保留原有 Wi-Fi 与蓝牙的运行期互斥策略，所以 **NAS Wi-Fi 流播放不能同时使用 A2DP**。
- 由于本分支需新增双模蓝牙内存开销，首次测试应同时观察 internal RAM、PSRAM、
  `BT_AUDIO` 状态及 BLE 配对通知。仅提交代码不代表已通过实机 SBC/共存测试。

### 编译与诊断

```powershell
git fetch origin
git switch feat/a2dp-source-ble-remote
pio run -e fakepod_s31
```

观察日志关键字 `BT_AUDIO`：`A2DP profile ready`、`A2DP scan finished`、
`A2DP connection state=2`、`A2DP negotiated PCM 44100Hz/48000Hz`、
`A2DP audio STARTED`。真机回归还应检查：手机 GATT 在耳机连接前后不断线、切歌/暂停/SEEK、
耳机断开后仍能正常从有线 DAC 播放，以及长时间播放是否出现 PCM 欠载或大量丢帧。

---

## 快速开始

### 环境准备

- Python 3.10+
- PlatformIO Core（建议 6.2.0 及以上，避免多版本共存）
- Git
- 工具链由 PlatformIO 自动安装（`toolchain-riscv32-esp@15.2.0+20251204`）

> 本机单独安装的 ESP-IDF 5.5 不支持 `esp32s31`，必须通过本工程的 PlatformIO 环境构建。

### 编译

```powershell
pio run -e fakepod_s31
```

产物位于 `.pio/build/fakepod_s31/firmware.bin`。

### 烧录与监视

```powershell
pio run -e fakepod_s31 -t upload
pio device monitor
```

- 串口为 USB-Serial-JTAG，端口号随插拔变化，以 `pio device list` 为准（当前验证为 `COM3`）。
- 上传速率 `921600`，监视速率 `115200`。
- 打开监视器时的 DTR/RTS 电平可能触发一次复位，属正常现象。

---

## 构建环境

| 环境 | 用途 |
|---|---|
| `fakepod_s31` | 唯一环境，正式固件，`APP_DIAG_PROFILE=0`（RELEASE） |

关键配置：

- `platform = symlink://./platform/espressif32_s31`：本地补丁平台，在 `platform.py`、`builder/main.py`、`builder/frameworks/espidf.py` 中补充 `esp32s31` 与 RISC-V 工具链分支。
- **必须使用 `symlink://`**。`file://` 会被 PlatformIO 按内容哈希缓存旧副本，导致补丁不生效。
- `board_build.esp-idf.sdkconfig_path = sdkconfig.fakepod_s31`（由 `sdkconfig.defaults;sdkconfig.ble.defaults` 生成）。
- `sdkconfig.defaults` 关键项：320MHz、DIO/80MHz/16MB、16MB Octal PSRAM（200MHz）、TinyUSB MSC+CDC；蓝牙配置放在 **已入库** 的 `sdkconfig.bluetooth.defaults`：Bluedroid、BLE GATT Server、Classic A2DP Source/SBC。
- LVGL 使用内置 TLSF，128KB pool 强制落在 PSRAM（见 [src/CMakeLists.txt](src/CMakeLists.txt)）。
- `esp_lcd_panel_rgb.c` 单独使用 `-O0`，规避 GCC 在 IRA 阶段的问题；本项目实际走 QSPI CO5300，不使用 RGB 数据路径。
- GCC 15 对带默认成员初始值的结构体 `memset` 产生新告警，已用 `-Wno-error=class-memaccess` 降级。

分区表（[partitions.csv](partitions.csv)）：

| 分区 | 类型 | 偏移 | 大小 |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 20K |
| app0 | app/ota_0 | 0x10000 | 6M |
| fontcache | data/0x40 | 0x610000 | 4M |
| spiffs | data/spiffs | 0xa10000 | 6016K |
| coredump | data/coredump | 0xff0000 | 64K |

---

## 版本基线

| 组件 | 版本 | 固定方式 |
|---|---|---|
| PlatformIO 平台 | 本地副本 `esp32s31 补丁版` | `platform = symlink://./platform/espressif32_s31`，平台目录需随工程入库 |
| ESP-IDF | `6.1.0` | `platformio/framework-espidf@4.60100.0` |
| RISC-V GCC | `15.2.0+20251204` | `platform_packages` |
| 托管组件（LVGL 9.2.2、esp_extractor、esp_tinyusb 3.0.1 等） | 见 [dependencies.lock](dependencies.lock) | 锁文件入库 |
| PlatformIO Core | 建议 `6.2.0` 及以上 | 文档约定 |
| Python | `3.10+` | 文档约定 |

说明：

- `managed_components/` 被 `.gitignore` 忽略，组件由 `dependencies.lock` 复现，不要手工改动其中的版本。
- PlatformIO Core 只保留一个安装：多版本共存会触发 `Obsolete PIO Core` 警告，并可能在 `platforms.lock` 上出现 `PermissionError`。
- `sdkconfig.*` 生成物不入库；`sdkconfig.defaults` 与 `sdkconfig.bluetooth.defaults` 已明确加入 Git，不需手动补齐。

---

## 相对原工程的移植改动

1. **板级定义**：`board_pins.h` 按 S31 硬件重写；新增 `FAKEPOD_LCD_VCI_EN`、I²C1、AXP2101 / PCF85063 地址、PMU INT、PDM 引脚；电源键、电池 ADC、IMU INT 标记为无独立 GPIO。
2. **电源管理**：新增 [src/drivers/power/axp2101.cpp](src/drivers/power/axp2101.cpp)：芯片 ID 校验、电池电压 ADC、电量计、PWRKEY 短按/长按中断、清中断。
3. **电源键服务**：`power_service.cpp` 弃用 GPIO 电平状态机，改为轮询 AXP2101 事件；短按沿用“上一曲/音量+”，长按沿用“音频安全收尾 + NVS flush”，并保留上电保护窗口。
4. **电池服务**：采样源由 GPIO ADC 改为 AXP2101 电压/电量，保留 2 秒采样、EMA 滤波与低电记录逻辑。
5. **触摸与 IMU**：I²C0 共用总线；`FAKEPOD_IMU_INT1 = -1` 时自动改为 50ms 轮询 Tap 状态。
6. **DAC**：CS43131 改用 I²C1（100kHz 起步），I²S 输出切到 `I2S_NUM_1`。
7. **显示**：新增 VCI_EN（GPIO44）上电时序（拉低 10ms → 拉高 → 等 120ms），QSPI 时钟提升到 80MHz。
8. **IDF 6.1 兼容**：NAS 配网从 mbedTLS legacy GCM/PKCS5 迁移到 PSA Crypto；`esp_tinyusb` 升级到 `3.0.1`；esp-dsp 卡尔曼模块补充 `<cmath>`；GCC 15 告警降级。
9. **BLE 配置**：S31 打开 Central + GATT Client（esp_hid 主机侧需要），并移除 S31 不存在的 `BT_CTRL_BLE_MAX_ACT`。
10. **BoundedSPI 放开 IDF 6.1**：原实现将直连 QSPI 的 Panel IO 私有前缀复用限定在 ESP-IDF 5.5.3，S31 上会静默回退并导致上滑圆环、封面直写、视频直出全部失效。已核对 6.1 的 `esp_lcd_panel_io_spi_t` 前缀字段顺序一致，纳入白名单；运行时仍由字段复核保证不匹配时自动回退。
11. **硬件 JPEG 解码**：新增 `drivers/jpeg/jpeg_hw_decoder`，封装 S31 片内 JPEG 编解码引擎（`esp_driver_jpeg`），替换封面、图片 APP、视频 MJPEG 帧三条链路的软件解码，并保留 `esp_new_jpeg` 回退。要点：仅支持 baseline（progressive 自动回退）、宽高乘积需能被 8 整除、输出按 MCU 对齐（460×460 填充到 464×464）后由封装裁切、硬件引擎单实例加锁互斥、对齐缓冲按最大帧常驻 PSRAM。实测首帧 1400×1400 封面解码 33.6ms。
12. **PPA 封面压暗**：新增 `drivers/ppa/ppa_ops`，用 PPA blend（背景 RGB565 封面 + 前景固定黑色 A8 蒙版，alpha≈151/255 对应 13/32 压暗）替代逐像素压暗循环，失败自动回退软件实现。注意 PPA 缩放步进只有 1/16，无法替代封面精确的 460×460 中心裁切重采样，因此仅用于无损的混色。

---

## 完整固件打包与离线烧录

1. 先完成一次工程编译，确保 `.pio/build/fakepod_s31/` 内有 bootloader、分区表与应用镜像。
2. 运行 [tools/firmware/merge_fakepod_full.bat](tools/firmware/merge_fakepod_full.bat)，合并为单一完整固件（脚本默认环境已改为 `fakepod_s31`）。
3. 使用乐鑫 `flash_download_tool` 离线烧录：

| 项目 | 设置 |
|---|---|
| 文件 | 合并后的完整固件（`FakePod_Full.bin`） |
| 地址 | `0x0000` |
| SPI MODE | DIO |
| SPI SPEED | 80MHz |
| Flash Size | 16MB |

> `*.bin` 已被 `.gitignore` 忽略，发布产物不入库。

---

## TF 卡目录约定

| 内容 | 路径 | 说明 |
|---|---|---|
| 音乐 | `/MUSIC` | 最多二级文件夹；MP3 / FLAC / WAV / Opus |
| 视频 | `/video` | AVI，可放子文件夹；建议同时放置同名 `.fvix` 索引 |
| 电子书 | `/txt` | UTF-8 编码，可使用子文件夹 |
| 电子音流 | `/synth` | NSF；NSFE 可识别但暂不能正常播放 |
| 图片 | SD 卡任意位置 | 图片浏览会扫描 `/sdcard` 下的 JPG / PNG |

标签与封面可通过 MusicTag 写入，歌词文件建议用 LDDC 下载。也可在设备内通过「设置 → 连接 → TF 卡文件管理」用电脑直接整理。

---

## 视频子系统

数据流：

```text
AVI 文件 → esp_extractor（仅保留 AVI）→ MJPEG 帧 → RGB565
        → BoundedSPI Presenter（QSPI 直发 CO5300，独占 SPI2 DMA，绕过 LVGL）
```

- 视频播放期间 LVGL 界面整体隐藏，UI 元素由 Presenter 的 producer 回调直接合成到 RGB565 缓冲。因此 BoundedSPI session 与 LVGL flush 互斥，一次播放结束后必须等 pipeline 完全退出才能开启下一个 session。
- 音频轨由 AVI 内 MP3 流经 `avi_mp3_audio_source` 接入统一音频服务；视频暂停时音频与画面同步暂停。
- 手势：右滑快进、左滑后退（速度由起手位置在上下半屏决定）、下滑返回列表、原地双击暂停/继续，详见 [视频播放操作说明](视频播放操作说明.md)。

### FVIX 侧车索引

- 使用 [tools/video/FFmpeg_MJPEG_GUI_v4](tools/video/FFmpeg_MJPEG_GUI_v4/README.md) 转换视频时，会为每个 AVI 自动生成同名 `.fvix` 索引，需与 AVI 一起拷入设备。
- 索引每 2 秒记录一个定位点，文件很小；没有 `.fvix` 的视频仍可正常播放，但**不能 seek**。
- 定位为粗精度：落点是不晚于目标时间的下一帧，实测与滑动估算可能相差 1~2 秒。
- 索引格式为 FVIX V2，包含来源文件校验，拒绝与当前视频不匹配的索引。

---

## 工具

| 用途 | 启动位置 | 说明 |
|---|---|---|
| 转换视频 | [tools/video/FFmpeg_MJPEG_GUI_v4/start.bat](tools/video/FFmpeg_MJPEG_GUI_v4/start.bat) | 批量转 460 宽 MJPEG AVI，并生成 `.fvix` |
| 管理 NAS 曲库 | [tools/nas/启动NAS曲库管理器.bat](tools/nas/启动NAS曲库管理器.bat) | 见 [NAS 工具说明](tools/nas/FakePod_NAS_GUI_使用说明.md) |
| 合并完整固件 | [tools/firmware/merge_fakepod_full.bat](tools/firmware/merge_fakepod_full.bat) | 需先完成工程编译 |

工具总览见 [tools/README.md](tools/README.md)。

---

## 主机回归测试

`tests/host/` 下是维护源码时使用的主机侧脚本，不在 ESP32 上运行，也不参与固件编译：

```powershell
python tests/host/test_audio_perf_math.py    # 需要 g++
python tests/host/test_nas_rebuffer.py
python tests/host/test_library_row_recycling.py
```

这些脚本使用主机替身，不能替代完整固件构建与真机验收。清单见 [tests/host/README.md](tests/host/README.md)。

---

## 开发约定

- **提交信息使用中文**，遵循 Conventional Commits：`<type>(<scope>): <中文描述>`，scope 用小写英文模块名（如 `video`、`display`、`audio`）。规则见 [.trae/rules/git-commit-message.md](.trae/rules/git-commit-message.md)。
- **版本号只在 [src/system/app_build_info.h](src/system/app_build_info.h) 维护**，运行期由 Boot 层统一输出一次。
- **改动视频 / 音频 / 显示通路时同步更新文档**：操作行为变化更新 `FakePod_使用操作说明.md` 或 `视频播放操作说明.md`。
- 诊断开关统一走 `APP_DIAG_PROFILE` 与 `APP_DIAG_*` 宏，正式固件保持 RELEASE。
- **修改本地补丁平台后**，确认 `platformio.ini` 仍使用 `symlink://`，并在 `pio run -t clean` 后重新编译验证。

---

## 常见问题

| 现象 | 原因与处理 |
|---|---|
| 蓝牙发射不可用（A2DP 未初始化） | 检查 `sdkconfig.bluetooth.defaults` 是否生效，确认 `CONFIG_BT_CLASSIC_ENABLED=y`、`CONFIG_BT_A2DP_ENABLE=y` 且未使用 external codec。清理旧 `sdkconfig.fakepod_s31` 后重新构建。 |
| 编译出现 `KeyError: 'toolchain-xtensa-esp-elf'` 或调用 xtensa 工具链 | 本地平台补丁未生效。确认 `platform = symlink://./platform/espressif32_s31`，并清理 `.pio` 缓存后重建。 |
| `Tool doesn't match supported version ['esp-15.2.0_20251204']` | ESP-IDF 6.1 要求 `toolchain-riscv32-esp@15.2.0+20251204`，检查 `platform_packages` 是否被覆盖。 |
| `PermissionError: platforms.lock` | 系统存在多个 PlatformIO Core（提示 obsolete PIO Core），或对平台目录无写权限。清理多余安装、统一 Core 版本后重试。 |
| TF 卡初始化失败 `sdmmc_card_init failed (0x107)` | 未插卡属正常（硬件无卡检测脚）；已插卡仍失败时检查卡座供电、CMD/D0–D3 上拉，或暂时降低 SDMMC 时钟验证时序。 |
| 启动日志 `MMU page size mismatch (0x8000 vs 0x10000)` | 提示级信息，不影响运行；如需消除，可在后续统一 bootloader 与应用的 MMU 页大小配置。 |
| 编译提示 app 分区接近上限 | 已改为单槽 app0 6MB（占约 61.5%），保留约 2.4MB 余量；不再支持 OTA 双槽，升级走整片串口烧录。 |
| 视频无法快进/快退 | 缺少同名 `.fvix` 索引，用视频转换工具重新生成。 |
| 改动设置后未生效 | 「设置 → 应用 → 音乐播放器」中的部分选项需**离开整个设置 APP** 后才正式保存生效。 |
| 烧录成功但串口反复复位 | USB-Serial-JTAG 在打开/关闭监视器时可能复位；确认端口号未变化，必要时重新上电后再连。 |

---

## 相关文档

- [hw_pod_nano_s31_v1_开发者硬件说明.md](hw_pod_nano_s31_v1_开发者硬件说明.md) —— S31 V1 量产版硬件说明（引脚、时序、电源）
- [FakePod_使用操作说明.md](FakePod_使用操作说明.md) —— 面向使用者的完整操作手册
- [视频播放操作说明.md](视频播放操作说明.md) —— 视频手势、SEEK 精度与片源要求
- [tools/README.md](tools/README.md) —— 电脑端工具入口
- [tools/video/FFmpeg_MJPEG_GUI_v4/README.md](tools/video/FFmpeg_MJPEG_GUI_v4/README.md) —— 视频转换与索引生成
- [tests/host/README.md](tests/host/README.md) —— 主机回归脚本清单
- [.trae/rules/git-commit-message.md](.trae/rules/git-commit-message.md) —— 提交信息规范

---

## 第三方组件

固件基于 ESP-IDF 6.1 构建，依赖并以 `managed_components/` 形式引入：LVGL 9.x、esp_extractor、esp_lv_decoder、esp_new_jpeg、libpng、esp_lcd_co5300、esp_lvgl_port、esp_audio_codec、esp_tinyusb 3.0.1、cmake_utilities，另有本地 vendored 的 ESP-DSP。各组件的许可证与版权声明见对应目录内的 `README.md` / `LICENSE`。
