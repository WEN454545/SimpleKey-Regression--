/**
 * @file tasks_config.h
 * @brief ESP32-S3 三模键盘 - 任务配置
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 * 
 * 本文件定义 FreeRTOS 任务的优先级、栈大小和核心分配。
 * 任务优先级数值越大，优先级越高。
 */

#ifndef TASKS_CONFIG_H
#define TASKS_CONFIG_H

// ==================== 任务优先级定义 ====================
// 注意：ESP-IDF 中优先级 0 最低，configMAX_PRIORITIES-1 最高
// FreeRTOS 默认 configMAX_PRIORITIES = 25

// 输入任务（高实时性）
#define TASK_PRIORITY_MATRIX        18      // 矩阵扫描（硬件定时器 ISR 驱动）
#define TASK_PRIORITY_ENCODER       11      // 编码器解码（GPIO 中断）

// 通信任务（中等实时性）
#define TASK_PRIORITY_USB_HID       16      // TinyUSB 设备栈
#define TASK_PRIORITY_BLE_HID       15      // NimBLE 协议栈
#define TASK_PRIORITY_SERIAL        5       // 串口命令处理

// 功能任务（低实时性）
#define TASK_PRIORITY_KEYMAP        7       // 键位映射 + 宏处理
#define TASK_PRIORITY_MACRO         2       // 宏播放（P6：低优先级，延时型任务）
#define TASK_PRIORITY_LED           3       // LED 状态指示
#define TASK_PRIORITY_SLEEP         4       // 睡眠监控（P7：1s 周期检查）

// ==================== 任务栈大小定义 ====================
#define TASK_STACK_MATRIX           2048    // 矩阵扫描任务
#define TASK_STACK_ENCODER          2048    // 编码器任务
#define TASK_STACK_USB_HID          4096    // USB HID 任务
#define TASK_STACK_BLE_HID          4096    // BLE HID 任务
#define TASK_STACK_SERIAL           4096    // 串口任务（含命令处理与模式切换调用链）
#define TASK_STACK_KEYMAP           2048    // 键位映射任务
#define TASK_STACK_MACRO            3072    // 宏播放任务（P6：含事件输出调用链）
#define TASK_STACK_LED              1024    // LED 任务
#define TASK_STACK_SLEEP            2560    // 睡眠监控任务（P7：含 light sleep 调用链）

// ==================== 任务核心分配 ====================
// Core 0：通信任务（USB、BLE、串口）
// Core 1：输入任务（矩阵、编码器）
// 注意：ESP32-S3 双核，Core 0 为 Protocol CPU，Core 1 为 Application CPU

#define TASK_CORE_MATRIX            1       // 矩阵扫描放在 Core 1
#define TASK_CORE_ENCODER           0       // 编码器放在 Core 0
#define TASK_CORE_USB_HID           0       // USB HID 放在 Core 0
#define TASK_CORE_BLE_HID           0       // BLE HID 放在 Core 0
#define TASK_CORE_SERIAL            0       // 串口任务放在 Core 0
#define TASK_CORE_KEYMAP            1       // 键位映射放在 Core 1
#define TASK_CORE_LED               0       // LED 任务放在 Core 0

// ==================== 队列配置 ====================
#define QUEUE_SIZE_KEY_EVENT        32      // 按键事件队列大小
#define QUEUE_SIZE_ENCODER_EVENT    32      // 编码器事件队列大小
#define QUEUE_SIZE_LED_EVENT        16      // LED 事件队列大小
#define QUEUE_SIZE_SERIAL_CMD       8       // 串口命令队列大小
#define QUEUE_SIZE_MACRO            2       // 宏触发队列大小（P6：播放中触发直接丢弃）

// ==================== 防抖配置 ====================
#define DEBOUNCE_MS                 20      // 按键防抖时间（毫秒）
#define SCAN_INTERVAL_MS            5       // 矩阵扫描间隔（毫秒）
#define ENCODER_DEBOUNCE_US         1000    // 编码器防抖时间（微秒）

// ==================== 编码器配置 ====================
#define ENCODER_STEPS_PER_DETENT    4       // 每格脉冲数（四倍频解码）
#define ENCODER_LONG_PRESS_MS       800     // 长按判定时间（毫秒）

// ==================== LED 配置 ====================
#define LED_BLINK_FAST_MS           100     // 快闪间隔（毫秒）
#define LED_BLINK_SLOW_MS           500     // 慢闪间隔（毫秒）
#define LED_BLINK_COUNT             3       // 快闪次数

// ==================== BLE 配置 ====================
#define BLE_MAX_SLOTS               3       // 最大配对槽位数
#define BLE_DEVICE_NAME             "SimpleKey_绝希"  // 蓝牙广播名称
#define BLE_ADV_INTERVAL_MIN        0x20    // 最小广播间隔（20ms）
#define BLE_ADV_INTERVAL_MAX        0x40    // 最大广播间隔（40ms）

// ==================== Profile 配置 ====================
#define PROFILE_MAX_COUNT           3       // 最大 Profile 数量
#define PROFILE_NAME_MAX_LEN        16      // Profile 名称最大长度

#endif // TASKS_CONFIG_H
