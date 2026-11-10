/**
 * @file ble_hid.h
 * @brief ESP32-S3 三模键盘 - BLE HID 设备
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 * 
 * 使用 NimBLE 实现 BLE HID 设备
 * - HID over GATT 服务
 * - 3 个配对槽位，独立虚拟 MAC
 * - 自动重连上次配对的设? */

#ifndef BLE_HID_H
#define BLE_HID_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// ==================== 数据结构 ====================

// BLE 配对槽位信息
typedef struct {
    bool paired;                // 是否已配
    char device_name[32];       // 设备名称
    uint8_t peer_addr[6];       // 对端地址
    uint8_t addr_type;          // 地址类型
} ble_slot_info_t;

// BLE 状
typedef enum {
    BLE_STATE_IDLE = 0,         // 空闲
    BLE_STATE_ADVERTISING,      // 广播
    BLE_STATE_CONNECTED,        // 已连
    BLE_STATE_PAIRING,          // 配对
} ble_state_t;

// ==================== 函数声明 ====================

/**
 * @brief 初始?BLE HID 设备
 * 
 * 初始?NimBLE，配?GATT 服务，开始广
 * 
 * @return esp_err_t ESP_OK 成功，其?失败
 */
esp_err_t ble_hid_init(void);

/**
 * @brief 反初始化 BLE HID 设备
 * 
 * 停止广播，断开连接，释放资
 * 
 * @return esp_err_t ESP_OK 成功，其?失败
 */
esp_err_t ble_hid_deinit(void);

/**
 * @brief 发送键盘报
 * 
 * @param report 键盘报告指针
 * @param len 报告长度
 * @return esp_err_t ESP_OK 成功，其?失败
 */
esp_err_t ble_hid_send_keyboard_report(const uint8_t *report, size_t len);

/**
 * @brief 发?Consumer Control 报告
 * 
 * @param report Consumer Control 报告指针
 * @param len 报告长度
 * @return esp_err_t ESP_OK 成功，其?失败
 */
esp_err_t ble_hid_send_consumer_report(const uint8_t *report, size_t len);

/**
 * @brief 发送调试监控事件（自定义监控服?0xFFE1 notify? *
 * 事件为紧凑二进制格式（单?? 字节，兼容默?ATT MTU 23）：
 *   [0x01, down, row, col, keycode]  按键事件，down: 1=按下 0=释放
 *   [0x02, delta]                    编码器顺时针旋转（音?
 *   [0x03, delta]                    编码器逆时针旋转（音量-
 *   [0x04] / [0x05] / [0x06]         编码器按?/ 释放 / 长按
 *   [0x07, mode]                     模式切换，mode: 0=usb 1=ble
 *   [0x08, profile]                  Profile 切换，profile: 1-3
 *
 * @param data 事件数据
 * @param len  数据长度（≤20
 * @return esp_err_t ESP_OK 成功，其?失败/未订? */
esp_err_t ble_hid_send_monitor(const uint8_t *data, size_t len);

/**
 * @brief 检?BLE HID 是否已连
 * 
 * @return true 已连接，false 未连? */
bool ble_hid_is_connected(void);

/**
 * @brief 获取 BLE 状
 * 
 * @return ble_state_t 当前状? */
ble_state_t ble_hid_get_state(void);

/**
 * @brief 发送按键事
 * 
 * @param key_code HID 键码
 * @param modifiers 修饰
 * @param pressed true 按下，false 释放
 * @return esp_err_t ESP_OK 成功，其?失败
 */
esp_err_t ble_hid_send_key(uint8_t key_code, uint8_t modifiers, bool pressed);

/**
 * @brief 发送音量控
 * 
 * @param volume_up true 音量+，false 音量-
 * @return esp_err_t ESP_OK 成功，其?失败
 */
esp_err_t ble_hid_send_volume(bool volume_up);

// ==================== 配对槽位管理 ====================

/**
 * @brief 获取槽位信息
 * 
 * @param slot 槽位号（0-2
 * @param info 槽位信息指针
 * @return esp_err_t ESP_OK 成功，其?失败
 */
esp_err_t ble_hid_get_slot_info(uint8_t slot, ble_slot_info_t *info);

/**
 * @brief 清除槽位配对
 * 
 * @param slot 槽位号（0-2
 * @return esp_err_t ESP_OK 成功，其?失败
 */
esp_err_t ble_hid_clear_slot(uint8_t slot);

/**
 * @brief 连接到指定槽
 * 
 * @param slot 槽位号（0-2
 * @return esp_err_t ESP_OK 成功，其?失败
 */
esp_err_t ble_hid_connect_slot(uint8_t slot);

/**
 * @brief 获取当前槽位
 * 
 * @return uint8_t 当前槽位号（0-2? */
uint8_t ble_hid_get_current_slot(void);

/**
 * @brief 设置当前槽位
 * 
 * @param slot 槽位号（0-2? */
void ble_hid_set_current_slot(uint8_t slot);

/**
 * @brief 清空所?BLE 配对绑定（NVS 持久化密钥）
 * @note  调试用：主机/设备单侧删除配对后无法重连时，执行后主机可重新配? */
void ble_hid_clear_bonds(void);

#endif // BLE_HID_H
