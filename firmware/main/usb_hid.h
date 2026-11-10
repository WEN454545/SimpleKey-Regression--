/**
 * @file usb_hid.h
 * @brief ESP32-S3 三模键盘 - USB HID 设备
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 * 
 * 使用 TinyUSB 实现 USB HID 设备
 * - 键盘 HID Report：6 键无冲 + 修饰键
 * - Consumer Control：Volume Up/Down
 */

#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// ==================== 数据结构 ====================

// HID 报告类型（自定义，避免与 TinyUSB 冲突）
typedef enum {
    SK_HID_REPORT_KEYBOARD,     // 键盘报告
    SK_HID_REPORT_CONSUMER,     // Consumer Control 报告
} sk_hid_report_type_t;

// 键盘报告结构体（6 键无冲）
typedef struct {
    uint8_t modifiers;          // 修饰键位掩码
    uint8_t reserved;           // 保留
    uint8_t keys[6];            // 键码
} __attribute__((packed)) usb_hid_keyboard_report_t;

// Consumer Control 报告结构体（位域：bit0=Vol+ bit1=Vol- bit2=Mute，见报告描述符）
typedef struct {
    uint8_t buttons;            // 按键位域
} __attribute__((packed)) usb_hid_consumer_report_t;

// ==================== 修饰键定义 ====================
#define HID_MODIFIER_LEFT_CTRL   0x01
#define HID_MODIFIER_LEFT_SHIFT  0x02
#define HID_MODIFIER_LEFT_ALT    0x04
#define HID_MODIFIER_LEFT_GUI    0x08
#define HID_MODIFIER_RIGHT_CTRL  0x10
#define HID_MODIFIER_RIGHT_SHIFT 0x20
#define HID_MODIFIER_RIGHT_ALT   0x40
#define HID_MODIFIER_RIGHT_GUI   0x80

// ==================== Consumer Control 使用码 ====================
// Consumer Control 位域掩码（与报告描述符 bit0~bit7 对应，P6 扩展为 8 位）
#define HID_CONSUMER_VOLUME_UP      0x01
#define HID_CONSUMER_VOLUME_DOWN    0x02
#define HID_CONSUMER_MUTE           0x04
#define HID_CONSUMER_PLAY_PAUSE     0x08
#define HID_CONSUMER_SCAN_NEXT      0x10
#define HID_CONSUMER_SCAN_PREV      0x20
#define HID_CONSUMER_BRIGHT_UP      0x40
#define HID_CONSUMER_BRIGHT_DOWN    0x80

// ==================== 函数声明 ====================

/**
 * @brief 初始化 USB HID 设备
 * 
 * 配置 USB PHY，安装 TinyUSB 驱动
 * 
 * @return esp_err_t ESP_OK 成功，其他 失败
 */
esp_err_t usb_hid_init(void);

/**
 * @brief 反初始化 USB HID 设备
 * 
 * 卸载 TinyUSB 驱动
 * 
 * @return esp_err_t ESP_OK 成功，其他 失败
 */
esp_err_t usb_hid_deinit(void);

/**
 * @brief 发送键盘报告
 * 
 * @param report 键盘报告指针
 * @param len 报告长度
 * @return esp_err_t ESP_OK 成功，其他 失败
 */
esp_err_t usb_hid_send_keyboard_report(const usb_hid_keyboard_report_t *report);

/**
 * @brief 发送 Consumer Control 报告
 * 
 * @param report Consumer Control 报告指针
 * @param len 报告长度
 * @return esp_err_t ESP_OK 成功，其他 失败
 */
esp_err_t usb_hid_send_consumer_report(const usb_hid_consumer_report_t *report);

/**
 * @brief 检查 USB HID 是否已连接
 * 
 * @return true 已连接，false 未连接
 */
bool usb_hid_is_connected(void);

/**
 * @brief 发送按键事件
 * 
 * @param key_code HID 键码
 * @param modifiers 修饰键
 * @param pressed true 按下，false 释放
 * @return esp_err_t ESP_OK 成功，其他 失败
 */
esp_err_t usb_hid_send_key(uint8_t key_code, uint8_t modifiers, bool pressed);

/**
 * @brief 发送音量控制
 * 
 * @param volume_up true 音量+，false 音量-
 * @return esp_err_t ESP_OK 成功，其他 失败
 */
esp_err_t usb_hid_send_volume(bool volume_up);

#endif // USB_HID_H
