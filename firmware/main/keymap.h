/**
 * @file keymap.h
 * @brief ESP32-S3 三模键盘 - 键位映射
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 *
 * 单层键位映射，参考 simplekey10.json
 * keycode 命名使用 QMK 体系（KC_*）
 */

#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdint.h>
#include <stdbool.h>

// ==================== HID 键码定义 ====================
// 参考 USB HID Usage Tables

#define KC_NONE         0x00
#define KC_A            0x04
#define KC_B            0x05
#define KC_C            0x06
#define KC_D            0x07
#define KC_E            0x08
#define KC_F            0x09
#define KC_G            0x0A
#define KC_H            0x0B
#define KC_I            0x0C
#define KC_J            0x0D
#define KC_K            0x0E
#define KC_L            0x0F
#define KC_M            0x10
#define KC_N            0x11
#define KC_O            0x12
#define KC_P            0x13
#define KC_Q            0x14
#define KC_R            0x15
#define KC_S            0x16
#define KC_T            0x17
#define KC_U            0x18
#define KC_V            0x19
#define KC_W            0x1A
#define KC_X            0x1B
#define KC_Y            0x1C
#define KC_Z            0x1D
#define KC_1            0x1E
#define KC_2            0x1F
#define KC_3            0x20
#define KC_4            0x21
#define KC_5            0x22
#define KC_6            0x23
#define KC_7            0x24
#define KC_8            0x25
#define KC_9            0x26
#define KC_0            0x27
#define KC_ENTER        0x28
#define KC_ESC          0x29
#define KC_BSPACE       0x2A
#define KC_TAB          0x2B
#define KC_SPACE        0x2C
#define KC_MINUS        0x2D
#define KC_EQUAL        0x2E
#define KC_LBRACKET     0x2F
#define KC_RBRACKET     0x30
#define KC_BSLASH       0x31
#define KC_SCOLON       0x33
#define KC_QUOTE        0x34
#define KC_GRAVE        0x35
#define KC_COMMA        0x36
#define KC_DOT          0x37
#define KC_SLASH        0x38
#define KC_CAPSLOCK     0x39
#define KC_F1           0x3A
#define KC_F2           0x3B
#define KC_F3           0x3C
#define KC_F4           0x3D
#define KC_F5           0x3E
#define KC_F6           0x3F
#define KC_F7           0x40
#define KC_F8           0x41
#define KC_F9           0x42
#define KC_F10          0x43
#define KC_F11          0x44
#define KC_F12          0x45
#define KC_PSCREEN      0x46
#define KC_SCROLLLOCK   0x47
#define KC_PAUSE        0x48
#define KC_INSERT       0x49
#define KC_HOME         0x4A
#define KC_PAGEUP       0x4B
#define KC_DELETE       0x4C
#define KC_END          0x4D
#define KC_PAGEDOWN     0x4E
#define KC_RIGHT        0x4F
#define KC_LEFT         0x50
#define KC_DOWN         0x51
#define KC_UP           0x52
#define KC_NUMLOCK      0x53

// 修饰键
#define KC_LCTRL        0xE0
#define KC_LSHIFT       0xE1
#define KC_LALT         0xE2
#define KC_LGUI         0xE3
#define KC_RCTRL        0xE4
#define KC_RSHIFT       0xE5
#define KC_RALT         0xE6
#define KC_RGUI         0xE7

// ==================== 扩展值域（P6 智能功能） ====================
// 0xC0~0xCF：Consumer 功能键（固件映射到 Consumer Control 报告位域 bit0~7）。
// 该段在 USB HID 键盘页为保留区，与普通键/修饰键零冲突；
// 键位表仍为 uint8_t，存储区与 0x03/0x04 帧格式无需任何迁移。
// 位域 bit 序 = 键码 - KC_CONSUMER_BASE（与报告描述符 Usage 顺序一一对应）：
//   C0=音量+ C1=音量- C2=静音 C3=播放/暂停 C4=下一曲 C5=上一曲 C6=亮度+ C7=亮度-，C8~CF 预留
#define KC_CONSUMER_BASE    0xC0
#define KC_CONSUMER_END     0xCF
#define KC_VOL_UP           0xC0
#define KC_VOL_DOWN         0xC1
#define KC_MUTE             0xC2
#define KC_PLAY_PAUSE       0xC3
#define KC_MEDIA_NEXT       0xC4
#define KC_MEDIA_PREV       0xC5
#define KC_BRIGHT_UP        0xC6
#define KC_BRIGHT_DOWN      0xC7

// 0xF0~0xF3：宏触发键 MACRO1~4（按下触发当前 Profile 中对应宏的播放）
#define KC_MACRO_BASE       0xF0
#define KC_MACRO1           0xF0
#define KC_MACRO2           0xF1
#define KC_MACRO3           0xF2
#define KC_MACRO4           0xF3

// 特殊键
#define KC_NO           0x00    // 无操作
#define KC_TRNS         0xFF    // 透传（到下一层）

// ==================== 矩阵布局 ====================
#define KEYMAP_ROWS     5
#define KEYMAP_COLS     4
#define KEYMAP_SIZE     (KEYMAP_ROWS * KEYMAP_COLS)

// ==================== 函数声明 ====================

/**
 * @brief 初始化键位映射
 *
 * 从 NVS 加载默认 Profile 的键位映射
 */
void keymap_init(void);

/**
 * @brief 获取指定位置的键码
 *
 * @param row 行号
 * @param col 列号
 * @return uint8_t HID 键码
 */
uint8_t keymap_get_key(uint8_t row, uint8_t col);

/**
 * @brief 设置指定位置的键码
 *
 * @param row 行号
 * @param col 列号
 * @param keycode HID 键码
 */
void keymap_set_key(uint8_t row, uint8_t col, uint8_t keycode);

/**
 * @brief 获取当前键位映射表
 *
 * @return const uint8_t* 键位映射表指针
 */
const uint8_t *keymap_get_current(void);

/**
 * @brief 加载 Profile 的键位映射
 *
 * @param profile_id Profile ID（0-2）
 */
void keymap_load_profile(uint8_t profile_id);

/**
 * @brief 打印当前键位映射到串口
 */
void keymap_print(void);

/**
 * @brief 获取键码名称
 *
 * @param keycode HID 键码
 * @return const char* 键码名称
 */
const char *keymap_get_key_name(uint8_t keycode);

#endif // KEYMAP_H
