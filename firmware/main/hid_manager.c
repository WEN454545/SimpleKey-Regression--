/**
 * @file hid_manager.c
 * @brief ESP32-S3 三模键盘 - HID 管理器
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 *
 * 统一管理 USB 和 BLE HID 设备
 * 延迟初始化、动态切换、错误恢复
 */

#include "hid_manager.h"
#include "usb_hid.h"
#include "ble_hid.h"
#include "wifi_ap.h"
#include "net_input.h"
#include "led_driver.h"
#include "pin_config.h"
#include "tasks_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "HID_MGR";

// ==================== 键盘报告状态机（6KRO + 修饰键，QMK 式） ====================
//
// 修复「按住 A 再按 B，A 被判为松开」：此前每次按键事件都发送一份只含当前键的
// 全新报告（usb_hid_send_key / ble_hid_send_key），多键同按必然互相覆盖。
// 现改为维护「当前按住键集合」：事件到来时增删集合成员，再重发完整报告
// （参考 QMK keyboard_report：8 位修饰键位域 + 6 个普通键槽）。
//
// 注意：main_task 是 send_key 的唯一调用者，状态无需加锁；模式切换时整体清空。

typedef struct {
    uint8_t modifiers;          // 修饰键位域（按下置位、释放清位）
    uint8_t keys[6];            // 普通键槽，0x00=空槽
} kb_state_t;

static kb_state_t kb_state;

static void kb_state_clear(void) {
    memset(&kb_state, 0, sizeof(kb_state));
}

// Consumer 按住位域刷新（定义在后，switch_mode 中先行使用）
static uint8_t consumer_bits;      // Consumer 按住位域（定义见下方 Consumer 路由段，P6）
static esp_err_t consumer_flush(void);

static hid_manager_state_t hid_state = {
    .current_mode = HID_MODE_NONE,
    .previous_mode = HID_MODE_NONE,
    .usb_initialized = false,
    .ble_initialized = false,
    .switching = false,
};

// 把当前状态组装成标准 8 字节 Boot 键盘报告并按当前通路发送
static esp_err_t kb_state_flush(void) {
    if (hid_state.current_mode == HID_MODE_USB) {
        usb_hid_keyboard_report_t rep = {
            .modifiers = kb_state.modifiers,
            .reserved = 0,
        };
        memcpy(rep.keys, kb_state.keys, sizeof(rep.keys));
        return usb_hid_send_keyboard_report(&rep);
    }
    if (hid_state.current_mode == HID_MODE_BLE) {
        uint8_t rep[8] = {0};   // [0]=mods [1]=reserved [2..7]=keys
        rep[0] = kb_state.modifiers;
        memcpy(&rep[2], kb_state.keys, 6);
        return ble_hid_send_keyboard_report(rep, sizeof(rep));
    }
    // WIFI 模式不发 HID 报告：按键以 [EVT] 行经 TCP 推送，输入由上位机 SendInput 注入
    return ESP_OK;
}

static const char *mode_names[] = {
    [HID_MODE_NONE] = "NONE",
    [HID_MODE_USB]  = "USB",
    [HID_MODE_BLE]  = "BLE",
    [HID_MODE_WIFI] = "WIFI",
};

static esp_err_t init_usb(void) {
    ESP_LOGD(TAG, "Initializing USB HID...");
    esp_err_t ret = usb_hid_init();
    if (ret == ESP_OK) {
        hid_state.usb_initialized = true;
    } else {
        ESP_LOGE(TAG, "USB HID init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t deinit_usb(void) {
    ESP_LOGD(TAG, "Deinitializing USB HID...");
    esp_err_t ret = usb_hid_deinit();
    if (ret == ESP_OK) {
        hid_state.usb_initialized = false;
    }
    return ret;
}

static esp_err_t init_ble(void) {
    ESP_LOGD(TAG, "Initializing BLE HID...");
    esp_err_t ret = ble_hid_init();
    if (ret == ESP_OK) {
        hid_state.ble_initialized = true;
    } else {
        ESP_LOGE(TAG, "BLE HID init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t deinit_ble(void) {
    ESP_LOGD(TAG, "Deinitializing BLE HID...");
    esp_err_t ret = ble_hid_deinit();
    if (ret == ESP_OK) {
        hid_state.ble_initialized = false;
    }
    return ret;
}

static esp_err_t init_wifi(void) {
    // WiFi 模式：SoftAP + TCP 服务（输入走 [EVT]→TCP→上位机 SendInput，无本地 HID）
    ESP_LOGD(TAG, "Initializing WiFi AP...");
    esp_err_t ret = wifi_ap_start();
    if (ret == ESP_OK) {
        net_input_start();
    } else {
        ESP_LOGE(TAG, "WiFi AP init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t deinit_wifi(void) {
    ESP_LOGD(TAG, "Deinitializing WiFi AP...");
    net_input_stop();
    return wifi_ap_stop();
}

void hid_manager_init(void) {
    ESP_LOGD(TAG, "Initializing HID manager...");
    hid_state.current_mode = HID_MODE_NONE;
    hid_state.previous_mode = HID_MODE_NONE;
    hid_state.switching = false;
}

esp_err_t hid_manager_switch_mode(hid_mode_t new_mode) {
    if (new_mode == hid_state.current_mode) {
        ESP_LOGI(TAG, "Already in %s mode", mode_names[new_mode]);
        return ESP_OK;
    }

    if (hid_state.switching) {
        ESP_LOGW(TAG, "Mode switch in progress");
        return ESP_ERR_INVALID_STATE;
    }

    hid_state.switching = true;
    ESP_LOGD(TAG, "Switching: %s -> %s", mode_names[hid_state.current_mode], mode_names[new_mode]);

    if (hid_state.current_mode == HID_MODE_USB) {
        deinit_usb();
    } else if (hid_state.current_mode == HID_MODE_BLE) {
        deinit_ble();
    } else if (hid_state.current_mode == HID_MODE_WIFI) {
        deinit_wifi();
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t ret = ESP_FAIL;

    if (new_mode == HID_MODE_USB) {
        ret = init_usb();
    } else if (new_mode == HID_MODE_BLE) {
        ret = init_ble();
    } else if (new_mode == HID_MODE_WIFI) {
        ret = init_wifi();
    } else {
        hid_state.switching = false;
        hid_state.current_mode = HID_MODE_NONE;
        return ESP_OK;
    }

    if (ret == ESP_OK) {
        hid_state.previous_mode = hid_state.current_mode;
        hid_state.current_mode = new_mode;
        ESP_LOGI(TAG, "Mode -> %s", mode_names[new_mode]);

        // 按住键集合随通路切换整体失效：清空并尽力发送一次全零报告，
        // 防止旧通路残留的按住键在新主机/新连接上表现为「按键卡住」
        kb_state_clear();
        kb_state_flush();
        consumer_bits = 0;      // Consumer 按住位域同步清空（P6）
        consumer_flush();

        // LED 跟随模式：快闪 3 次后自动恢复为新模式颜色常亮
        led_color_t color = LED_COLOR_NONE;
        if (new_mode == HID_MODE_USB) {
            color = LED_COLOR_USB;
        } else if (new_mode == HID_MODE_BLE) {
            color = LED_COLOR_BLE;
        } else if (new_mode == HID_MODE_WIFI) {
            color = LED_COLOR_2G4;      // 绿色 = WiFi 模式
        }
        if (color != LED_COLOR_NONE) {
            led_blink_n(color, LED_BLINK_COUNT);
        }
    } else {
        ESP_LOGW(TAG, "Switch failed, trying to restore previous mode");
        if (hid_state.current_mode == HID_MODE_USB) {
            init_usb();
        } else if (hid_state.current_mode == HID_MODE_BLE) {
            init_ble();
        } else if (hid_state.current_mode == HID_MODE_WIFI) {
            init_wifi();
        }
    }

    hid_state.switching = false;
    return ret;
}

hid_mode_t hid_manager_get_mode(void) {
    return hid_state.current_mode;
}

void hid_manager_get_state(hid_manager_state_t *state) {
    if (state) {
        *state = hid_state;
    }
}

esp_err_t hid_manager_send_key(uint8_t key_code, uint8_t modifiers, bool pressed) {
    if (hid_state.switching) {
        return ESP_ERR_INVALID_STATE;
    }
    if (hid_state.current_mode != HID_MODE_USB && hid_state.current_mode != HID_MODE_BLE &&
        hid_state.current_mode != HID_MODE_WIFI) {
        return ESP_ERR_INVALID_STATE;
    }

    // 更新按住键集合（QMK 式：修饰键位域 + 6 键槽增删）
    bool changed = false;
    if (pressed) {
        if (modifiers) {
            kb_state.modifiers |= modifiers;
            changed = true;
        }
        if (key_code != 0x00) {
            bool present = false;
            for (int i = 0; i < 6; i++) {
                if (kb_state.keys[i] == key_code) { present = true; break; }
            }
            if (!present) {
                int slot = -1;
                for (int i = 0; i < 6; i++) {
                    if (kb_state.keys[i] == 0x00) { slot = i; break; }
                }
                if (slot >= 0) {
                    kb_state.keys[slot] = key_code;
                    changed = true;
                } else {
                    // 6KRO 满载：丢弃第 7 键（完整 NKRO 需改报告描述符，暂不引入）
                    ESP_LOGW(TAG, "6-key rollover full, key 0x%02X dropped", key_code);
                }
            }
        }
    } else {
        if (modifiers) {
            kb_state.modifiers &= (uint8_t)~modifiers;
            changed = true;
        }
        if (key_code != 0x00) {
            for (int i = 0; i < 6; i++) {
                if (kb_state.keys[i] == key_code) {
                    kb_state.keys[i] = 0x00;
                    changed = true;
                }
            }
        }
    }

    // 无变化不重发（避免重复事件刷报告）
    if (!changed) {
        return ESP_OK;
    }
    return kb_state_flush();
}

esp_err_t hid_manager_send_volume(bool volume_up) {
    if (hid_state.switching) {
        return ESP_ERR_INVALID_STATE;
    }

    if (hid_state.current_mode == HID_MODE_USB) {
        return usb_hid_send_volume(volume_up);
    } else if (hid_state.current_mode == HID_MODE_BLE) {
        return ble_hid_send_volume(volume_up);
    } else if (hid_state.current_mode == HID_MODE_WIFI) {
        // 音量事件同样经 [EVT] ENC 行推送上位机注入（main.c 统一处理），本地无 HID
        return ESP_OK;
    }

    return ESP_ERR_INVALID_STATE;
}

// ==================== Consumer 功能路由（P6） ====================
//
// 键位值域 0xC0~0xCF 与旋钮可配置输出的统一出口：维护一个按住位域
// （bit0~bit7 与报告描述符 Usage 一一对应），有变化才发报告。
// 与 kb_state 同为 main_task / macro 任务串行调用，无需加锁。

static uint8_t consumer_bits = 0;   // 当前按住的 Consumer 功能位域

static esp_err_t consumer_flush(void) {
    if (hid_state.current_mode == HID_MODE_USB) {
        usb_hid_consumer_report_t rep = { .buttons = consumer_bits };
        return usb_hid_send_consumer_report(&rep);
    }
    if (hid_state.current_mode == HID_MODE_BLE) {
        uint8_t rep[1] = { consumer_bits };   // BLE 通知发原始报告（无 Report ID 前缀）
        return ble_hid_send_consumer_report(rep, sizeof(rep));
    }
    // WiFi 模式不发本地 HID：事件经 [EVT] 行推送，上位机 SendInput 注入
    return ESP_OK;
}

esp_err_t hid_manager_send_consumer(uint8_t func_idx, bool pressed) {
    if (func_idx >= 8) {
        return ESP_ERR_INVALID_ARG;
    }
    if (hid_state.switching) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t bit = (uint8_t)(1U << func_idx);
    bool is_set = (consumer_bits & bit) != 0;
    if (pressed == is_set) {
        return ESP_OK;   // 无变化不重发
    }
    if (pressed) {
        consumer_bits |= bit;
    } else {
        consumer_bits &= (uint8_t)~bit;
    }
    return consumer_flush();
}

esp_err_t hid_manager_send_consumer_pulse(uint8_t func_idx) {
    esp_err_t ret = hid_manager_send_consumer(func_idx, true);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(20));  // 脉冲宽度：确保主机识别一次按压
    return hid_manager_send_consumer(func_idx, false);
}

bool hid_manager_is_connected(void) {
    if (hid_state.current_mode == HID_MODE_USB) {
        return usb_hid_is_connected();
    } else if (hid_state.current_mode == HID_MODE_BLE) {
        return ble_hid_is_connected();
    } else if (hid_state.current_mode == HID_MODE_WIFI) {
        return net_input_has_client();  // 以 TCP 客户端在线为「已连接」
    }
    return false;
}

const char *hid_manager_get_mode_name(hid_mode_t mode) {
    if (mode <= HID_MODE_WIFI) {
        return mode_names[mode];
    }
    return "UNKNOWN";
}
