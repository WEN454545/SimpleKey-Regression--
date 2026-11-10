/**
 * @file power_mgr.h
 * @brief ESP32-S3 三模键盘 - 电源管理（电池采?充电检测）
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-20
 *
 * 硬件约束（方?§3.2/§3.3）：
 * - VBAT_SENSE=GPIO1（ADC1_CH0?00k/100k 分压，下臂经 2N7002 接地
 * - EN_BAT=GPIO17：高=接通分压采样，?断路零漏电（深睡 RTC 保持低）
 * - CHRG_DET=GPIO41 / STDBY_DET=GPIO42（TP4056 开漏输出，100k 上拉 3.3V）：
 *   ?充电?已充满，任一?USB 在位
 * - 电池未焊?ADC 读数为悬空值（无意义），充电检测脚仍可判断 USB 在位
 */

#ifndef POWER_MGR_H
#define POWER_MGR_H

#include <stdint.h>
#include <stdbool.h>

// ==================== 数据结构 ====================

typedef struct {
    uint16_t mv;            // 电池电压（mV，分压还原后）；电池未焊时为悬空采样
    uint16_t mv_raw;        // 校准前原始电压（mV，P7 两点校准 UI 用）
    uint8_t pct;            // 电量百分比 0-100（查表插值）；充电中不更新、仅报充电
    bool charging;          // 充电中（CHRG_DET 低）
    bool full;              // 已充满（STDBY_DET 低）
    bool usb_present;       // USB 在位（CHRG_DET/STDBY_DET 任一低）
} power_status_t;

// 两点校准点（P7）：固件测量值 → 万用表实测值的线性映射锚点
typedef struct {
    uint16_t meas_mv;       // 固件测量值（mV）
    uint16_t true_mv;       // 万用表实测值（mV）；0 = 该点未校准
} power_cal_point_t;

// ==================== 函数声明 ====================

/**
 * @brief 初始化电源管理（EN_BAT 输出低分压断路、充电检测脚输入、ADC1 配置） */
void power_mgr_init(void);

/**
 * @brief 采样一次电池电压并返回电源状态
 *
 * 采样流程：EN_BAT 拉高 → 稳定延时 → ADC 采样 16 次平均 → EN_BAT 拉低
 * （采样窗口约几毫秒，不可在深睡前遗留 EN_BAT 为高）
 *
 * @param out 输出状态结构体（不可为 NULL） */
void power_mgr_get(power_status_t *out);

/**
 * @brief 电源策略周期检查（P7，由 sleep_mgr 任务 1s 周期调用，内部完成一次采样）
 *
 * - 3.55V 告警（电池供电时当前模式 LED 1Hz 慢闪，恢复后自动解除）
 * - 3.35V 连续 3 次确认（1s 间隔，防 TX 压降误判）→ sleep_mgr 深睡关机
 * - USB 在位时跳过全部判定并复位告警/关机计数
 *
 * @param out 可选输出（非 NULL 时回传本次采样状态，供调用方复用免二次采样）
 */
void power_mgr_policy_tick(power_status_t *out);

/**
 * @brief 读取两点校准（P7；未校准点 true_mv=0）
 */
bool power_mgr_cal_get(power_cal_point_t *lo, power_cal_point_t *hi);

/**
 * @brief 设置两点校准并写 NVS（P7）
 *
 * 语义：固件测得 meas_mv 时，真实电压应为 true_mv（万用表实测）。
 * 两点确定线性映射 y = f(x)，低于/高于锚点区间时线性外推。
 *
 * @return false 参数非法（有效范围 500~4500mV 且 hi_meas > lo_meas）
 */
bool power_mgr_cal_set(uint16_t lo_meas, uint16_t lo_true,
                       uint16_t hi_meas, uint16_t hi_true);

#endif // POWER_MGR_H
