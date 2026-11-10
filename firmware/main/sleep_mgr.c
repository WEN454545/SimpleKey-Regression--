/**
 * @file sleep_mgr.c
 * @brief ESP32-S3 三模键盘 - 两级睡眠管理器（P7，方案 §12.13）
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-28
 *
 * 实现说明：
 * - 矩阵为「列驱动高、行读下拉」（matrix_driver.c 实测口径），
 *   睡眠期间列保持输出高，按键经二极管拉高行线 → 行作高电平唤醒源
 * - light sleep：esp_sleep_enable_gpio_wakeup（行高电平 + 旋钮按下低电平），
 *   唤醒后恢复矩阵扫描与 LED；唤醒源含编码器旋转（A/B 低电平唤醒，2026-10-10 用户决策；
 *   A/B 静态非高即触点闭合驻停时不布旋转唤醒源，防反复瞬醒）
 * - deep sleep：行列脚切换 RTC 域（列 RTC 输出高保持 + 行 RTC 唤醒），
 *   EN_BAT RTC 保持低 → 电池采样分压断路零漏电；其余 RTC 引脚 isolate；
 *   esp_deep_sleep_start() 不返回，唤醒即复位重启（约 300ms）
 * - BLE 连接态 / WiFi 热点模式不进 light sleep（方案 §12.13 降级口径）
 * - light sleep 被拒（USB 栈活跃等返回 ESP_ERR_SLEEP_REJECT）时 30s 退避重试
 */

#include "sleep_mgr.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "nvs.h"

#include "pin_config.h"
#include "tasks_config.h"
#include "matrix_driver.h"
#include "led_driver.h"
#include "power_mgr.h"
#include "hid_manager.h"
#include "ble_hid.h"
#include "macro.h"

static const char *TAG = "SLEEP";

// ==================== 参数与状态 ====================
#define SLEEP_NVS_NAMESPACE     "sleepmgr"
#define SLEEP_NVS_KEY_LIGHT     "light_min"
#define SLEEP_NVS_KEY_DEEP      "deep_hr"
#define SLEEP_LIGHT_MIN_DEFAULT 5       // light sleep 闲置阈值（分钟）
#define SLEEP_DEEP_HR_DEFAULT   24      // deep sleep 闲置阈值（小时）
#define SLEEP_RETRY_BACKOFF_S   30      // light sleep 被拒后的退避重试间隔

static uint16_t s_light_min = SLEEP_LIGHT_MIN_DEFAULT;
static uint16_t s_deep_hr = SLEEP_DEEP_HR_DEFAULT;
static volatile int64_t s_last_activity_us = 0;   // 最近活动时间戳（esp_timer 微秒）
static int s_backoff_s = 0;                        // light sleep 退避倒计时（秒）

// 矩阵/编码器引脚表（与 matrix_driver/encoder_driver 一致）
static const gpio_num_t s_cols[MATRIX_COL_NUM] = {
    MATRIX_COL_0, MATRIX_COL_1, MATRIX_COL_2, MATRIX_COL_3 };
static const gpio_num_t s_rows[MATRIX_ROW_NUM] = {
    MATRIX_ROW_0, MATRIX_ROW_1, MATRIX_ROW_2, MATRIX_ROW_3, MATRIX_ROW_4 };

// ==================== NVS 参数 ====================
static void load_params(void) {
    nvs_handle_t h;
    if (nvs_open(SLEEP_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, SLEEP_NVS_KEY_LIGHT, &s_light_min);
        nvs_get_u16(h, SLEEP_NVS_KEY_DEEP, &s_deep_hr);
        nvs_close(h);
    }
    // 越界兜底
    if (s_light_min == 0 || s_light_min > 120) s_light_min = SLEEP_LIGHT_MIN_DEFAULT;
    if (s_deep_hr == 0 || s_deep_hr > 168) s_deep_hr = SLEEP_DEEP_HR_DEFAULT;
}

uint16_t sleep_mgr_get_light_min(void) { return s_light_min; }
uint16_t sleep_mgr_get_deep_hr(void) { return s_deep_hr; }

bool sleep_mgr_set_params(uint16_t light_min, uint16_t deep_hr) {
    if (light_min == 0 || light_min > 120) return false;
    if (deep_hr == 0 || deep_hr > 168) return false;
    nvs_handle_t h;
    esp_err_t err = nvs_open(SLEEP_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;
    err = nvs_set_u16(h, SLEEP_NVS_KEY_LIGHT, light_min);
    if (err == ESP_OK) err = nvs_set_u16(h, SLEEP_NVS_KEY_DEEP, deep_hr);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;
    s_light_min = light_min;
    s_deep_hr = deep_hr;
    ESP_LOGI(TAG, "Sleep params: light=%umin deep=%uh", s_light_min, s_deep_hr);
    return true;
}

// ==================== 活动时间戳 ====================
void sleep_mgr_activity(void) {
    s_last_activity_us = esp_timer_get_time();
}

// ==================== light sleep ====================

// 当前模式对应的 LED 颜色（与 main.c 语义一致：USB=白/BLE=蓝/WiFi=绿）
static led_color_t mode_led_color(hid_mode_t mode) {
    return (mode == HID_MODE_USB) ? LED_COLOR_USB :
           (mode == HID_MODE_BLE) ? LED_COLOR_BLE : LED_COLOR_2G4;
}

// light sleep 允许条件：USB 不在位 + 非 WiFi 热点 + 宏未播放
// BLE 连接态也允许（2026-10-11 用户决策）：modem sleep 已启用（sdkconfig
// BT_CTRL_MODEM_SLEEP Mode1 + 主晶振 LPCLK），睡眠中控制器按连接事件自动唤醒；
// 旧条件「连接不睡」是 modem sleep 未启用、入睡必被拒时代的保守兜底，已移除
static bool light_sleep_allowed(bool usb_present, hid_mode_t mode) {
    if (usb_present) return false;
    if (mode == HID_MODE_WIFI) return false;                        // SoftAP 需持续在线
    if (macro_engine_is_playing()) return false;                    // 宏播放中不睡（防时序中断）
    return true;
}

static void enter_light_sleep(void) {
    hid_mode_t mode = hid_manager_get_mode();
    led_color_t color = mode_led_color(mode);

    // LED 熄灭省电（light sleep 期间 GPIO 电平保持，常亮 LED 会主导功耗）
    // 注意：恢复时统一回常亮，若睡眠前处于慢闪（低电）等状态，下一轮低电检测会重新设置
    led_set_mode(color, LED_MODE_OFF);

    // 唤醒源：任意键（行高电平，列保持高由 matrix_sleep_prepare 保证）+ 旋钮按下（低电平）
    for (int r = 0; r < MATRIX_ROW_NUM; r++) {
        gpio_wakeup_enable(s_rows[r], GPIO_INTR_HIGH_LEVEL);
    }
    gpio_wakeup_enable(ENC_SW_PIN, GPIO_INTR_LOW_LEVEL);
    // 旋转唤醒（2026-10-10 用户决策）：A/B 静态高（上拉 + 触点断开驻停）时布低电平唤醒；
    // 若任一脚静态为低（触点闭合驻停），布了会立即瞬醒-重睡循环，本轮跳过旋转唤醒源
    gpio_pullup_en(ENC_A_PIN);
    gpio_pullup_en(ENC_B_PIN);
    esp_rom_delay_us(100);   // 等内部上拉建立再读静态电平，防浮空误判
    bool enc_rest_high = gpio_get_level(ENC_A_PIN) == 1 && gpio_get_level(ENC_B_PIN) == 1;
    if (enc_rest_high) {
        gpio_wakeup_enable(ENC_A_PIN, GPIO_INTR_LOW_LEVEL);
        gpio_wakeup_enable(ENC_B_PIN, GPIO_INTR_LOW_LEVEL);
    }
    esp_sleep_enable_gpio_wakeup();

    matrix_sleep_prepare();
    ESP_LOGI(TAG, "Light sleep enter (mode=%s enc_wake=%d)", hid_manager_get_mode_name(mode), enc_rest_high);
    esp_err_t err = esp_light_sleep_start();

    // ---- 唤醒（或进入失败）恢复 ----
    matrix_sleep_restore();
    // 撤销 GPIO 唤醒源配置
    for (int r = 0; r < MATRIX_ROW_NUM; r++) {
        gpio_wakeup_enable(s_rows[r], GPIO_INTR_DISABLE);
    }
    gpio_wakeup_enable(ENC_SW_PIN, GPIO_INTR_DISABLE);
    gpio_wakeup_enable(ENC_A_PIN, GPIO_INTR_DISABLE);
    gpio_wakeup_enable(ENC_B_PIN, GPIO_INTR_DISABLE);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    led_set_mode(color, LED_MODE_ON);
    sleep_mgr_activity();

    if (err != ESP_OK) {
        // 多为 USB/BLE 栈活跃时 ESP_ERR_SLEEP_REJECT：退避后重试
        s_backoff_s = SLEEP_RETRY_BACKOFF_S;
        ESP_LOGW(TAG, "Light sleep rejected (0x%x), retry in %ds", err, SLEEP_RETRY_BACKOFF_S);
    } else {
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        ESP_LOGI(TAG, "Light sleep wake, cause=%d", (int)cause);
    }
}

// ==================== deep sleep ====================
void sleep_mgr_deep_sleep_enter(const char *reason) {
    ESP_LOGI(TAG, "Deep sleep enter: %s", reason ? reason : "idle");   // 关键状态变化用 LOGI（AGENTS §5）

    // 1. 矩阵列 → RTC 输出高并保持（按键经二极管拉高行线触发唤醒）
    for (int c = 0; c < MATRIX_COL_NUM; c++) {
        rtc_gpio_init(s_cols[c]);
        rtc_gpio_set_direction(s_cols[c], RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(s_cols[c], 1);
        rtc_gpio_hold_en(s_cols[c]);
    }
    // 2. 矩阵行 → RTC 输入下拉 + 高电平唤醒
    uint64_t wake_mask = 0;
    for (int r = 0; r < MATRIX_ROW_NUM; r++) {
        rtc_gpio_init(s_rows[r]);
        rtc_gpio_set_direction(s_rows[r], RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pulldown_en(s_rows[r]);
        rtc_gpio_pullup_dis(s_rows[r]);
        rtc_gpio_wakeup_enable(s_rows[r], GPIO_INTR_HIGH_LEVEL);
        rtc_gpio_hold_en(s_rows[r]);
        wake_mask |= (1ULL << s_rows[r]);
    }
    // S3 深睡唤醒仅支持 RTC GPIO：走 ext1（ANY_HIGH），勿用 esp_deep_sleep_enable_gpio_wakeup（C 系列专用）
    esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_HIGH);

    // 3. EN_BAT 保持低：电池采样分压网络断路（2N7002 关断）零漏电
    rtc_gpio_init(EN_BAT_PIN);
    rtc_gpio_set_direction(EN_BAT_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(EN_BAT_PIN, 0);
    rtc_gpio_hold_en(EN_BAT_PIN);

    // 4. 其余 RTC 域引脚隔离防悬空漏电（VBAT_SENSE/LED×3/编码器×3），
    //    勿动唤醒用的行列脚；GPIO41/42 非 RTC 域无需处理
    const gpio_num_t others[] = {
        VBAT_SENSE_PIN, LED_USB_PIN, LED_BLE_PIN, LED_2G4_PIN,
        ENC_A_PIN, ENC_B_PIN, ENC_SW_PIN };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        rtc_gpio_isolate(others[i]);
        rtc_gpio_hold_en(others[i]);
    }

    esp_deep_sleep_start();   // 不返回；任意键唤醒 = 复位重启
}

// ==================== 深睡 hold 释放（上电最早阶段调用） ====================

void sleep_mgr_release_wake_holds(void) {
    // 深睡唤醒 = 复位，但 RTC pad hold 不随复位清除（2026-10-10 深睡审查发现）：
    // 不释放则列锁输出高、行锁 RTC 唤醒配置、EN_BAT 锁低——矩阵无输入、
    // 电池采样断路（mv≈0 误触发低电关机），只有断电重上电才恢复
    for (int c = 0; c < MATRIX_COL_NUM; c++) {
        rtc_gpio_hold_dis(s_cols[c]);
    }
    for (int r = 0; r < MATRIX_ROW_NUM; r++) {
        rtc_gpio_hold_dis(s_rows[r]);
    }
    const gpio_num_t held[] = {
        EN_BAT_PIN, VBAT_SENSE_PIN, LED_USB_PIN, LED_BLE_PIN, LED_2G4_PIN,
        ENC_A_PIN, ENC_B_PIN, ENC_SW_PIN };
    for (size_t i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
        rtc_gpio_hold_dis(held[i]);
    }
}

// ==================== 监控任务 ====================
static void sleep_mgr_task(void *arg) {
    esp_task_wdt_add(NULL);   // 1s 有界循环，可安全注册看门狗
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (s_backoff_s > 0) {
            s_backoff_s--;
            esp_task_wdt_reset();
            continue;
        }

        power_status_t ps;
        power_mgr_policy_tick(&ps);   // 1s 周期电源策略（低电告警 + 3.35V 关机确认）
        hid_mode_t mode = hid_manager_get_mode();
        int64_t idle_us = esp_timer_get_time() - s_last_activity_us;

        if (idle_us >= (int64_t)s_deep_hr * 3600ULL * 1000000ULL) {
            if (ps.usb_present) {
                // USB 在位永不睡眠（方案 §12.13 无条件口径，含深睡）：刷新计时待机
                sleep_mgr_activity();
            } else {
                esp_task_wdt_delete(NULL);   // 深睡不返回，防退出前喂狗告警
                sleep_mgr_deep_sleep_enter("idle timeout");
            }
        } else if (idle_us >= (int64_t)s_light_min * 60ULL * 1000000ULL &&
                   light_sleep_allowed(ps.usb_present, mode)) {
            enter_light_sleep();
        }
        esp_task_wdt_reset();
    }
}

void sleep_mgr_init(void) {
    load_params();
    sleep_mgr_activity();
    BaseType_t ok = xTaskCreate(sleep_mgr_task, "sleep_mgr", TASK_STACK_SLEEP, NULL,
                                TASK_PRIORITY_SLEEP, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sleep_mgr task");
        return;
    }
    ESP_LOGD(TAG, "Sleep mgr: light=%umin deep=%uh", s_light_min, s_deep_hr);
}
