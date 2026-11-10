/**
 * @file led_driver.c
 * @brief ESP32-S3 三模键盘 - LED 驱动
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 * 
 * 3 颗单色模式 LED（低电平点亮）
 * - USB 模式：白色（GPIO2）
 * - BLE 模式：蓝色（GPIO15）
 * - 2.4G 模式：绿色（GPIO16）
 *
 * LED 语义
 * - 常亮：当前模式
 * - 慢闪 1Hz：低电（<3.55V）
 * - 快闪 N 次：切换 Profile N
 * - 快闪 5Hz：配对中 */

#include "led_driver.h"
#include "pin_config.h"
#include "tasks_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "LED";

// ==================== 全局变量 ====================
static QueueHandle_t led_event_queue = NULL;  // LED 事件队列
static led_color_t current_color = LED_COLOR_NONE;  // 当前颜色
static led_mode_t current_mode = LED_MODE_OFF;  // 当前模式
static bool low_battery_mode = false;  // 低电模式

// ==================== 辅助函数 ====================
/**
 * @brief 设置单个 LED 状
 * 
 * @param color LED 颜色
 * @param on true 点亮，false 熄灭
 */
static void led_set_single(led_color_t color, bool on) {
    gpio_num_t pin;
    switch (color) {
        case LED_COLOR_USB: pin = LED_USB_PIN; break;
        case LED_COLOR_BLE: pin = LED_BLE_PIN; break;
        case LED_COLOR_2G4: pin = LED_2G4_PIN; break;
        default: return;
    }
    
    // 低电平点
    gpio_set_level(pin, on ? 0 : 1);
}

/**
 * @brief 关闭所有LED
 */
static void led_all_off(void) {
    gpio_set_level(LED_USB_PIN, 1);   // 高电平熄灭
    gpio_set_level(LED_BLE_PIN, 1);
    gpio_set_level(LED_2G4_PIN, 1);
}

/**
 * @brief LED 闪烁控制任务
 * 
 * 根据当前模式控制 LED 闪烁
 */
static void led_task(void *arg) {
    ESP_LOGD(TAG, "LED task started");
    
    led_event_t event;
    uint8_t blink_counter = 0;
    bool led_state = false;
    
    while (1) {
        // 检查是否有新事
        if (xQueueReceive(led_event_queue, &event, 0) == pdTRUE) {
            switch (event.type) {
                case LED_EVENT_SET_COLOR:
                    current_color = event.color;
                    blink_counter = 0;
                    break;
                    
                case LED_EVENT_SET_MODE:
                    current_mode = event.mode;
                    blink_counter = 0;
                    break;
                    
                case LED_EVENT_BLINK_N:
                    current_color = event.color;
                    current_mode = LED_MODE_BLINK_N;
                    blink_counter = event.blink_count;
                    break;
                    
                case LED_EVENT_LOW_BATTERY:
                    low_battery_mode = (event.blink_count > 0);
                    break;
            }
        }
        
        // 根据模式控制 LED
        switch (current_mode) {
            case LED_MODE_OFF:
                led_all_off();
                break;
                
            case LED_MODE_ON:
                led_all_off();
                if (current_color != LED_COLOR_NONE) {
                    led_set_single(current_color, true);
                }
                break;
                
            case LED_MODE_BLINK_SLOW:
                // 慢闪?Hz
                led_state = !led_state;
                led_all_off();
                if (led_state && current_color != LED_COLOR_NONE) {
                    led_set_single(current_color, true);
                }
                vTaskDelay(pdMS_TO_TICKS(LED_BLINK_SLOW_MS));
                continue;  // 跳过最后的延时
                
            case LED_MODE_BLINK_FAST:
                // 快闪?Hz
                led_state = !led_state;
                led_all_off();
                if (led_state && current_color != LED_COLOR_NONE) {
                    led_set_single(current_color, true);
                }
                vTaskDelay(pdMS_TO_TICKS(LED_BLINK_FAST_MS));
                continue;  // 跳过最后的延时
                
            case LED_MODE_BLINK_N:
                // 快闪 N 
                if (blink_counter > 0) {
                    led_state = !led_state;
                    led_all_off();
                    
                    if (led_state) {
                        led_set_single(current_color, true);
                        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_FAST_MS));
                    } else {
                        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_FAST_MS));
                        blink_counter--;
                    }
                    continue;  // 跳过最后的延时
                } else {
                    // 闪烁完成，恢复常
                    current_mode = LED_MODE_ON;
                }
                break;
        }
        
        // 主循环延
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ==================== 初始化函数 ====================
void led_init(void) {
    ESP_LOGD(TAG, "Initializing LEDs...");
    
    // 创建 LED 事件队列
    led_event_queue = xQueueCreate(QUEUE_SIZE_LED_EVENT, sizeof(led_event_t));
    if (led_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create LED event queue");
        return;
    }
    
    // 配置 LED 引脚（输出）
    gpio_config_t led_config = {
        .pin_bit_mask = (1ULL << LED_USB_PIN) | (1ULL << LED_BLE_PIN) | (1ULL << LED_2G4_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&led_config);
    
    // 关闭所有LED
    led_all_off();
    
    // 创建 LED 任务
    xTaskCreate(led_task, "led_task", TASK_STACK_LED, NULL, TASK_PRIORITY_LED, NULL);
    
    ESP_LOGD(TAG, "LED initialized");
    ESP_LOGD(TAG, "  USB LED (White): GPIO%d", LED_USB_PIN);
    ESP_LOGD(TAG, "  BLE LED (Blue): GPIO%d", LED_BLE_PIN);
    ESP_LOGD(TAG, "  2.4G LED (Green): GPIO%d", LED_2G4_PIN);
}

// ==================== 控制函数 ====================
void led_set_mode(led_color_t color, led_mode_t mode) {
    led_event_t event = {
        .type = LED_EVENT_SET_COLOR,
        .color = color,
    };
    xQueueSend(led_event_queue, &event, 0);
    
    event.type = LED_EVENT_SET_MODE;
    event.mode = mode;
    xQueueSend(led_event_queue, &event, 0);
}

void led_blink_n(led_color_t color, uint8_t count) {
    led_event_t event = {
        .type = LED_EVENT_BLINK_N,
        .color = color,
        .blink_count = count,
    };
    xQueueSend(led_event_queue, &event, 0);
}

void led_set_low_battery(bool enable) {
    led_event_t event = {
        .type = LED_EVENT_LOW_BATTERY,
        .blink_count = enable ? 1 : 0,
    };
    xQueueSend(led_event_queue, &event, 0);
}

void *led_get_event_queue(void) {
    return led_event_queue;
}
