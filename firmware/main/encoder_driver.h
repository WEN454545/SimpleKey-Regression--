/**
 * @file encoder_driver.h
 * @brief ESP32-S3 三模键盘 - 编码器驱
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 * 
 * EC11 旋转编码器驱
 * - A/B 相：GPIO13/14，四倍频解码
 * - 按键：GPIO21
 * - 使用 GPIO 中断 + 状态表解码
 */

#ifndef ENCODER_DRIVER_H
#define ENCODER_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

// ==================== 数据结构 ====================

// 编码器事件类
typedef enum {
    ENCODER_EVENT_ROTATION,     // 旋转事件
    ENCODER_EVENT_BUTTON,       // 按键事件
} encoder_event_type_t;

// 编码器事件结构体
typedef struct {
    encoder_event_type_t type;  // 事件类型
    int8_t delta;               // 旋转增量（单?格数?N 顺时?/ -N 逆时针）
    bool button_state;          // 按键状态（按下/释放
    int64_t timestamp;          // 时间戳（微秒
} encoder_event_t;

// ==================== 函数声明 ====================

/**
 * @brief 初始化编码器驱动
 * 
 * 配置 GPIO 引脚，安装中断服务，创建事件队列
 */
void encoder_init(void);

/**
 * @brief 获取编码器事件队列句
 * 
 * @return 编码器事件队列句? */
void *encoder_get_event_queue(void);

/**
 * @brief 获取编码器旋转计
 * 
 * @return 旋转计数（正=顺时针，?逆时针）
 */
int32_t encoder_get_count(void);

/**
 * @brief 重置编码器旋转计? */
void encoder_reset_count(void);

/**
 * @brief 获取按键状
 * 
 * @return true 按下，false 释放
 */
bool encoder_get_button_state(void);

#endif // ENCODER_DRIVER_H
