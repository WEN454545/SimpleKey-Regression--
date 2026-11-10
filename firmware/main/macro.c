/**
 * @file macro.c
 * @brief ESP32-S3 三模键盘 - 宏引擎（P6 智能功能）
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-22
 *
 * 播放模型：触发入队 → 播放任务逐取出 → 逐步骤执行 → 结束清残留。
 * 步骤执行语义：
 *   DOWN  普通键/修饰键走 hid_manager_send_key（6KRO 状态机）；Consumer 键按脉冲处理
 *   UP    与 DOWN 对应的释放
 *   DELAY vTaskDelay 真实延时（WiFi 模式同样在键盘端延时，上位机照睡，时序不失真）
 *   TAP   展开为 DOWN + 30ms + UP
 * 结束时统一释放播放期间按住的全部键与修饰键，防止卡键。
 */

#include "macro.h"
#include "profile_store.h"
#include "keymap.h"
#include "hid_manager.h"
#include "net_input.h"
#include "tasks_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "MACRO";

static QueueHandle_t macro_queue = NULL;
static volatile bool playing = false;

// 宏录制模式标志（0x0E 控制；RAM 生效不持久化，模式/Profile 切换自动退出）
static volatile bool recording = false;

// [EVT] 统一输出：串口 printf + WiFi 模式推 TCP（与 main.c evt_send 同一约定）
static void macro_evt(const char *fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    net_input_send_line(buf);
}

// 单步骤按下/松开的执行与事件输出
// is_wifi: WiFi 模式只发事件不发本地 HID
static void step_key(bool down, uint8_t keycode, bool is_wifi) {
    if (keycode >= KC_LCTRL && keycode <= KC_RGUI) {
        uint8_t mod = (uint8_t)(1U << (keycode - KC_LCTRL));
        if (!is_wifi) {
            hid_manager_send_key(0, mod, down);
        }
    } else {
        if (!is_wifi) {
            hid_manager_send_key(keycode, 0, down);
        }
    }
    macro_evt("[EVT] MACRO %s 0x%02X %s", down ? "KD" : "KU", keycode,
              keymap_get_key_name(keycode));
}

// Consumer 步骤：脉冲语义（按下→20ms→释放），事件行 [EVT] MACRO CC <名称>
static void step_consumer(uint8_t keycode, bool is_wifi) {
    uint8_t fidx = keycode - KC_CONSUMER_BASE;
    if (!is_wifi) {
        hid_manager_send_consumer_pulse(fidx);
    }
    macro_evt("[EVT] MACRO CC %s", keymap_get_key_name(keycode));
}

static void macro_play(const macro_t *m) {
    bool is_wifi = (hid_manager_get_mode() == HID_MODE_WIFI);
    playing = true;
    ESP_LOGI(TAG, "Playing macro \"%s\" (%d steps, mode=%s)",
             m->name, m->step_count, is_wifi ? "WIFI" : "LOCAL");

    // 播放期间按住的键（含修饰键），结束时统一释放防卡键
    uint8_t held[MACRO_STEP_MAX];
    uint8_t held_count = 0;

    for (uint8_t i = 0; i < m->step_count && i < MACRO_STEP_MAX; i++) {
        const macro_step_t *s = &m->steps[i];
        switch (s->action) {
        case MACRO_ACT_DOWN:
            if (s->keycode >= KC_CONSUMER_BASE && s->keycode <= KC_CONSUMER_END) {
                step_consumer(s->keycode, is_wifi);   // Consumer 无保持语义，按脉冲处理
            } else {
                step_key(true, s->keycode, is_wifi);
                if (held_count < MACRO_STEP_MAX) {
                    held[held_count++] = s->keycode;
                }
            }
            break;
        case MACRO_ACT_UP:
            if (s->keycode >= KC_CONSUMER_BASE && s->keycode <= KC_CONSUMER_END) {
                break;   // Consumer 步骤无保持，UP 无操作
            }
            step_key(false, s->keycode, is_wifi);
            break;
        case MACRO_ACT_DELAY: {
            uint16_t ms = s->delay_ms ? s->delay_ms : 1;
            if (ms > 60000) ms = 60000;
            macro_evt("[EVT] MACRO DL %u", ms);
            // 长延时按 1s 分段休眠并喂狗（macro_task 已注册看门狗，单次 vTaskDelay 不得超过 5s）
            uint16_t remaining = ms;
            while (remaining > 0) {
                uint16_t chunk = remaining > 1000 ? 1000 : remaining;
                vTaskDelay(pdMS_TO_TICKS(chunk));
                remaining -= chunk;
                esp_task_wdt_reset();
            }
            break;
        }
        case MACRO_ACT_TAP:
            if (s->keycode >= KC_CONSUMER_BASE && s->keycode <= KC_CONSUMER_END) {
                step_consumer(s->keycode, is_wifi);
            } else {
                step_key(true, s->keycode, is_wifi);
                vTaskDelay(pdMS_TO_TICKS(30));
                step_key(false, s->keycode, is_wifi);
            }
            break;
        default:
            ESP_LOGW(TAG, "Unknown action %d at step %d", s->action, i);
            break;
        }
    }

    // 收尾：释放播放期间按住的全部键（逆序），清残留防卡键
    for (int i = held_count - 1; i >= 0; i--) {
        step_key(false, held[i], is_wifi);
    }
    macro_evt("[EVT] MACRO END");
    playing = false;
}

static void macro_task(void *arg) {
    uint8_t idx;
    esp_task_wdt_add(NULL);   // 注册任务级看门狗（5s 超时 → panic → coredump，P7）
    while (1) {
        // 原为 portMAX_DELAY：注册看门狗后必须改为有界等待以便周期喂狗
        if (xQueueReceive(macro_queue, &idx, pdMS_TO_TICKS(1000)) == pdTRUE) {
            macro_t *m = profile_get_macro(idx);
            if (m != NULL && m->step_count > 0) {
                macro_play(m);
            } else {
                // 空宏/未配置：输出诊断事件行（上位机日志可见，便于排查"按了没反应"）
                ESP_LOGW(TAG, "Macro %d empty or unavailable", idx + 1);
                macro_evt("[EVT] MACRO EMPTY %d", idx + 1);
            }
        }
        // 空闲路径喂狗：xQueueReceive 超时只是让出 CPU，不喂狗则 5s 后 WDT 必杀（2026-10-10 重启循环根因）
        esp_task_wdt_reset();
    }
}

void macro_engine_init(void) {
    macro_queue = xQueueCreate(QUEUE_SIZE_MACRO, sizeof(uint8_t));
    if (macro_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create macro queue");
        return;
    }
    xTaskCreate(macro_task, "macro_task", TASK_STACK_MACRO, NULL, TASK_PRIORITY_MACRO, NULL);
    ESP_LOGD(TAG, "Macro engine initialized");
}

bool macro_engine_trigger(uint8_t idx) {
    if (idx >= MACRO_MAX || macro_queue == NULL) {
        return false;
    }
    if (playing) {
        ESP_LOGW(TAG, "Macro trigger dropped: already playing (idx=%d)", idx + 1);
        return false;
    }
    if (xQueueSend(macro_queue, &idx, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Macro trigger dropped: queue busy (idx=%d)", idx + 1);
        return false;
    }
    return true;
}

bool macro_engine_is_playing(void) {
    return playing;
}

bool macro_rec_start(void) {
    if (recording) {
        return false;
    }
    recording = true;
    ESP_LOGI(TAG, "Recording mode ON (keys suppressed, [EVT] KEY only)");
    macro_evt("[EVT] REC START");
    return true;
}

bool macro_rec_stop(void) {
    if (!recording) {
        return false;
    }
    recording = false;
    ESP_LOGI(TAG, "Recording mode OFF");
    macro_evt("[EVT] REC STOP");
    return true;
}

bool macro_recording(void) {
    return recording;
}
