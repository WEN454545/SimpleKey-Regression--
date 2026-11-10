/**
 * @file main.c
 * @brief ESP32-S3 三模键盘 - 主程序
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 *
 * 系统入口，初始化各模块，创建任务
 *
 * 初始化顺序：
 * 1. NVS Flash
 * 2. GPIO
 * 3. 定时器
 * 4. 矩阵扫描
 * 5. 编码器
 * 6. LED
 * 7. Profile
 * 8. 串口命令
 * 9. HID 管理器
 * 10. 根据默认模式初始化 HID
 * 11. 创建任务
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"

#include "pin_config.h"
#include "tasks_config.h"
#include "matrix_driver.h"
#include "encoder_driver.h"
#include "led_driver.h"
#include "power_mgr.h"
#include "sleep_mgr.h"
#include "keymap.h"
#include "profile_store.h"
#include "macro.h"
#include "serial_cmd.h"
#include "hid_manager.h"
#include "ble_hid.h"
#include "net_input.h"
#include <stdarg.h>

static const char *TAG = "MAIN";

// 长按计时器
static int64_t button_press_start = 0;
static bool button_long_press_handled = false;

/**
 * @brief 结构化事件统一输出：串口 printf + WiFi 模式下推送 TCP（上位机 SendInput 注入源）
 *
 * WiFi 模式的按键/旋钮输入完全依赖 [EVT] 行经 TCP 推送（P5 方案 A），
 * 故所有 [EVT] 输出必须同时走两条通路
 */
static void evt_send(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    net_input_send_line(buf);   // 无客户端/WiFi 未启动时内部静默丢弃
}

/**
 * @brief 处理编码器事件
 */
static void process_encoder_events(void) {
    encoder_event_t event;
    void *queue = encoder_get_event_queue();

    if (queue == NULL) return;

    while (xQueueReceive(queue, &event, 0) == pdTRUE) {
        sleep_mgr_activity();   // 输入活动：刷新睡眠监控时间戳（P7）
        if (event.type == ENCODER_EVENT_ROTATION) {
            // 旋转事件：按当前 Profile 的旋钮配置输出（P6，默认音量加减行为不变）
            uint8_t knob_cw, knob_ccw, knob_press;
            profile_get_knob(&knob_cw, &knob_ccw, &knob_press);
            uint8_t func_kc = event.delta > 0 ? knob_cw : knob_ccw;
            int steps = event.delta > 0 ? event.delta : -event.delta;
            bool default_volume = (func_kc == KC_VOL_UP || func_kc == KC_VOL_DOWN);

            for (int i = 0; i < steps; i++) {
                hid_manager_send_consumer_pulse(func_kc - KC_CONSUMER_BASE);
                if (i < steps - 1) {
                    vTaskDelay(pdMS_TO_TICKS(10));  // 步进间留间隔，确保主机逐次识别
                }
            }

            if (default_volume) {
                // 默认音量输出：保持既有 [EVT] ENC 行语义（上位机按 ENC CW/CCW 注入音量）
                evt_send("[EVT] ENC %s %d", event.delta > 0 ? "CW" : "CCW", event.delta > 0 ? event.delta : -event.delta);
            } else {
                // 非默认输出：以 [EVT] CC 行推送实际功能（上位机按功能注入）
                for (int i = 0; i < steps; i++) {
                    evt_send("[EVT] CC %s", keymap_get_key_name(func_kc));
                }
            }
            // BLE 监控推送：0x02=CW / 0x03=CCW
            uint8_t mon[3] = {event.delta > 0 ? 0x02 : 0x03, 0, (uint8_t)(event.delta > 0 ? event.delta : -event.delta)};
            ble_hid_send_monitor(mon, sizeof(mon));
            ESP_LOGV(TAG, "Encoder rotation: %d", event.delta);
        } else if (event.type == ENCODER_EVENT_BUTTON) {
            if (event.button_state) {
                // 按下
                button_press_start = event.timestamp;
                button_long_press_handled = false;
                evt_send("[EVT] ENC PRESS");
                uint8_t mon[1] = {0x04};  // 0x04=按下
                ble_hid_send_monitor(mon, sizeof(mon));
                ESP_LOGD(TAG, "Encoder button pressed");
            } else {
                // 释放
                int64_t press_duration = event.timestamp - button_press_start;
                evt_send("[EVT] ENC RELEASE");
                uint8_t mon[1] = {0x05};  // 0x05=释放
                ble_hid_send_monitor(mon, sizeof(mon));
                ESP_LOGD(TAG, "Encoder button released, duration: %lld us", press_duration);

                if (!button_long_press_handled && press_duration < (ENCODER_LONG_PRESS_MS * 1000)) {
                    // 短按：按 Profile 的短按配置分发（v1.9；默认 0xFE=循环切换 Profile，行为与旧版一致）
                    uint8_t knob_cw, knob_ccw, knob_press;
                    profile_get_knob(&knob_cw, &knob_ccw, &knob_press);

                    if (knob_press >= KC_MACRO_BASE && knob_press <= KC_MACRO_BASE + MACRO_MAX - 1) {
                        // 短按触发宏（按下沿语义在宏引擎内部按一次触发处理）
                        ESP_LOGI(TAG, "Short press: triggering macro %d", knob_press - KC_MACRO_BASE + 1);
                        macro_engine_trigger(knob_press - KC_MACRO_BASE);
                    } else if (knob_press >= KC_CONSUMER_BASE && knob_press <= KC_CONSUMER_END) {
                        // 短按输出 Consumer 功能（脉冲语义；WiFi 模式经 [EVT] CC 行注入）
                        ESP_LOGI(TAG, "Short press: consumer %s", keymap_get_key_name(knob_press));
                        hid_manager_send_consumer_pulse(knob_press - KC_CONSUMER_BASE);
                        evt_send("[EVT] CC %s", keymap_get_key_name(knob_press));
                        uint8_t mon[2] = {0x09, knob_press};  // 0x09=短按 Consumer 输出
                        ble_hid_send_monitor(mon, sizeof(mon));
                    } else {
                        // 默认：循环切换 Profile
                        ESP_LOGI(TAG, "Short press: switching profile");
                        profile_switch_next();
                        macro_rec_stop();   // Profile 切换自动退出录制
                        // 格式：[EVT] PROFILE N（1-3）
                        evt_send("[EVT] PROFILE %d", profile_get_current_index() + 1);
                        uint8_t mon[2] = {0x08, (uint8_t)(profile_get_current_index() + 1)};  // 0x08=Profile 切换
                        ble_hid_send_monitor(mon, sizeof(mon));
                    }
                }

                button_press_start = 0;
            }
        }
    }

    // 检查长按
    if (button_press_start > 0 && !button_long_press_handled) {
        int64_t current_time = esp_timer_get_time();
        int64_t press_duration = current_time - button_press_start;

        if (press_duration >= (ENCODER_LONG_PRESS_MS * 1000)) {
            // 长按：切换模式
            ESP_LOGI(TAG, "Long press: switching mode");
            button_long_press_handled = true;
            evt_send("[EVT] ENC LONG");
            uint8_t mon[1] = {0x06};  // 0x06=长按
            ble_hid_send_monitor(mon, sizeof(mon));

            // 三模循环：USB -> BLE -> WiFi（P5 方案 A）
            hid_mode_t current = hid_manager_get_mode();
            hid_mode_t next;
            if (current == HID_MODE_USB) {
                next = HID_MODE_BLE;
            } else if (current == HID_MODE_BLE) {
                next = HID_MODE_WIFI;
            } else {
                next = HID_MODE_USB;
            }
            if (hid_manager_switch_mode(next)) {
                macro_rec_stop();   // 模式切换自动退出录制（防录制标志跨模式残留）
                profile_set_default_mode((uint8_t)next);  // 持久化：重启后回到该模式
                // 格式：[EVT] MODE usb|ble|wifi
                evt_send("[EVT] MODE %s", hid_manager_get_mode_name(next));
                // 监控推送：0x07=模式切换，payload=模式序号 0=USB / 1=BLE / 2=WiFi
                uint8_t mon2[2] = {0x07, (uint8_t)next - 1};
                ble_hid_send_monitor(mon2, sizeof(mon2));

                // LED 提示
                led_color_t color = (next == HID_MODE_USB) ? LED_COLOR_USB :
                                    (next == HID_MODE_BLE) ? LED_COLOR_BLE : LED_COLOR_2G4;
                led_blink_n(color, 3);
            }
        }
    }
}

/**
 * @brief 处理按键事件
 */
static void process_key_events(void) {
    key_event_t event;
    void *queue = matrix_get_event_queue();

    if (queue == NULL) return;

    while (xQueueReceive(queue, &event, 0) == pdTRUE) {
        sleep_mgr_activity();   // 输入活动：刷新睡眠监控时间戳（P7）
        // 获取键码（事件报文需要键码与键名）
        uint8_t keycode = keymap_get_key(event.row, event.col);

        // 上报结构化矩阵事件（在键码过滤之前，供上位机测试全部键位）
        // 格式：[EVT] KEY DOWN/UP <row> <col> <keycode> <name>
        evt_send("[EVT] KEY %s %d %d 0x%02X %s",
                 event.type == KEY_EVENT_PRESSED ? "DOWN" : "UP",
                 event.row, event.col, keycode, keymap_get_key_name(keycode));
        // BLE 监控推送：0x01=按键事件
        uint8_t mon[5] = {0x01, event.type == KEY_EVENT_PRESSED ? 1 : 0, event.row, event.col, keycode};
        ble_hid_send_monitor(mon, sizeof(mon));

        if (keycode == KC_NONE || keycode == KC_TRNS) {
            continue;
        }

        // 宏录制模式：拦截一切 HID 输出（普通键/修饰键/Consumer/宏触发），
        // 按键仅保留上方 [EVT] KEY 行上报，由上位机打时间戳生成宏步骤（0x0E 录制控制）
        if (macro_recording()) {
            continue;
        }

        // Consumer 功能键（0xC0~0xCF，P6）：走 Consumer 报告路由，不进普通键 HID
        if (keycode >= KC_CONSUMER_BASE && keycode <= KC_CONSUMER_END) {
            bool pressed = (event.type == KEY_EVENT_PRESSED);
            if (pressed) {
                // 所有模式都输出 CC 行；USB/BLE 本地报告同时生效（上位机对串口/BLE 通路仅展示）
                evt_send("[EVT] CC %s", keymap_get_key_name(keycode));
            }
            if (hid_manager_get_mode() != HID_MODE_WIFI) {
                hid_manager_send_consumer(keycode - KC_CONSUMER_BASE, pressed);
            }
            continue;
        }

        // 宏触发键（0xF0~0xF3，P6）：按下沿触发宏引擎播放，原键码不再下发
        if (keycode >= KC_MACRO_BASE && keycode <= KC_MACRO_BASE + MACRO_MAX - 1) {
            if (event.type == KEY_EVENT_PRESSED) {
                macro_engine_trigger(keycode - KC_MACRO_BASE);
            }
            continue;
        }

        // 处理修饰键
        uint8_t modifiers = 0;
        if (keycode >= KC_LCTRL && keycode <= KC_RGUI) {
            modifiers = (1 << (keycode - KC_LCTRL));
            keycode = 0;
        }

        // 发送 HID 报告
        hid_manager_send_key(keycode, modifiers, event.type == KEY_EVENT_PRESSED);

        ESP_LOGD(TAG, "Key %s: row=%d col=%d key=0x%02X mod=0x%02X",
                 event.type == KEY_EVENT_PRESSED ? "PRESS" : "RELEASE",
                 event.row, event.col, keycode, modifiers);
    }
}

/**
 * @brief 主任务：处理 HID 事件
 */
static void main_task(void *arg) {
    ESP_LOGI(TAG, "Main task started");
    esp_task_wdt_add(NULL);   // 注册任务级看门狗（5s 超时 → panic → coredump，P7）

    while (1) {
        process_encoder_events();
        process_key_events();
        vTaskDelay(pdMS_TO_TICKS(5));
        esp_task_wdt_reset();
    }
}

/**
 * @brief 主函数
 */
void app_main(void) {
    ESP_LOGI(TAG, "SimpleKey Tri-mode v1.0.0 (%s %s)", __DATE__, __TIME__);

    // 压制 TinyUSB 描述符摘要表格打印（INFO 级、占屏且终端编码下乱码）
    esp_log_level_set("tusb_desc", ESP_LOG_NONE);
    esp_log_level_set("gpio", ESP_LOG_NONE);  // 压制 GPIO 初始化逐脚日志刷屏

    // 0. 深睡唤醒遗留 hold 释放（必须在任何外设 GPIO 初始化之前：
    //    RTC pad hold 不随深睡唤醒复位清除，不释放则矩阵/电池采样被锁死）
    sleep_mgr_release_wake_holds();

    // 1. NVS Flash 初始化
    ESP_LOGD(TAG, "Initializing NVS Flash...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Profile 初始化（包含 keymap）
    ESP_LOGD(TAG, "Initializing profiles...");
    profile_init();

    // 3. 矩阵扫描初始化
    ESP_LOGD(TAG, "Initializing matrix scan...");
    matrix_init();

    // 4. 编码器初始化
    ESP_LOGD(TAG, "Initializing encoder...");
    encoder_init();

    // 5. LED 初始化
    ESP_LOGD(TAG, "Initializing LEDs...");
    led_init();

    // 5.5 电源管理初始化（ADC/EN_BAT/充电检测脚）
    power_mgr_init();

    // 6. 串口命令初始化
    ESP_LOGD(TAG, "Initializing serial command...");
    serial_cmd_init();

    // 7. HID 管理器初始化
    ESP_LOGD(TAG, "Initializing HID manager...");
    hid_manager_init();

    // 7.5 宏引擎初始化（P6：播放任务+触发队列）
    macro_engine_init();

    // 8. 根据默认模式初始化 HID（默认 BLE；NVS 可存 USB/BLE/WiFi）
    hid_mode_t default_mode = (hid_mode_t)profile_get_default_mode();
    if (default_mode < HID_MODE_USB || default_mode > HID_MODE_WIFI) {
        default_mode = HID_MODE_BLE;    // NVS 越界兜底
    }
    // 上电 USB 在位优先（方案 §2.2「USB 插入时自动切换有线」的启动时点实现）：
    // CHRG_DET/STDBY_DET 任一低 = USB 在位，插线即用，覆盖 NVS 持久化模式；
    // 仅启动时点生效、不写回 NVS——拔线启动仍按原持久化模式，长按切换行为不变
    power_status_t ps;
    power_mgr_get(&ps);
    if (ps.usb_present && default_mode != HID_MODE_USB) {
        ESP_LOGI(TAG, "USB present at boot, prefer USB mode (NVS default: %s)",
                 hid_manager_get_mode_name(default_mode));
        default_mode = HID_MODE_USB;
    }
    ESP_LOGD(TAG, "Default HID mode: %s", hid_manager_get_mode_name(default_mode));
    led_set_mode(LED_COLOR_BLE, LED_MODE_BLINK_FAST);
    hid_manager_switch_mode(default_mode);

    // 9. 启动矩阵扫描
    matrix_start();

    // 10. 设置 LED 为当前模式常亮
    led_color_t led_color = (default_mode == HID_MODE_USB) ? LED_COLOR_USB :
                            (default_mode == HID_MODE_BLE) ? LED_COLOR_BLE : LED_COLOR_2G4;
    led_set_mode(led_color, LED_MODE_ON);

    // 11. 创建主任务
    xTaskCreate(main_task, "main_task", 4096, NULL, 10, NULL);

    // 11.1 创建矩阵防抖任务（ISR 只更新原始电平，防抖确认与事件入队在此任务完成；
    //      缺少此任务会导致按键事件永远不产生——[EVT] 不输出、HID 键盘无输入）
    xTaskCreate(matrix_task, "matrix_task", TASK_STACK_MATRIX, NULL, TASK_PRIORITY_MATRIX, NULL);

    // 12. 创建串口任务
    xTaskCreate(serial_task, "serial_task", TASK_STACK_SERIAL, NULL, TASK_PRIORITY_SERIAL, NULL);

    // 13. 睡眠管理器（P7：两级睡眠，需在各模块初始化完成后启动）
    sleep_mgr_init();

    ESP_LOGI(TAG, "System init complete, mode=%s", hid_manager_get_mode_name(default_mode));
}
