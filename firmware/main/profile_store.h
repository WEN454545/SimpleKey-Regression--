/**
 * @file profile_store.h
 * @brief ESP32-S3 三模键盘 - Profile 存储
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 *
 * Profile 管理，NVS 持久化存储
 * 支持 3 个 Profile，每个 Profile 包含键位映射和其他设置
 */

#ifndef PROFILE_STORE_H
#define PROFILE_STORE_H

#include <stdint.h>
#include <stdbool.h>

// ==================== 数据结构 ====================

#define PROFILE_NAME_LEN    16
#define PROFILE_COUNT       3

// ==================== 宏定义（P6 智能功能） ====================

#define MACRO_MAX           4       // 每个 Profile 最多 4 条宏（对应键位 0xF0~0xF3）
#define MACRO_STEP_MAX      32      // 每条宏最多 32 步
#define MACRO_NAME_LEN      8       // 宏名称长度（含 \0）

// 宏步骤动作类型
#define MACRO_ACT_DOWN      0       // 按下
#define MACRO_ACT_UP        1       // 松开
#define MACRO_ACT_DELAY     2       // 延迟 delay_ms（1~60000）
#define MACRO_ACT_TAP       3       // 轻点（执行时展开为按下+30ms+松开）

// 旋钮短按伪功能码（v1.9 交互扩展：短按可配，默认行为不变）
#define KNOB_FUNC_PROFILE   0xFE    // 短按=循环切换 Profile（默认行为，向后兼容）

// 宏步骤（4 字节紧凑格式；keycode 支持 0x04~0xE7 普通键/修饰键与 0xC0~0xCF Consumer）
typedef struct __attribute__((packed)) {
    uint8_t action;         // MACRO_ACT_*
    uint8_t keycode;        // 键码 / Consumer 键码
    uint16_t delay_ms;      // 仅 action==MACRO_ACT_DELAY 时有效（小端）
} macro_step_t;

// 宏定义（137 字节 packed）
typedef struct __attribute__((packed)) {
    char name[MACRO_NAME_LEN];              // 宏名称
    uint8_t step_count;                     // 有效步数（0=空宏）
    macro_step_t steps[MACRO_STEP_MAX];     // 步骤序列
} macro_t;

// Profile 结构体（v3，2026-09-24：新增旋钮短按配置；NVS blob 总量 590B×3 ≈ 1.7KB）
// v2（589B，P6）→ v3 迁移：新 NVS key "data3"，键位/名称/模式/宏整体拷贝，knob_press 填默认
typedef struct __attribute__((packed)) {
    char name[PROFILE_NAME_LEN];    // Profile 名称
    uint8_t keymap[5][4];           // 键位映射
    uint8_t default_mode;           // 默认连接模式（存 hid_mode_t 值：1=USB, 2=BLE, 3=WiFi）
    uint8_t knob_cw;                // 旋钮顺时针输出（Consumer 键码 0xC0~0xCF，默认 KC_VOL_UP）
    uint8_t knob_ccw;               // 旋钮逆时针输出（默认 KC_VOL_DOWN）
    uint8_t knob_press;             // 旋钮短按输出（0xFE=切 Profile 默认 / 0xC0~0xCF / 0xF0~0xF3 宏）
    uint8_t version;                // 结构版本（0x03）
    bool valid;                     // 是否有效
    macro_t macros[MACRO_MAX];      // 宏存储（随 Profile 切换整体加载）
} profile_t;

// ==================== 函数声明 ====================

/**
 * @brief 初始化 Profile 存储
 *
 * 从 NVS 加载 Profile 数据
 */
void profile_init(void);

/**
 * @brief 获取当前 Profile
 *
 * @return profile_t* 当前 Profile 指针
 */
profile_t *profile_get_current(void);

/**
 * @brief 获取指定 Profile 槽位（只读）
 *
 * @param index Profile 索引（0-2）
 * @return const profile_t* 槽位指针；index 越界返回 NULL
 */
const profile_t *profile_get_slot(uint8_t index);

/**
 * @brief 获取当前 Profile 索引
 *
 * @return uint8_t Profile 索引（0-2）
 */
uint8_t profile_get_current_index(void);

/**
 * @brief 切换到下一个 Profile
 *
 * LED 快闪 N 次提示
 */
void profile_switch_next(void);

/**
 * @brief 设置当前 Profile
 *
 * @param index Profile 索引（0-2）
 */
void profile_set_current(uint8_t index);

/**
 * @brief 写入指定 Profile 槽位（名称+键位，RAM+立即 NVS 持久化）
 *
 * 用于上位机 Profile 导入/重命名（帧协议 PROFILE_WRITE）。
 * 若写入的是当前 Profile，自动重载键位到运行态。
 *
 * @param index  Profile 索引（0-2）
 * @param name   名称（最长 PROFILE_NAME_LEN-1 字符，超长截断）
 * @param keymap 20 字节键位表（5x4 行优先）
 * @return true 成功；false index 越界
 */
bool profile_write_slot(uint8_t index, const char *name, const uint8_t *keymap);

/**
 * @brief 获取默认连接模式
 *
 * @return uint8_t 默认模式（直接存 hid_mode_t 值：1=USB, 2=BLE；2.4G 预留）
 */
uint8_t profile_get_default_mode(void);

/**
 * @brief 设置并持久化默认连接模式
 *
 * 写入当前 Profile 的 default_mode 并立即保存 NVS，
 * 重启/重插 USB 后按此模式初始化
 *
 * @param mode 默认模式（hid_mode_t 值：1=USB, 2=BLE）
 */
void profile_set_default_mode(uint8_t mode);

/**
 * @brief 保存所有 Profile 到 NVS
 */
void profile_save(void);

/**
 * @brief 获取当前 Profile 的宏（P6）
 *
 * @param idx 宏序号（0~3，对应键位 0xF0~0xF3）
 * @return macro_t* 宏指针（可直接修改 RAM 内容，修改后调 profile_save 落盘）；idx 越界返回 NULL
 */
macro_t *profile_get_macro(uint8_t idx);

/**
 * @brief 写入当前 Profile 的宏（仅 RAM，靠 profile_save 落盘）（P6）
 *
 * @param idx 宏序号（0~3）
 * @param m   宏数据（step_count 自动钳制到 MACRO_STEP_MAX）
 * @return true 成功；false idx 越界或 m 为 NULL
 */
bool profile_set_macro(uint8_t idx, const macro_t *m);

/**
 * @brief 获取当前 Profile 的旋钮输出配置（P6；v1.9 扩展短按）
 *
 * @param cw    输出：顺时针 Consumer 键码（0xC0~0xCF）
 * @param ccw   输出：逆时针 Consumer 键码
 * @param press 输出：短按功能码（KNOB_FUNC_PROFILE / 0xC0~0xCF / 0xF0~0xF3）
 */
void profile_get_knob(uint8_t *cw, uint8_t *ccw, uint8_t *press);

/**
 * @brief 设置当前 Profile 的旋钮输出（仅 RAM，靠 profile_save 落盘）（P6）
 *
 * @param cw    顺时针 Consumer 键码（0xC0~0xCF）
 * @param ccw   逆时针 Consumer 键码（0xC0~0xCF）
 * @param press 短按功能码（KNOB_FUNC_PROFILE=0xFE / Consumer 0xC0~0xCF / 宏 0xF0~0xF3）
 * @return true 成功；false 参数越界
 */
bool profile_set_knob(uint8_t cw, uint8_t ccw, uint8_t press);

/**
 * @brief 恢复出厂设置
 *
 * 清除所有 Profile 数据，恢复默认值
 */
void profile_factory_reset(void);

/**
 * @brief 打印 Profile 信息到串口
 */
void profile_print(void);

#endif // PROFILE_STORE_H
