/**
 * @file matrix_driver.c
 * @brief ESP32-S3 三模键盘 - 矩阵扫描驱动
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 * 
 * 4×5 矩阵扫描?9 ?+ 1 ?2u 空格
 * 使用硬件定时?ISR 驱动扫描，软件防抖（20ms
 * 
 * 矩阵原理
 * - 列（COL）：输出，逐列驱动高电
 * - 行（ROW）：输入，下拉读
 * - 二极管方向：行→列（阳极→行，阴极→列）
 */

#include "matrix_driver.h"
#include "pin_config.h"
#include "tasks_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "driver/gptimer.h"
#include "esp_rom_sys.h"

static const char *TAG = "MATRIX";

// ==================== 全局变量 ====================
static QueueHandle_t key_event_queue = NULL;      // 按键事件队列
static matrix_key_state_t key_states[MATRIX_ROW_NUM][MATRIX_COL_NUM] = {0};  // 按键状
static gptimer_handle_t scan_timer = NULL;        // 扫描定时
static uint8_t current_col = 0;                   // 当前扫描?
// ==================== 定时器回?====================
/**
 * @brief 矩阵扫描定时器回调函
 * 
 * ?5ms 触发一次，逐列扫描
 * 注意：此函数在中断上下文中执行，必须快? */
static bool IRAM_ATTR matrix_scan_isr(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_data) {
    // 拉高当前
    gpio_set_level(matrix_col_pins[current_col], 1);
    
    // 短暂延时等待电平稳定（约 1us
    esp_rom_delay_us(1);
    
    // 读取所有行
    for (int r = 0; r < MATRIX_ROW_NUM; r++) {
        uint8_t raw_state = gpio_get_level(matrix_row_pins[r]);
        
        // 记录状态变化（防抖由主循环处理
        if (raw_state != key_states[r][current_col].current_state) {
            key_states[r][current_col].current_state = raw_state;
            key_states[r][current_col].last_change_time = esp_timer_get_time();
        }
    }
    
    // 拉低当前
    gpio_set_level(matrix_col_pins[current_col], 0);
    
    // 切换到下一
    current_col = (current_col + 1) % MATRIX_COL_NUM;
    
    return false;  // 不需要任务切
}

// ==================== 初始化函?====================
void matrix_init(void) {
    ESP_LOGD(TAG, "Initializing matrix scan...");
    
    // 创建按键事件队列
    key_event_queue = xQueueCreate(QUEUE_SIZE_KEY_EVENT, sizeof(key_event_t));
    if (key_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create key event queue");
        return;
    }
    
    // 配置列引脚（输出，初始低电平
    gpio_config_t col_config = {
        .pin_bit_mask = (1ULL << MATRIX_COL_0) | (1ULL << MATRIX_COL_1) |
                        (1ULL << MATRIX_COL_2) | (1ULL << MATRIX_COL_3),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&col_config);
    
    // 所有列初始化为低电
    for (int c = 0; c < MATRIX_COL_NUM; c++) {
        gpio_set_level(matrix_col_pins[c], 0);
    }
    
    // 配置行引脚（输入，下拉）
    gpio_config_t row_config = {
        .pin_bit_mask = (1ULL << MATRIX_ROW_0) | (1ULL << MATRIX_ROW_1) |
                        (1ULL << MATRIX_ROW_2) | (1ULL << MATRIX_ROW_3) |
                        (1ULL << MATRIX_ROW_4),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&row_config);
    
    // 初始化按键状
    for (int r = 0; r < MATRIX_ROW_NUM; r++) {
        for (int c = 0; c < MATRIX_COL_NUM; c++) {
            key_states[r][c].current_state = false;
            key_states[r][c].confirmed_state = false;
            key_states[r][c].last_change_time = 0;
        }
    }
    
    // 配置硬件定时
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,  // 1MHz?us 分辨?
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &scan_timer));
    
    // 配置定时器报警（?5ms 触发一次）
    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,
        .alarm_count = SCAN_INTERVAL_MS * 1000,  // 5ms = 5000us
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(scan_timer, &alarm_config));
    
    // 注册定时器回
    gptimer_event_callbacks_t cbs = {
        .on_alarm = matrix_scan_isr,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(scan_timer, &cbs, NULL));

    // 使能定时器（gptimer_start 要求?enable，否则报 ESP_ERR_INVALID_STATE
    ESP_ERROR_CHECK(gptimer_enable(scan_timer));

    ESP_LOGD(TAG, "Matrix scan initialized");
    ESP_LOGD(TAG, "  COL: GPIO%d/%d/%d/%d", MATRIX_COL_0, MATRIX_COL_1, MATRIX_COL_2, MATRIX_COL_3);
    ESP_LOGD(TAG, "  ROW: GPIO%d/%d/%d/%d/%d", MATRIX_ROW_0, MATRIX_ROW_1, MATRIX_ROW_2, MATRIX_ROW_3, MATRIX_ROW_4);
    ESP_LOGD(TAG, "  Scan interval: %dms", SCAN_INTERVAL_MS);
}

// ==================== 启动/停止函数 ====================
void matrix_start(void) {
    if (scan_timer != NULL) {
        // 幂等：已启动时忽?ESP_ERR_INVALID_STATE，避?abort 重启
        esp_err_t ret = gptimer_start(scan_timer);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(ret);
        }
        ESP_LOGD(TAG, "Matrix scan started");
    }
}

void matrix_stop(void) {
    if (scan_timer != NULL) {
        // 幂等：已停止时忽?ESP_ERR_INVALID_STATE
        esp_err_t ret = gptimer_stop(scan_timer);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(ret);
        }
        ESP_LOGD(TAG, "Matrix scan stopped");
    }
}

// ==================== 查询函数 ====================
bool matrix_get_key_state(uint8_t row, uint8_t col) {
    if (row < MATRIX_ROW_NUM && col < MATRIX_COL_NUM) {
        return key_states[row][col].confirmed_state;
    }
    return false;
}

void matrix_get_all_states(bool states[][4]) {
    for (int r = 0; r < MATRIX_ROW_NUM; r++) {
        for (int c = 0; c < MATRIX_COL_NUM; c++) {
            states[r][c] = key_states[r][c].confirmed_state;
        }
    }
}

void *matrix_get_event_queue(void) {
    return key_event_queue;
}

// ==================== 防抖处理任务 ====================
void matrix_task(void *arg) {
    ESP_LOGD(TAG, "Matrix task started");
    esp_task_wdt_add(NULL);   // 注册任务级看门狗（5s 超时 → panic → coredump，P7）

    int64_t current_time;

    while (1) {
        current_time = esp_timer_get_time();
        
        // 扫描所有按
        for (int r = 0; r < MATRIX_ROW_NUM; r++) {
            for (int c = 0; c < MATRIX_COL_NUM; c++) {
                // 检查状态变
                if (key_states[r][c].current_state != key_states[r][c].confirmed_state) {
                    // 计算变化时间
                    int64_t elapsed = current_time - key_states[r][c].last_change_time;
                    
                    // 防抖确认?0ms
                    if (elapsed >= (DEBOUNCE_MS * 1000)) {
                        // 状态确
                        key_states[r][c].confirmed_state = key_states[r][c].current_state;
                        
                        // 按键按下时输出日
                        if (key_states[r][c].confirmed_state) {
                            ESP_LOGD(TAG, "Key PRESS: row=%d col=%d", r, c);
                        }
                        
                        // 创建按键事件
                        key_event_t event = {
                            .row = r,
                            .col = c,
                            .key_code = r * MATRIX_COL_NUM + c,  // 临时键码，后续由 keymap 替换
                            .type = key_states[r][c].confirmed_state ? KEY_EVENT_PRESSED : KEY_EVENT_RELEASED,
                            .timestamp = current_time,
                        };
                        
                        // 发送事件到队列
                        if (xQueueSend(key_event_queue, &event, 0) != pdTRUE) {
                            ESP_LOGW(TAG, "Key event queue full, event dropped");
                        }
                    }
                }
            }
        }
        
        // 休眠一段时
        vTaskDelay(pdMS_TO_TICKS(5));
        esp_task_wdt_reset();
    }
}

// ==================== 睡眠支持（P7 sleep_mgr 调用） ====================
static bool s_matrix_sleeping = false;

void matrix_sleep_prepare(void) {
    if (s_matrix_sleeping) return;
    // 停止扫描定时器（light sleep 期间 gptimer 中断会反复唤醒芯片，必须停）
    gptimer_stop(scan_timer);
    // 所有列置高：按键经二极管拉高行线 → 行作高电平唤醒源
    for (int c = 0; c < MATRIX_COL_NUM; c++) {
        gpio_set_level(matrix_col_pins[c], 1);
        // Pad hold 锁住列高电平（2026-10-11 实测修复）：行脚是唤醒源，IDF 的
        // gpio_wakeup_enable 会自动对其 sleep_sel_dis（睡眠中保持活跃配置）；
        // 列脚不是唤醒源，睡眠中 pad 切到睡眠配置会把输出高释放——行拉不高，
        // 任意键唤醒失效（表现为只有旋钮能唤醒）。hold 锁住输出电平即解决
        gpio_hold_en(matrix_col_pins[c]);
    }
    s_matrix_sleeping = true;
}

void matrix_sleep_restore(void) {
    if (!s_matrix_sleeping) return;
    // 先解除 hold 再拉低恢复常态（顺序反了会先闪一下高电平）
    for (int c = 0; c < MATRIX_COL_NUM; c++) {
        gpio_hold_dis(matrix_col_pins[c]);
        gpio_set_level(matrix_col_pins[c], 0);
    }
    // 清防抖状态（睡眠期间电平变化已不可信，防唤醒瞬间产生幽灵按键事件）
    for (int r = 0; r < MATRIX_ROW_NUM; r++) {
        for (int c = 0; c < MATRIX_COL_NUM; c++) {
            key_states[r][c].current_state = false;
            key_states[r][c].confirmed_state = false;
            key_states[r][c].last_change_time = 0;
        }
    }
    gptimer_start(scan_timer);
    s_matrix_sleeping = false;
}
