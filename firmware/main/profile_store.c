/**
 * @file profile_store.c
 * @brief ESP32-S3 三模键盘 - Profile 存储
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 *
 * Profile 管理，NVS 持久化存储
 */

#include "profile_store.h"
#include "keymap.h"
#include "led_driver.h"
#include "hid_manager.h"
#include "tasks_config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "PROFILE";

static profile_t profiles[PROFILE_COUNT] = {0};
static uint8_t current_index = 0;
static nvs_handle_t nvs_profile_handle = 0;

// 旧版 Profile 结构（v1，NVS key "data"）：仅用于迁移读取，勿改动字段
typedef struct {
    char name[PROFILE_NAME_LEN];
    uint8_t keymap[5][4];
    uint8_t default_mode;
    bool valid;
} profile_legacy_t;

// 旧版 Profile 结构（v2，NVS key "data2"，P6：589B）：仅用于迁移读取，勿改动字段
typedef struct __attribute__((packed)) {
    char name[PROFILE_NAME_LEN];
    uint8_t keymap[5][4];
    uint8_t default_mode;
    uint8_t knob_cw;
    uint8_t knob_ccw;
    uint8_t version;
    bool valid;
    macro_t macros[MACRO_MAX];
} profile_v2_legacy_t;

#define PROFILE_STRUCT_VERSION  0x03
#define NVS_KEY_DATA_V3         "data3"     // v3 数据 key（旧 "data2"/"data" 保留只读作回滚备份）
#define NVS_KEY_DATA_V2_LEGACY  "data2"     // v2 旧 key（迁移源）

// 默认 Profile
static const profile_t default_profile = {
    .name = "Default",
    .keymap = {
        {KC_1, KC_2, KC_3, KC_4},
        {KC_Q, KC_W, KC_E, KC_R},
        {KC_A, KC_S, KC_D, KC_F},
        {KC_Z, KC_X, KC_C, KC_V},
        {KC_LCTRL, KC_LSHIFT, KC_SPACE, KC_ENTER},
    },
    .default_mode = HID_MODE_USB,  // 默认 USB（插 USB 即用；切 BLE 后持久化为 BLE）
    .knob_cw = KC_VOL_UP,          // 旋钮默认音量加减（行为与 P5 前一致）
    .knob_ccw = KC_VOL_DOWN,
    .knob_press = KNOB_FUNC_PROFILE,  // 短按默认循环切 Profile（v1.9 起可配）
    .version = PROFILE_STRUCT_VERSION,
    .valid = true,
};

// 用默认值填充 P6+ 新增字段（v1/v2 迁移与恢复出厂共用）
static void profile_fill_p6_defaults(profile_t *p) {
    p->knob_cw = KC_VOL_UP;
    p->knob_ccw = KC_VOL_DOWN;
    p->knob_press = KNOB_FUNC_PROFILE;
    p->version = PROFILE_STRUCT_VERSION;
    memset(p->macros, 0, sizeof(p->macros));
}

static esp_err_t nvs_load_profiles(void) {
    esp_err_t ret = nvs_open("profiles", NVS_READWRITE, &nvs_profile_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed, using defaults");
        return ret;
    }

    size_t size = sizeof(profiles);
    ret = nvs_get_blob(nvs_profile_handle, NVS_KEY_DATA_V3, profiles, &size);
    if (ret != ESP_OK) {
        // v3 数据不存在：先尝试从 v2（P6，"data2"）迁移——键位/名称/模式/宏整体保留，
        // knob_press 填默认（短按=切 Profile，行为向后兼容）
        profile_v2_legacy_t v2[PROFILE_COUNT];
        size_t v2_size = sizeof(v2);
        if (nvs_get_blob(nvs_profile_handle, NVS_KEY_DATA_V2_LEGACY, v2, &v2_size) == ESP_OK) {
            for (int i = 0; i < PROFILE_COUNT; i++) {
                memset(&profiles[i], 0, sizeof(profile_t));
                memcpy(profiles[i].name, v2[i].name, PROFILE_NAME_LEN);
                memcpy(profiles[i].keymap, v2[i].keymap, sizeof(profiles[i].keymap));
                profiles[i].default_mode = v2[i].default_mode;
                profiles[i].knob_cw = v2[i].knob_cw;
                profiles[i].knob_ccw = v2[i].knob_ccw;
                profiles[i].valid = v2[i].valid;
                memcpy(profiles[i].macros, v2[i].macros, sizeof(profiles[i].macros));
                profiles[i].knob_press = KNOB_FUNC_PROFILE;
                profiles[i].version = PROFILE_STRUCT_VERSION;
            }
            nvs_set_blob(nvs_profile_handle, NVS_KEY_DATA_V3, profiles, sizeof(profiles));
            nvs_commit(nvs_profile_handle);
            ESP_LOGI(TAG, "Migrated profiles v2 -> v3 (old data2 kept as rollback)");
        } else {
            // v2 也不存在：尝试从最旧 v1（"data"）迁移（键位/名称/默认模式保留，宏与旋钮填默认）
            profile_legacy_t legacy[PROFILE_COUNT];
            size_t legacy_size = sizeof(legacy);
            if (nvs_get_blob(nvs_profile_handle, "data", legacy, &legacy_size) == ESP_OK) {
                for (int i = 0; i < PROFILE_COUNT; i++) {
                    memset(&profiles[i], 0, sizeof(profile_t));
                    memcpy(profiles[i].name, legacy[i].name, PROFILE_NAME_LEN);
                    memcpy(profiles[i].keymap, legacy[i].keymap, sizeof(profiles[i].keymap));
                    profiles[i].default_mode = legacy[i].default_mode;
                    profiles[i].valid = legacy[i].valid;
                    profile_fill_p6_defaults(&profiles[i]);
                }
                nvs_set_blob(nvs_profile_handle, NVS_KEY_DATA_V3, profiles, sizeof(profiles));
                nvs_commit(nvs_profile_handle);
                ESP_LOGI(TAG, "Migrated profiles v1 -> v3 (old data kept as rollback)");
            } else {
                ESP_LOGI(TAG, "No profiles in NVS, loading defaults");
                for (int i = 0; i < PROFILE_COUNT; i++) {
                    profiles[i] = default_profile;
                    snprintf(profiles[i].name, PROFILE_NAME_LEN, "Profile %d", i + 1);
                }
            }
        }
    }

    uint8_t idx = 0;
    size = sizeof(idx);
    nvs_get_u8(nvs_profile_handle, "current", &idx);
    if (idx < PROFILE_COUNT) {
        current_index = idx;
    }

    return ESP_OK;
}

static void nvs_save_profiles(void) {
    if (nvs_profile_handle == 0) return;
    nvs_set_blob(nvs_profile_handle, NVS_KEY_DATA_V3, profiles, sizeof(profiles));
    nvs_set_u8(nvs_profile_handle, "current", current_index);
    nvs_commit(nvs_profile_handle);
}

void profile_init(void) {
    ESP_LOGI(TAG, "Initializing profiles...");
    nvs_load_profiles();
    keymap_init();
    keymap_load_profile(current_index);
    ESP_LOGI(TAG, "Profiles initialized, current: %d (%s)", current_index + 1, profiles[current_index].name);
}

profile_t *profile_get_current(void) {
    return &profiles[current_index];
}

const profile_t *profile_get_slot(uint8_t index) {
    return (index < PROFILE_COUNT) ? &profiles[index] : NULL;
}

uint8_t profile_get_current_index(void) {
    return current_index;
}

// 指示灯颜色跟随当前连接模式（USB=白 / BLE=蓝 / WiFi=绿；仅长按切模式时才用目标模式颜色）
static led_color_t led_color_current_mode(void) {
    hid_mode_t mode = hid_manager_get_mode();
    return (mode == HID_MODE_USB) ? LED_COLOR_USB :
           (mode == HID_MODE_BLE) ? LED_COLOR_BLE : LED_COLOR_2G4;
}

void profile_switch_next(void) {
    uint8_t old_index = current_index;
    current_index = (current_index + 1) % PROFILE_COUNT;
    ESP_LOGI(TAG, "Switching profile: %d -> %d (%s)", old_index + 1, current_index + 1, profiles[current_index].name);

    keymap_load_profile(current_index);
    nvs_save_profiles();
    led_blink_n(led_color_current_mode(), current_index + 1);
}

void profile_set_current(uint8_t index) {
    if (index < PROFILE_COUNT) {
        current_index = index;
        keymap_load_profile(current_index);
        nvs_save_profiles();
        led_blink_n(led_color_current_mode(), index + 1);
    }
}

bool profile_write_slot(uint8_t index, const char *name, const uint8_t *keymap) {
    if (index >= PROFILE_COUNT || name == NULL || keymap == NULL) {
        return false;
    }
    snprintf(profiles[index].name, PROFILE_NAME_LEN, "%s", name);
    memcpy(profiles[index].keymap, keymap, sizeof(profiles[index].keymap));
    profiles[index].valid = true;
    nvs_save_profiles();
    // 写的是当前 Profile：重载键位使写入立即生效
    if (index == current_index) {
        keymap_load_profile(current_index);
    }
    ESP_LOGI(TAG, "Profile %d written: \"%s\"", index + 1, profiles[index].name);
    return true;
}

uint8_t profile_get_default_mode(void) {
    return profiles[current_index].default_mode;
}

void profile_set_default_mode(uint8_t mode) {
    if (mode < (uint8_t)HID_MODE_USB || mode > (uint8_t)HID_MODE_WIFI) {
        ESP_LOGW(TAG, "Invalid default mode: %d", mode);
        return;
    }
    profiles[current_index].default_mode = mode;
    nvs_save_profiles();
    ESP_LOGI(TAG, "Default mode saved: %d", mode);
}

void profile_save(void) {
    nvs_save_profiles();
    ESP_LOGI(TAG, "Profiles saved");
}

// ==================== 宏与旋钮配置（P6） ====================

macro_t *profile_get_macro(uint8_t idx) {
    if (idx >= MACRO_MAX) {
        return NULL;
    }
    return &profiles[current_index].macros[idx];
}

bool profile_set_macro(uint8_t idx, const macro_t *m) {
    if (idx >= MACRO_MAX || m == NULL) {
        return false;
    }
    profiles[current_index].macros[idx] = *m;
    // 步数越界防御：钳制到容量上限
    if (profiles[current_index].macros[idx].step_count > MACRO_STEP_MAX) {
        profiles[current_index].macros[idx].step_count = MACRO_STEP_MAX;
    }
    ESP_LOGI(TAG, "Macro %d set: \"%s\" (%d steps)", idx + 1,
             profiles[current_index].macros[idx].name,
             profiles[current_index].macros[idx].step_count);
    return true;
}

void profile_get_knob(uint8_t *cw, uint8_t *ccw, uint8_t *press) {
    if (cw)    *cw    = profiles[current_index].knob_cw;
    if (ccw)   *ccw   = profiles[current_index].knob_ccw;
    if (press) *press = profiles[current_index].knob_press;
}

// 短按功能码校验：Consumer 0xC0~0xCF / 宏触发 0xF0~0xF3 / 切 Profile 0xFE
static bool knob_press_valid(uint8_t press) {
    if (press == KNOB_FUNC_PROFILE) return true;
    if (press >= KC_MACRO_BASE && press <= KC_MACRO_BASE + MACRO_MAX - 1) return true;
    return (press >= KC_CONSUMER_BASE && press <= KC_CONSUMER_END);
}

bool profile_set_knob(uint8_t cw, uint8_t ccw, uint8_t press) {
    if (cw < KC_CONSUMER_BASE || cw > KC_CONSUMER_END ||
        ccw < KC_CONSUMER_BASE || ccw > KC_CONSUMER_END ||
        !knob_press_valid(press)) {
        ESP_LOGW(TAG, "Invalid knob config: cw=0x%02X ccw=0x%02X press=0x%02X", cw, ccw, press);
        return false;
    }
    profiles[current_index].knob_cw = cw;
    profiles[current_index].knob_ccw = ccw;
    profiles[current_index].knob_press = press;
    ESP_LOGI(TAG, "Knob config saved: cw=0x%02X ccw=0x%02X press=0x%02X", cw, ccw, press);
    return true;
}

void profile_factory_reset(void) {
    ESP_LOGI(TAG, "Factory reset profiles");
    for (int i = 0; i < PROFILE_COUNT; i++) {
        profiles[i] = default_profile;
        snprintf(profiles[i].name, PROFILE_NAME_LEN, "Profile %d", i + 1);
    }
    current_index = 0;
    keymap_load_profile(current_index);
    nvs_save_profiles();
}

void profile_print(void) {
    ESP_LOGI(TAG, "Profiles:");
    for (int i = 0; i < PROFILE_COUNT; i++) {
        printf("  [%d] %s %s\n", i + 1, profiles[i].name, (i == current_index) ? "(active)" : "");
    }
}
