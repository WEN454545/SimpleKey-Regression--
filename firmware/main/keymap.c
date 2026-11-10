/**
 * @file keymap.c
 * @brief ESP32-S3 三模键盘 - 键位映射
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 *
 * 单层键位映射，参考 simplekey10.json 默认键位
 */

#include "keymap.h"
#include "profile_store.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "KEYMAP";

// 当前键位映射表
static uint8_t current_keymap[KEYMAP_ROWS][KEYMAP_COLS] = {0};

// 默认键位映射（参考 simplekey10.json 第一层）
static const uint8_t default_keymap[KEYMAP_ROWS][KEYMAP_COLS] = {
    {KC_1,     KC_2,     KC_3,     KC_4},
    {KC_Q,     KC_W,     KC_E,     KC_R},
    {KC_A,     KC_S,     KC_D,     KC_F},
    {KC_Z,     KC_X,     KC_C,     KC_V},
    {KC_LCTRL, KC_LSHIFT, KC_SPACE, KC_ENTER},
};

// 键码名称表
static const struct {
    uint8_t code;
    const char *name;
} key_names[] = {
    {KC_NONE,     "NONE"},
    {KC_A,        "A"}, {KC_B, "B"}, {KC_C, "C"}, {KC_D, "D"},
    {KC_E,        "E"}, {KC_F, "F"}, {KC_G, "G"}, {KC_H, "H"},
    {KC_I,        "I"}, {KC_J, "J"}, {KC_K, "K"}, {KC_L, "L"},
    {KC_M,        "M"}, {KC_N, "N"}, {KC_O, "O"}, {KC_P, "P"},
    {KC_Q,        "Q"}, {KC_R, "R"}, {KC_S, "S"}, {KC_T, "T"},
    {KC_U,        "U"}, {KC_V, "V"}, {KC_W, "W"}, {KC_X, "X"},
    {KC_Y,        "Y"}, {KC_Z, "Z"},
    {KC_1, "1"}, {KC_2, "2"}, {KC_3, "3"}, {KC_4, "4"}, {KC_5, "5"},
    {KC_6, "6"}, {KC_7, "7"}, {KC_8, "8"}, {KC_9, "9"}, {KC_0, "0"},
    {KC_ENTER,    "ENTER"}, {KC_ESC, "ESC"}, {KC_BSPACE, "BSPACE"},
    {KC_TAB,      "TAB"}, {KC_SPACE, "SPACE"}, {KC_MINUS, "MINUS"},
    {KC_EQUAL,    "EQUAL"}, {KC_LBRACKET, "["}, {KC_RBRACKET, "]"},
    {KC_BSLASH,   "\\"}, {KC_SCOLON, ";"}, {KC_QUOTE, "'"},
    {KC_GRAVE,    "`"}, {KC_COMMA, ","}, {KC_DOT, "."}, {KC_SLASH, "/"},
    {KC_CAPSLOCK, "CAPSLOCK"},
    {KC_F1, "F1"}, {KC_F2, "F2"}, {KC_F3, "F3"}, {KC_F4, "F4"},
    {KC_F5, "F5"}, {KC_F6, "F6"}, {KC_F7, "F7"}, {KC_F8, "F8"},
    {KC_F9, "F9"}, {KC_F10, "F10"}, {KC_F11, "F11"}, {KC_F12, "F12"},
    {KC_LCTRL,    "LCTRL"}, {KC_LSHIFT, "LSHIFT"}, {KC_LALT, "LALT"}, {KC_LGUI, "LGUI"},
    {KC_RCTRL,    "RCTRL"}, {KC_RSHIFT, "RSHIFT"}, {KC_RALT, "RALT"}, {KC_RGUI, "RGUI"},
    // Consumer 功能键（P6）：名称同时用于 [EVT] CC 行与上位机回显，勿随意改动
    {KC_VOL_UP,     "VOL+"}, {KC_VOL_DOWN, "VOL-"}, {KC_MUTE, "MUTE"},
    {KC_PLAY_PAUSE, "PLAY"}, {KC_MEDIA_NEXT, "NEXT"}, {KC_MEDIA_PREV, "PREV"},
    {KC_BRIGHT_UP,  "BRI+"}, {KC_BRIGHT_DOWN, "BRI-"},
    // 宏触发键（P6）
    {KC_MACRO1, "MACRO1"}, {KC_MACRO2, "MACRO2"}, {KC_MACRO3, "MACRO3"}, {KC_MACRO4, "MACRO4"},
    {0, NULL}
};

void keymap_init(void) {
    ESP_LOGI(TAG, "Initializing keymap...");
    memcpy(current_keymap, default_keymap, sizeof(current_keymap));
    ESP_LOGI(TAG, "Keymap initialized with default layout");
}

uint8_t keymap_get_key(uint8_t row, uint8_t col) {
    if (row < KEYMAP_ROWS && col < KEYMAP_COLS) {
        return current_keymap[row][col];
    }
    return KC_NONE;
}

void keymap_set_key(uint8_t row, uint8_t col, uint8_t keycode) {
    if (row < KEYMAP_ROWS && col < KEYMAP_COLS) {
        current_keymap[row][col] = keycode;
    }
}

const uint8_t *keymap_get_current(void) {
    return &current_keymap[0][0];
}

void keymap_load_profile(uint8_t profile_id) {
    ESP_LOGI(TAG, "Loading keymap for profile %d", profile_id + 1);
    // 从当前 Profile 恢复键位（profile_init 已完成 NVS 加载）；
    // Profile 数据无效时回退默认键位
    profile_t *p = profile_get_current();
    if (p != NULL && p->valid) {
        memcpy(current_keymap, p->keymap, sizeof(current_keymap));
    } else {
        memcpy(current_keymap, default_keymap, sizeof(current_keymap));
    }
}

void keymap_print(void) {
    ESP_LOGI(TAG, "Current keymap:");
    // 格式（上位机解析回显依赖，勿改）：Row <r>: 0xNN(名称) ...
    for (int r = 0; r < KEYMAP_ROWS; r++) {
        printf("Row %d:", r);
        for (int c = 0; c < KEYMAP_COLS; c++) {
            printf(" 0x%02X(%s)", current_keymap[r][c],
                   keymap_get_key_name(current_keymap[r][c]));
        }
        printf("\n");
    }
}

const char *keymap_get_key_name(uint8_t keycode) {
    if (keycode == KC_TRNS) return "TRNS";
    for (int i = 0; key_names[i].name != NULL; i++) {
        if (key_names[i].code == keycode) {
            return key_names[i].name;
        }
    }
    return "???";
}
