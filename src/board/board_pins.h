#pragma once

// ============================================================
// FakePod S31 硬件引脚定义（ESP32-S31 / 硬件版本 V1 量产版）
// 参考：hw_pod_nano_s31_v1_开发者硬件说明.md
// 所有编号均为 ESP32-S31 的 GPIO 编号。
// ============================================================


// ============================================================
// I2C0 总线（触摸 / 电源管理 / IMU / RTC）
// ============================================================

#define FAKEPOD_I2C_SCL        47
#define FAKEPOD_I2C_SDA        48

// ============================================================
// I2C1 总线（CS43131 DAC 控制）
// ============================================================

#define FAKEPOD_I2C1_SCL       54
#define FAKEPOD_I2C1_SDA       55


// ============================================================
// I2C 设备地址（7-bit，无需左移）
// ============================================================

#define FAKEPOD_ADDR_CST820    0x15
#define FAKEPOD_ADDR_QMI8658   0x6B
#define FAKEPOD_ADDR_CS43131   0x30
#define FAKEPOD_ADDR_AXP2101   0x34
#define FAKEPOD_ADDR_PCF85063  0x51


// ============================================================
// CST820 触摸控制
// ============================================================

#define FAKEPOD_TOUCH_INT      45
#define FAKEPOD_TOUCH_RST      46


// ============================================================
// CO5300 AMOLED
// 460 × 460 / QSPI
// ============================================================

// AMOLED 电源使能（高电平使能）
#define FAKEPOD_LCD_VCI_EN     44

#define FAKEPOD_LCD_TE         35
#define FAKEPOD_LCD_RST        36
#define FAKEPOD_LCD_CS         37
#define FAKEPOD_LCD_CLK        38

#define FAKEPOD_LCD_D0         39
#define FAKEPOD_LCD_D1         40
#define FAKEPOD_LCD_D2         42
#define FAKEPOD_LCD_D3         43

#define FAKEPOD_LCD_WIDTH      460
#define FAKEPOD_LCD_HEIGHT     460

// 这块屏实际显存窗口从 X=10 开始
#define FAKEPOD_LCD_X_OFFSET   10
#define FAKEPOD_LCD_Y_OFFSET   0


// ============================================================
// CS43131 音频 DAC
// DAC 使用板载 24.576MHz 晶振，MCLK 不占用主控 GPIO。
// 控制接口在 I2C1（地址 0x30），音频数据走 I2S1。
// ============================================================

#define FAKEPOD_I2S_LRCK       51
#define FAKEPOD_I2S_DOUT       50
#define FAKEPOD_I2S_BCLK       52
#define FAKEPOD_DAC_RST        53

// ============================================================
// PDM 麦克风（I2S0 的 PDM 接收）
// ============================================================

#define FAKEPOD_PDM_DATA       2
#define FAKEPOD_PDM_CLK        6

// ============================================================
// 电源管理 AXP2101
// - 电源按键直接接 AXP2101 PWRKEY，无独立主控 GPIO
// - PMU 中断 GPIO49（下降沿触发后读取中断状态）
// - 电池电压 / 电量由 AXP2101 采集，无外部 ADC 分压
// ============================================================

#define FAKEPOD_PMU_INT        49
#define FAKEPOD_POWER_KEY      -1

// ============================================================
// 下方独立按键 K1 / GPIO61
// 低电平按下，内部上拉。
// ============================================================

#define FAKEPOD_AUX_KEY        61

// ============================================================
// 电池采集：由 AXP2101 提供，不使用 GPIO ADC
// ============================================================

#define FAKEPOD_BATTERY_ADC    -1

// ============================================================
// QMI8658C IMU：I2C0，地址 0x6B
// 本文档未提供 INT 引脚连接，使用寄存器轮询。
// ============================================================

#define FAKEPOD_IMU_INT1       -1

// ============================================================
// TF 卡 - SDMMC 4-bit
// 工作时钟参考 40MHz，不使用 UHS 模式。
// ============================================================

#define FAKEPOD_SD_CLK         24
#define FAKEPOD_SD_CMD         25

#define FAKEPOD_SD_D0          20
#define FAKEPOD_SD_D1          21
#define FAKEPOD_SD_D2          22
#define FAKEPOD_SD_D3          23
