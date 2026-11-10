/**
 * @file matrix_driver.h
 * @brief ESP32-S3 三模键盘 - 矩阵扫描驱动
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 * 
 * 4×5 矩阵扫描?9 ?+ 1 ?2u 空格
 * 使用硬件定时?ISR 驱动扫描，软件防抖（20ms? */

#ifndef MATRIX_DRIVER_H
#define MATRIX_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

// ==================== 数据结构 ====================

// 按键事件类型
typedef enum {
    KEY_EVENT_PRESSED,      // 按下
    KEY_EVENT_RELEASED,     // 释放
} key_event_type_t;

// 按键事件结构
typedef struct {
    uint8_t row;            // 行号
    uint8_t col;            // 列号
    uint8_t key_code;       // HID 键码
    key_event_type_t type;  // 事件类型
    int64_t timestamp;      // 时间戳（微秒
} key_event_t;

// 矩阵状态结构体
typedef struct {
    bool current_state;     // 当前状
    bool confirmed_state;   // 确认状
    int64_t last_change_time;  // 最后变化时
} matrix_key_state_t;

// ==================== 函数声明 ====================

/**
 * @brief 初始化矩阵扫描驱
 * 
 * 配置 GPIO 引脚，创建硬件定时器，初始化防抖状? */
void matrix_init(void);

/**
 * @brief 启动矩阵扫描
 * 
 * 启动硬件定时器，开始周期性扫? */
void matrix_start(void);

/**
 * @brief 停止矩阵扫描
 * 
 * 停止硬件定时? */
void matrix_stop(void);

/**
 * @brief 获取按键状
 * 
 * @param row 行号
 * @param col 列号
 * @return true 按下，false 释放
 */
bool matrix_get_key_state(uint8_t row, uint8_t col);

/**
 * @brief 获取所有按键状
 * 
 * @param states 输出状态数?[ROW_NUM][COL_NUM]
 */
void matrix_get_all_states(bool states[][4]);

/**
 * @brief 获取按键事件队列句柄
 *
 * @return 按键事件队列句柄
 */
void *matrix_get_event_queue(void);

/**
 * @brief 矩阵防抖任务（FreeRTOS 任务函数? *
 * 周期比对 ISR 记录的原始电平与确认状态，防抖?0ms）确认后生成按键事件入队
 * 必须?app_main 创建任务运行，否则按键事件永远不会产生? *
 * @param arg 未使? */
void matrix_task(void *arg);

/**
 * @brief 睡眠准备（P7 sleep_mgr 调用）：停止扫描定时器，所有列置高
 *
 * 列保持高电平，按键经二极管拉高行线 → 行作高电平唤醒源
 */
void matrix_sleep_prepare(void);

/**
 * @brief 睡眠恢复（P7 sleep_mgr 调用）：列置低、清防抖状态、重启扫描定时器
 */
void matrix_sleep_restore(void);

#endif // MATRIX_DRIVER_H
