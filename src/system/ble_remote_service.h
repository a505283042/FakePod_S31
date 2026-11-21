#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "device_settings.h"

// R46.0：BLE Foundation 负责蓝牙生命周期、广播和连接状态。
// 手机遥控通过本服务的加密 GATT 写入入口调用现有 Player 控制，不让 APP 直接操作蓝牙协议栈。
enum class BleRemoteState : uint8_t {
    Disabled = 0,
    Starting,
    Advertising,
    Scanning,
    ControllerConnecting,
    ControllerConnected,
    Connected,
    Stopping,
    RetryWait,
    Unsupported,
};

struct BleRemoteSnapshot {
    bool ready = false;
    bool desired_enabled = false;
    bool stack_initialized = false;
    bool connected = false;
    BleRemoteState state = BleRemoteState::Disabled;
    esp_err_t last_error = ESP_OK;
    DeviceBluetoothAudioRole audio_role = DeviceBluetoothAudioRole::Transmit;
    DeviceBluetoothAudioRole audio_role_target = DeviceBluetoothAudioRole::Transmit;
    bool audio_role_switching = false;
    esp_err_t audio_role_last_error = ESP_OK;
    uint8_t startup_stage = 0; // 0=准备, 1-10=控制器/Host/A2DP/GATT/广播启动阶段
};

// 轻量初始化；不会在用户关闭 BLE 时启动蓝牙栈。
esp_err_t ble_remote_service_init();

// 只更新“用户意图”，真正的蓝牙栈启停由 update() 异步执行，避免阻塞 LVGL/loopTask。
void ble_remote_service_set_enabled(bool enabled);
void ble_remote_service_set_mode(DeviceBluetoothMode mode);
void ble_remote_service_update();

// Switch Classic A2DP Source/Sink without restarting the BLE GATT remote.
// Returns false if a Bluetooth stack transition is busy or unavailable.
// Successful switches are stored in NVS by the transition worker.
bool ble_remote_service_request_audio_role(DeviceBluetoothAudioRole role);
// Restart only the active phone A2DP Sink after changing its AAC/SBC
// advertising preference. Existing Transmit/Receive selection is unchanged.
bool ble_remote_service_restart_audio_sink();

// 第一阶段 HID Host 关闭，手柄（Controller）模式不可用；UI 据此跳过该选项。
bool ble_remote_service_controller_supported();

bool ble_remote_service_get_snapshot(BleRemoteSnapshot *out_snapshot);
const char *ble_remote_service_state_name(BleRemoteState state);
const char *ble_remote_service_startup_stage_name(uint8_t startup_stage);
