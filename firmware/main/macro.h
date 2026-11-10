/**
 * @file macro.h
 * @brief ESP32-S3 三模键盘 - 宏引擎（P6 智能功能）
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-22
 *
 * 宏播放引擎：按宏步骤序列依次执行按下/松开/延迟/轻点。
 * - USB/BLE 模式：经 hid_manager 直接发本地 HID（复用 6KRO 状态机与 Consumer 路由）
 * - WiFi 模式：不发本地 HID，步骤以 [EVT] MACRO 文本行经 TCP 推送，
 *   由上位机 SendInput 注入器播放（延迟仍在键盘端真实执行，时序不失真）
 * - 所有模式下 [EVT] MACRO 行均会输出（USB/BLE 下仅供上位机展示，不注入）
 */

#ifndef MACRO_H
#define MACRO_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief 初始化宏引擎（创建播放任务与触发队列；在 app_main 中调用一次）
 */
void macro_engine_init(void);

/**
 * @brief 触发指定宏的播放（由 main_task 在按键事件中调用，仅按下沿触发）
 *
 * 播放中再次触发：丢弃并 LOGW（不排队，行为可预测）
 *
 * @param idx 宏序号（0~3，对应键位 KC_MACRO1~4）
 * @return true 已接受（排队成功）；false idx 越界 / 播放中丢弃
 */
bool macro_engine_trigger(uint8_t idx);

/**
 * @brief 是否正在播放宏
 */
bool macro_engine_is_playing(void);

/**
 * @brief 进入宏录制模式（帧协议 0x0E 调用）
 *
 * 录制期间主任务拦截所有 HID 输出（普通键/修饰键/Consumer/宏触发），
 * 按键事件仅保留 [EVT] KEY 行上报，由上位机收集时间戳生成宏步骤。
 * RAM 标志，不持久化；模式切换/Profile 切换时自动退出。
 *
 * @return true 已进入录制；false 已在录制中
 */
bool macro_rec_start(void);

/**
 * @brief 退出宏录制模式（帧协议 0x0E 停止/模式或 Profile 切换自动退出）
 *
 * @return true 已退出；false 本就不在录制中
 */
bool macro_rec_stop(void);

/**
 * @brief 是否处于宏录制模式（主任务按键分流用）
 */
bool macro_recording(void);

#endif // MACRO_H
