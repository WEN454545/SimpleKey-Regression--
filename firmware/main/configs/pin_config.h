/**
 * @file pin_config.h
 * @brief ESP32-S3 三模键盘 - 引脚定义
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 * 
 * 本文件集中定义所有硬件引脚，修改硬件时只需改动此文件。
 * 引脚分配参考：docx/项目方案.md §3.2
 */

#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

#include "driver/gpio.h"

// ==================== 矩阵引脚 ====================
// COL（列）：输出，逐列驱动高电平
#define MATRIX_COL_0        GPIO_NUM_4
#define MATRIX_COL_1        GPIO_NUM_5
#define MATRIX_COL_2        GPIO_NUM_6
#define MATRIX_COL_3        GPIO_NUM_7
#define MATRIX_COL_NUM      4

// ROW（行）：输入，下拉读取
#define MATRIX_ROW_0        GPIO_NUM_8
#define MATRIX_ROW_1        GPIO_NUM_9
#define MATRIX_ROW_2        GPIO_NUM_10
#define MATRIX_ROW_3        GPIO_NUM_11
#define MATRIX_ROW_4        GPIO_NUM_12
#define MATRIX_ROW_NUM      5

// 矩阵引脚数组（方便遍历）
static const gpio_num_t matrix_col_pins[MATRIX_COL_NUM] = {
    MATRIX_COL_0, MATRIX_COL_1, MATRIX_COL_2, MATRIX_COL_3
};

static const gpio_num_t matrix_row_pins[MATRIX_ROW_NUM] = {
    MATRIX_ROW_0, MATRIX_ROW_1, MATRIX_ROW_2, MATRIX_ROW_3, MATRIX_ROW_4
};

// ==================== 编码器引脚 ====================
// EC11 旋转编码器：A/B 相 + 按键
#define ENC_A_PIN           GPIO_NUM_13
#define ENC_B_PIN           GPIO_NUM_14
#define ENC_SW_PIN          GPIO_NUM_21

// ==================== LED 引脚 ====================
// 低电平点亮：0=ON, 1=OFF
#define LED_USB_PIN         GPIO_NUM_2     // 白色 - USB 模式
#define LED_BLE_PIN         GPIO_NUM_15    // 蓝色 - BLE 模式
#define LED_2G4_PIN         GPIO_NUM_16    // 绿色 - 2.4G 模式

// ==================== 电源管理引脚 ====================
// 电池电压检测
#define VBAT_SENSE_PIN      GPIO_NUM_1     // ADC1_CH0，100k/100k 分压
#define EN_BAT_PIN          GPIO_NUM_17    // 控制 2N7002 栅极：高=采样，低=断路

// 充电状态检测（TP4056 开漏输出，100k 上拉至 3.3V）
#define CHRG_DET_PIN        GPIO_NUM_41    // 低=充电中
#define STDBY_DET_PIN       GPIO_NUM_42    // 低=已充满

// ==================== USB 引脚 ====================
// ESP32-S3 原生 USB OTG（固定引脚）
#define USB_DPLUS_PIN       GPIO_NUM_20    // USB D+
#define USB_DMINUS_PIN      GPIO_NUM_19    // USB D-

// ==================== 调试引脚 ====================
// UART0 调试串口（与 USB 共用，烧录后可省略）
#define DEBUG_TX_PIN        GPIO_NUM_43    // UART0 TX
#define DEBUG_RX_PIN        GPIO_NUM_44    // UART0 RX

// ==================== 禁用引脚 ====================
// 以下引脚被模组占用，禁止使用：
// - GPIO26-32：Flash
// - GPIO33-37：R8 模组 PSRAM
// - GPIO0/3/45/46：Strapping 引脚

// ==================== RTC 域引脚 ====================
// 以下引脚在 RTC 域（GPIO0-21），支持深睡唤醒：
// - 矩阵：GPIO4-12
// - 编码器：GPIO13/14/21
// - LED：GPIO2/15/16

#endif // PIN_CONFIG_H
