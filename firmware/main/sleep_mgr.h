/**
 * @file sleep_mgr.h
 * @brief ESP32-S3 三模键盘 - 两级睡眠管理器（P7，方案 §12.13）
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-28
 *
 * 睡眠策略（用户冻结决策，2026-09-28）：
 * - USB 在位（CHRG_DET/STDBY_DET 任一低）永不睡眠
 * - 电池供电闲置 5min（NVS 可配）→ light sleep：任意键 1ms 唤醒，唤醒即恢复
 * - 闲置 24h（NVS 可配）→ deep sleep：RTC 任意键唤醒（约 300ms，等效重启）
 * - BLE 连接态允许 light sleep（2026-10-11 用户决策）：sdkconfig 已启用蓝牙 modem sleep
 *   （Mode1 + 主晶振 LPCLK），睡眠期间控制器按连接事件自动唤醒，按键延迟 ~ms；
 *   旧口径「连接不睡」是 modem sleep 未启用、入睡必被拒时代的保守兜底，已废止
 * - WiFi 热点模式不进 light sleep（SoftAP 需持续在线），仅允许 24h 深睡
 * - 深睡前 GPIO 隔离：EN_BAT 保持低（分压断路防漏电）+ rtc_gpio_isolate()
 * - 深睡唤醒 = 复位重启：RTC pad hold 不会随复位清除，上电最早阶段须调
 *   sleep_mgr_release_wake_holds() 解除，否则矩阵/电池采样被锁死在深睡配置
 */

#ifndef SLEEP_MGR_H
#define SLEEP_MGR_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief 初始化睡眠管理器（读 NVS 参数 + 创建监控任务）
 *
 * 需在 NVS/矩阵/宏/电源各模块初始化完成后调用
 */
void sleep_mgr_init(void);

/**
 * @brief 刷新活动时间戳（矩阵按键 / 编码器 / 配置帧活动时调用）
 */
void sleep_mgr_activity(void);

/**
 * @brief 立即进入深睡关机（低电 3.35V 关机复用；不返回，唤醒即复位重启）
 *
 * @param reason 进入原因（仅日志记录）
 */
void sleep_mgr_deep_sleep_enter(const char *reason);

/**
 * @brief 解除深睡遗留的 RTC pad hold（app_main 最早阶段调用）
 *
 * 深睡唤醒是芯片复位，但 rtc_gpio_hold_en 的保持不会随复位清除（仅断电或
 * rtc_gpio_hold_dis 可清）；不释放则列/行/EN_BAT 锁死在深睡配置——
 * 矩阵读不到键、电池采样分压断路（mv≈0 误触发低电关机）
 */
void sleep_mgr_release_wake_holds(void);

/** @brief light sleep 闲置阈值（分钟，NVS 持久化，默认 5）*/
uint16_t sleep_mgr_get_light_min(void);

/** @brief deep sleep 闲置阈值（小时，NVS 持久化，默认 24）*/
uint16_t sleep_mgr_get_deep_hr(void);

/**
 * @brief 设置睡眠参数并写 NVS
 * @return true 成功；false 参数非法（light 1~120min，deep 1~168h）
 */
bool sleep_mgr_set_params(uint16_t light_min, uint16_t deep_hr);

#endif // SLEEP_MGR_H
