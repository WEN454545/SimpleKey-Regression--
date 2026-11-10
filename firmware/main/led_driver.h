/**
 * @file led_driver.h
 * @brief ESP32-S3 三模键盘 - LED 驱动
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 * 
 * 3 颗单色模?LED（低电平点亮
 * - USB 模式：白色（GPIO2
 * - BLE 模式：蓝色（GPIO15
 * - 2.4G 模式：绿色（GPIO16? */

#ifndef LED_DRIVER_H
#define LED_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

// ==================== 数据结构 ====================

// LED 状态
typedef enum {
    LED_MODE_OFF = 0,        // 熄灭
    LED_MODE_ON,             // 常亮
    LED_MODE_BLINK_SLOW,     // 慢闪 1Hz - 低电
    LED_MODE_BLINK_FAST,     // 快闪 5Hz - 配对中
    LED_MODE_BLINK_N,        // 快闪 N 次 - Profile 切换
} led_mode_t;

// LED 颜色（模式）
typedef enum {
    LED_COLOR_NONE = 0,      // 
    LED_COLOR_USB,           // 白色 - USB 模式
    LED_COLOR_BLE,           // 蓝色 - BLE 模式
    LED_COLOR_2G4,           // 绿色 - 2.4G 模式
} led_color_t;

// LED 事件类型
typedef enum {
    LED_EVENT_SET_MODE,      // 设置模式
    LED_EVENT_SET_COLOR,     // 设置颜色
    LED_EVENT_BLINK_N,       // 快闪 N 
    LED_EVENT_LOW_BATTERY,   // 低电指示
} led_event_type_t;

// LED 事件结构
typedef struct {
    led_event_type_t type;   // 事件类型
    led_color_t color;       // LED 颜色
    led_mode_t mode;         // LED 模式
    uint8_t blink_count;     // 快闪次数
} led_event_t;

// ==================== 函数声明 ====================

/**
 * @brief 初始化 LED 驱动
 * 
 * 配置 GPIO 引脚，创建 LED 任务
 */
void led_init(void);

/**
 * @brief 设置 LED 模式
 * 
 * @param color LED 颜色
 * @param mode LED 模式
 */
void led_set_mode(led_color_t color, led_mode_t mode);

/**
 * @brief LED 快闪 N 
 * 
 * @param color LED 颜色
 * @param count 快闪次数
 */
void led_blink_n(led_color_t color, uint8_t count);

/**
 * @brief 设置低电指示
 * 
 * @param enable true 开启低电指示，false 关闭
 */
void led_set_low_battery(bool enable);

/**
 * @brief 获取 LED 事件队列句柄
 * 
 * @return LED 事件队列句柄
 */
void *led_get_event_queue(void);

#endif // LED_DRIVER_H
