/**
 * @file encoder_driver.c
 * @brief ESP32-S3 三模键盘 - 编码器驱
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 * 
 * EC11 旋转编码器驱
 * - A/B 相：GPIO13/14，四倍频解码
 * - 按键：GPIO21
 * - 使用 GPIO 中断 + 状态表解码
 * 
 * 解码原理
 * - 四倍频：每个脉冲的上升沿和下降沿都计数
 * - 状态表?old_state << 2) | new_state 查表得到方向
 * - ?4 个脉?= 1 格旋? */

#include "encoder_driver.h"
#include "pin_config.h"
#include "tasks_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char *TAG = "ENCODER";

// ==================== 全局变量 ====================
static QueueHandle_t encoder_event_queue = NULL;  // 编码器事件队
static volatile uint8_t encoder_state = 0;        // 编码?A/B 状
static volatile int32_t encoder_count = 0;        // 旋转计数（四倍频，原始脉冲）
static volatile int32_t encoder_pending = 0;      // 未满 1 格的脉冲累计（按格合并事件用
static volatile bool button_pressed = false;      // 按键状
static volatile int64_t button_press_time = 0;    // 按键按下时间

// 四倍频解码状态表
// 索引?old_state << 2) | new_state
// 值：+1 = 顺时针，-1 = 逆时针，0 = 无效
// 四倍频正交解码表：index = (旧状态<<2)|新状态，值 = 方向增量
// 2026-10-11 用户决策：本批硬件顺时针 = 原表逆时针，整体取反修正方向
// （旧表值符号与下表相反；若更换编码器方向再反，恢复旧符号即可）
static const int8_t encoder_table[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0,
};

// ==================== 中断处理函数 ====================
/**
 * @brief 编码?A/B 相中断处理函
 * 
 * 处理编码器旋转，发送旋转事? */
static void IRAM_ATTR encoder_isr_handler(void *arg) {
    // 读取 A/B 相状
    uint8_t a = gpio_get_level(ENC_A_PIN);
    uint8_t b = gpio_get_level(ENC_B_PIN);
    
    // 计算新状
    uint8_t new_state = (a << 1) | b;
    uint8_t index = (encoder_state << 2) | new_state;
    
    // 查表得到方向
    int8_t delta = encoder_table[index];
    
    // 更新状
    encoder_state = new_state;
    
    // 如果有旋转，累计脉冲；每?1 格（ENCODER_STEPS_PER_DETENT 个脉冲）才发送一次事件?    // 否则一格旋转会逐边沿触?4 次事件（= 音量连加 4 格），与「一格一步进」预期不
    if (delta != 0) {
        encoder_count += delta;
        encoder_pending += delta;

        int8_t detents = (int8_t)(encoder_pending / ENCODER_STEPS_PER_DETENT);
        if (detents != 0) {
            encoder_pending -= detents * ENCODER_STEPS_PER_DETENT;

            encoder_event_t event = {
                .type = ENCODER_EVENT_ROTATION,
                .delta = detents,  // 单位=格数（快速旋转时可能 >1?                .button_state = button_pressed,
                .timestamp = esp_timer_get_time(),
            };

            // ?ISR 发送事件到队列
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xQueueSendFromISR(encoder_event_queue, &event, &xHigherPriorityTaskWoken);

            if (xHigherPriorityTaskWoken) {
                portYIELD_FROM_ISR();
            }
        }
    }
}

/**
 * @brief 编码器按键中断处理函
 * 
 * 处理编码器按键按?释放
 */
static void IRAM_ATTR button_isr_handler(void *arg) {
    // 读取按键状态（低电平有效）
    bool new_state = !gpio_get_level(ENC_SW_PIN);
    int64_t current_time = esp_timer_get_time();
    
    // 防抖处理（简单的时间判断
    if (new_state != button_pressed) {
        if (current_time - button_press_time > (ENCODER_DEBOUNCE_US)) {
            button_pressed = new_state;
            button_press_time = current_time;
            
            encoder_event_t event = {
                .type = ENCODER_EVENT_BUTTON,
                .delta = 0,
                .button_state = button_pressed,
                .timestamp = current_time,
            };
            
            // ?ISR 发送事件到队列
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xQueueSendFromISR(encoder_event_queue, &event, &xHigherPriorityTaskWoken);
            
            if (xHigherPriorityTaskWoken) {
                portYIELD_FROM_ISR();
            }
        }
    }
}

// ==================== 初始化函?====================
void encoder_init(void) {
    ESP_LOGD(TAG, "Initializing encoder...");
    
    // 创建事件队列
    encoder_event_queue = xQueueCreate(QUEUE_SIZE_ENCODER_EVENT, sizeof(encoder_event_t));
    if (encoder_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create encoder event queue");
        return;
    }
    
    // 配置 A/B 相引脚（输入，上拉）
    gpio_config_t ab_config = {
        .pin_bit_mask = (1ULL << ENC_A_PIN) | (1ULL << ENC_B_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,  // 任何边沿触发
    };
    gpio_config(&ab_config);
    
    // 配置按键引脚（输入，上拉
    gpio_config_t sw_config = {
        .pin_bit_mask = (1ULL << ENC_SW_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,  // 任何边沿触发
    };
    gpio_config(&sw_config);
    
    // 安装 GPIO 中断服务
    gpio_install_isr_service(0);
    
    // 添加 A/B 相中断处理函
    gpio_isr_handler_add(ENC_A_PIN, encoder_isr_handler, NULL);
    gpio_isr_handler_add(ENC_B_PIN, encoder_isr_handler, NULL);
    
    // 添加按键中断处理函数
    gpio_isr_handler_add(ENC_SW_PIN, button_isr_handler, NULL);
    
    // 初始化编码器状
    encoder_state = (gpio_get_level(ENC_A_PIN) << 1) | gpio_get_level(ENC_B_PIN);
    button_pressed = !gpio_get_level(ENC_SW_PIN);
    
    ESP_LOGD(TAG, "Encoder initialized");
    ESP_LOGD(TAG, "  A Phase: GPIO%d", ENC_A_PIN);
    ESP_LOGD(TAG, "  B Phase: GPIO%d", ENC_B_PIN);
    ESP_LOGD(TAG, "  Button: GPIO%d", ENC_SW_PIN);
}

// ==================== 查询函数 ====================
void *encoder_get_event_queue(void) {
    return encoder_event_queue;
}

int32_t encoder_get_count(void) {
    return encoder_count / ENCODER_STEPS_PER_DETENT;  // 转换为格
}

void encoder_reset_count(void) {
    encoder_count = 0;
}

bool encoder_get_button_state(void) {
    return button_pressed;
}
