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
| 连接 | USB TinyUSB MSC + CDC：电脑直接管理 TF 卡；BLE 双角色（手机 GATT 外设 / 手柄 HID 主机，运行期互斥） |
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
├── sdkconfig.ble.defaults      # BLE 相关 sdkconfig 片段
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
- `sdkconfig.defaults` 关键项：320MHz、DIO/80MHz/16MB、16MB Octal PSRAM（200MHz）、`CONFIG_SPIRAM_USE_MALLOC`、AVI 之外的其他解封装器全部关闭、TinyUSB MSC+CDC、NimBLE 双角色、BLE Central + GATT Client（esp_hid 主机侧需要）。
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
- `sdkconfig.*` 生成物不入库（`sdkconfig.defaults` 除外），由配置文件重新生成；`sdkconfig.ble.defaults` 也受该规则忽略，首次克隆需自行补齐。

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
| CMake 报 `SDKCONFIG_DEFAULTS 'sdkconfig.ble.defaults' does not exist` | 该文件被 `.gitignore` 的 `sdkconfig.*` 规则忽略，不在版本库中。首次克隆需自行补齐该文件，或删除 `board_build.cmake_extra_args` 中的对应项后重新配置。 |
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
