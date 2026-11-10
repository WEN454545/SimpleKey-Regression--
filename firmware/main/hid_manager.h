/**
 * @file hid_manager.h
 * @brief ESP32-S3 三模键盘 - HID 管理器
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 * 
 * 统一管理 USB 和 BLE HID 设备
 * - 延迟初始化：同一时间只初始化一个 HID 接口
 * - 动态切换：支持运行时切换模式
 * - 错误恢复：初始化失败时回退到旧模式
 */

#ifndef HID_MANAGER_H
#define HID_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// ==================== 数据结构 ====================

// HID 模式
typedef enum {
    HID_MODE_NONE = 0,      // 无
    HID_MODE_USB,           // USB HID
    HID_MODE_BLE,           // BLE HID
    HID_MODE_WIFI,          // WiFi 热点模式（P5 方案 A：SoftAP+TCP，输入由上位机 SendInput 注入）
} hid_mode_t;

// HID 管理器状态
typedef struct {
    hid_mode_t current_mode;        // 当前模式
    hid_mode_t previous_mode;       // 上一个模式
    bool usb_initialized;           // USB 是否已初始化
    bool ble_initialized;           // BLE 是否已初始化
    bool switching;                 // 正在切换中
} hid_manager_state_t;

// ==================== 函数声明 ====================

/**
 * @brief 初始化 HID 管理器
 * 
 * 根据配置初始化默认的 HID 模式
 */
void hid_manager_init(void);

/**
 * @brief 切换 HID 模式
 * 
 * 反初始化当前模式，初始化新模式
 * 如果新模式初始化失败，回退到旧模式
 * 
 * @param new_mode 新的 HID 模式
 * @return esp_err_t ESP_OK 成功，其他 失败
 */
esp_err_t hid_manager_switch_mode(hid_mode_t new_mode);

/**
 * @brief 获取当前 HID 模式
 * 
 * @return hid_mode_t 当前模式
 */
hid_mode_t hid_manager_get_mode(void);

/**
 * @brief 获取 HID 管理器状态
 * 
 * @param state 状态指针
 */
void hid_manager_get_state(hid_manager_state_t *state);

/**
 * @brief 发送按键报告
 * 
 * 根据当前模式自动选择 USB 或 BLE
 * 
 * @param key_code HID 键码
 * @param modifiers 修饰键
 * @param pressed true 按下，false 释放
 * @return esp_err_t ESP_OK 成功，其他 失败
 */
esp_err_t hid_manager_send_key(uint8_t key_code, uint8_t modifiers, bool pressed);

/**
 * @brief 发送音量控制
 *
 * @param volume_up true 音量+，false 音量-
 * @return esp_err_t ESP_OK 成功，其他 失败
 */
esp_err_t hid_manager_send_volume(bool volume_up);

/**
 * @brief 发送 Consumer 功能按下/释放（P6：键位值域 0xC0~0xCF 的统一出口）
 *
 * func_idx = 键码 - KC_CONSUMER_BASE（0~7，与 Consumer 报告位域 bit 序一致）。
 * 内部维护按住位域，按下置位、释放清位，有变化才发报告。
 * WiFi 模式不发本地 HID（事件由调用方经 [EVT] 行推送，上位机 SendInput 注入）。
 *
 * @param func_idx 功能序号 0~7
 * @param pressed true 按下，false 释放
 * @return esp_err_t ESP_OK 成功，其他 失败/参数无效
 */
esp_err_t hid_manager_send_consumer(uint8_t func_idx, bool pressed);

/**
 * @brief 发送一次 Consumer 功能脉冲（按下→20ms→释放）
 *
 * 用于旋钮旋转等无保持语义的场景（对齐原 send_volume 的脉冲行为）
 *
 * @param func_idx 功能序号 0~7
 * @return esp_err_t ESP_OK 成功，其他 失败
 */
esp_err_t hid_manager_send_consumer_pulse(uint8_t func_idx);

/**
 * @brief 检查 HID 是否已连接
 * 
 * @return true 已连接，false 未连接
 */
bool hid_manager_is_connected(void);

/**
 * @brief 获取模式名称
 * 
 * @param mode HID 模式
 * @return const char* 模式名称
 */
const char *hid_manager_get_mode_name(hid_mode_t mode);

#endif // HID_MANAGER_H
