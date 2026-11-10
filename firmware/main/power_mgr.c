/**
 * @file power_mgr.c
 * @brief ESP32-S3 三模键盘 - 电源管理（电池采?充电检测）
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-20
 *
 * ADC：ADC1_CH0（GPIO1? 曲线拟合校准；分?100k/100k ?实际电压 = ADC 电压 × 2
 * 电量百分比：3.30~4.20V 锂电 OCV 查表线性插值（103450/2000mAh，方?§3.3.1
 * 低电锚点?.55V 告警?.35V 自动关机（关机策?P7 实现，本模块只上报）
 */

#include "power_mgr.h"
#include "configs/pin_config.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "adc_cali_schemes.h"   // 曲线拟合方案声明（esp32s3/include，无 esp_adc/ 前缀
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "led_driver.h"
#include "sleep_mgr.h"
#include <string.h>

static const char *TAG = "POWER";

static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t cali_handle = NULL;
static bool cali_ready = false;

// ==================== 电量查表（电?mV ?百分?%?====================
// 锂电 OCV 近似曲线?.55V=10% 告警锚点?.35V=0% 关机锚点（方?§3
static const struct { uint16_t mv; uint8_t pct; } OCV_TABLE[] = {
    {3350, 0}, {3450, 2}, {3550, 10}, {3650, 25}, {3750, 40},
    {3850, 55}, {3950, 70}, {4050, 85}, {4150, 97}, {4200, 100},
};
#define OCV_TABLE_LEN (sizeof(OCV_TABLE) / sizeof(OCV_TABLE[0]))

// ==================== 两点校准（P7） ====================
// NVS "pwrmgr"：cal_lm/cal_lt/cal_hm/cal_ht（u16，true=0 表示该点未校准）
#define PWR_CAL_NVS_NS      "pwrmgr"
#define PWR_CAL_KEY_LM      "cal_lm"
#define PWR_CAL_KEY_LT      "cal_lt"
#define PWR_CAL_KEY_HM      "cal_hm"
#define PWR_CAL_KEY_HT      "cal_ht"
#define PWR_CAL_MV_MIN      500
#define PWR_CAL_MV_MAX      4500

static power_cal_point_t s_cal_lo = {0, 0};   // 低锚点（true_mv=0 = 未校准）
static power_cal_point_t s_cal_hi = {0, 0};   // 高锚点

static void cal_load(void) {
    nvs_handle_t h;
    if (nvs_open(PWR_CAL_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, PWR_CAL_KEY_LM, &s_cal_lo.meas_mv);
        nvs_get_u16(h, PWR_CAL_KEY_LT, &s_cal_lo.true_mv);
        nvs_get_u16(h, PWR_CAL_KEY_HM, &s_cal_hi.meas_mv);
        nvs_get_u16(h, PWR_CAL_KEY_HT, &s_cal_hi.true_mv);
        nvs_close(h);
    }
}

// 两点线性映射（区间内插值、区间外外推）；未校准（任一锚点 true=0）时恒等返回
static uint16_t cal_correct(uint16_t mv) {
    if (s_cal_lo.true_mv == 0 || s_cal_hi.true_mv == 0) return mv;
    int32_t d_meas = (int32_t)s_cal_hi.meas_mv - (int32_t)s_cal_lo.meas_mv;
    int32_t d_true = (int32_t)s_cal_hi.true_mv - (int32_t)s_cal_lo.true_mv;
    if (d_meas == 0) return mv;
    int32_t t = (int32_t)s_cal_lo.true_mv +
                ((int32_t)mv - (int32_t)s_cal_lo.meas_mv) * d_true / d_meas;
    if (t < 0) t = 0;
    if (t > 5000) t = 5000;
    return (uint16_t)t;
}

bool power_mgr_cal_get(power_cal_point_t *lo, power_cal_point_t *hi) {
    if (lo == NULL || hi == NULL) return false;
    *lo = s_cal_lo;
    *hi = s_cal_hi;
    return true;
}

bool power_mgr_cal_set(uint16_t lo_meas, uint16_t lo_true,
                       uint16_t hi_meas, uint16_t hi_true) {
    uint16_t vals[4] = {lo_meas, lo_true, hi_meas, hi_true};
    for (int i = 0; i < 4; i++) {
        if (vals[i] > PWR_CAL_MV_MAX) return false;
        // 实测值须 ≥500（分压后有效量程）；真实值允许 0 = 该点未校准（cal_correct 哨兵语义）
        if ((i == 0 || i == 2) && vals[i] < PWR_CAL_MV_MIN) return false;
    }
    if (hi_meas <= lo_meas) return false;

    nvs_handle_t h;
    esp_err_t err = nvs_open(PWR_CAL_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cal NVS open failed: 0x%x", err);
        return false;
    }
    err = nvs_set_u16(h, PWR_CAL_KEY_LM, lo_meas);
    if (err == ESP_OK) err = nvs_set_u16(h, PWR_CAL_KEY_LT, lo_true);
    if (err == ESP_OK) err = nvs_set_u16(h, PWR_CAL_KEY_HM, hi_meas);
    if (err == ESP_OK) err = nvs_set_u16(h, PWR_CAL_KEY_HT, hi_true);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;

    s_cal_lo.meas_mv = lo_meas; s_cal_lo.true_mv = lo_true;
    s_cal_hi.meas_mv = hi_meas; s_cal_hi.true_mv = hi_true;
    ESP_LOGI(TAG, "Calibration set: lo(%u->%u) hi(%u->%u)",
             lo_meas, lo_true, hi_meas, hi_true);
    return true;
}

// ==================== 三级电压策略（P7） ====================
#define PWR_LOW_WARN_MV       3550   // 告警锚点：当前模式 LED 1Hz 慢闪
#define PWR_SHUTDOWN_MV       3350   // 关机锚点：连续确认后深睡关机
#define PWR_SHUTDOWN_CONFIRM  3      // 关机确认次数（1s 间隔，防 TX 压降误判）

static int s_shutdown_confirm = 0;
static bool s_low_warn_active = false;

void power_mgr_policy_tick(power_status_t *out) {
    power_status_t ps;
    power_mgr_get(&ps);
    if (out != NULL) *out = ps;

    // USB 在位（含充电/充满）：电压虚高无意义，跳过判定并复位计数与告警
    if (ps.usb_present) {
        s_shutdown_confirm = 0;
        if (s_low_warn_active) {
            led_set_low_battery(false);
            s_low_warn_active = false;
        }
        return;
    }

    // 一级：3.55V 告警（LED 慢闪 1Hz，随当前模式颜色）
    if (ps.mv < PWR_LOW_WARN_MV) {
        if (!s_low_warn_active) {
            ESP_LOGW(TAG, "Low battery warning: %umV", ps.mv);
            led_set_low_battery(true);
            s_low_warn_active = true;
        }
    } else if (s_low_warn_active && ps.mv > PWR_LOW_WARN_MV + 50) {
        // 50mV 迟滞防临界抖动
        led_set_low_battery(false);
        s_low_warn_active = false;
    }

    // 二级：3.35V 连续 3 次确认 → 深睡关机（含 EN_BAT 保持低等 GPIO 隔离）
    if (ps.mv < PWR_SHUTDOWN_MV) {
        s_shutdown_confirm++;
        ESP_LOGW(TAG, "Shutdown confirm %d/%d (%umV)",
                 s_shutdown_confirm, PWR_SHUTDOWN_CONFIRM, ps.mv);
        if (s_shutdown_confirm >= PWR_SHUTDOWN_CONFIRM) {
            sleep_mgr_deep_sleep_enter("low battery 3.35V shutdown");
        }
    } else {
        s_shutdown_confirm = 0;
    }
}

static uint8_t voltage_to_pct(uint16_t mv) {
    if (mv >= OCV_TABLE[OCV_TABLE_LEN - 1].mv) return 100;
    if (mv <= OCV_TABLE[0].mv) return 0;
    for (size_t i = 1; i < OCV_TABLE_LEN; i++) {
        if (mv <= OCV_TABLE[i].mv) {
            uint16_t v0 = OCV_TABLE[i - 1].mv, v1 = OCV_TABLE[i].mv;
            uint8_t p0 = OCV_TABLE[i - 1].pct, p1 = OCV_TABLE[i].pct;
            return (uint8_t)(p0 + ((uint32_t)(mv - v0) * (p1 - p0)) / (v1 - v0));
        }
    }
    return 0;
}

// ==================== 初始?====================

void power_mgr_init(void) {
    // EN_BAT：输出低 = 分压下臂断路（零漏电；采样时才拉高）
    gpio_config_t en_conf = {
        .pin_bit_mask = (1ULL << EN_BAT_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&en_conf);
    gpio_set_level(EN_BAT_PIN, 0);

    // 充电检测脚：输入（外部 100k 上拉?3.3V，低有效
    gpio_config_t det_conf = {
        .pin_bit_mask = (1ULL << CHRG_DET_PIN) | (1ULL << STDBY_DET_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&det_conf);

    // ADC1 配置 + 曲线拟合校准
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_0, &chan_cfg));

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_0,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle) == ESP_OK) {
        cali_ready = true;
    } else {
        ESP_LOGW(TAG, "ADC calibration unavailable, raw values only");
    }

    ESP_LOGD(TAG, "Power manager initialized");

    cal_load();   // 两点校准参数（P7）
}

// ==================== 采样 ====================

void power_mgr_get(power_status_t *out) {
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));

    // 充电状态：开漏输出，?有效（未焊电池时脚位?100k 上拉保持高）
    int chrg = gpio_get_level(CHRG_DET_PIN);
    int stdby = gpio_get_level(STDBY_DET_PIN);
    out->charging = (chrg == 0);
    out->full = (stdby == 0);
    out->usb_present = out->charging || out->full;

    // 分压采样：EN_BAT 拉高接通下??稳定 ?16 次平??拉低断路
    gpio_set_level(EN_BAT_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(2));   // 分压网络稳定 + 2N7002 开关建

    uint32_t raw_sum = 0;
    const int N = 16;
    int raw;
    for (int i = 0; i < N; i++) {
        adc_oneshot_read(adc_handle, ADC_CHANNEL_0, &raw);
        raw_sum += (uint32_t)raw;
    }
    gpio_set_level(EN_BAT_PIN, 0);  // 立即断路，杜绝分压漏

    uint32_t raw_avg = raw_sum / N;
    uint32_t mv = 0;
    if (cali_ready) {
        int cal_mv = 0;
        if (adc_cali_raw_to_voltage(cali_handle, (int)raw_avg, &cal_mv) == ESP_OK) {
            mv = (uint32_t)cal_mv * 2;  // 100k/100k 分压还原
        }
    }
    out->mv_raw = (uint16_t)mv;         // 校准前原始值（校准 UI 用）
    out->mv = cal_correct((uint16_t)mv);  // 两点校准线性映射（未校准=恒等）

    // 百分比：充电期间电压虚高（TP4056 CV 阶段），暂停更新、上报最近一次有效值
    // （2026-10-11 修复：原实现充电中硬编码 pct=0，上位机恒显 0%）；
    // 开机即充电（无历史值）时用当前电压估算一次兜底
    static uint8_t s_last_pct = 0;
    if (out->charging) {
        if (s_last_pct == 0) {
            s_last_pct = voltage_to_pct(out->mv);
        }
        out->pct = s_last_pct;
    } else {
        out->pct = voltage_to_pct(out->mv);
        s_last_pct = out->pct;
    }

    ESP_LOGD(TAG, "raw=%lu mv=%u pct=%u chrg=%d stdby=%d",
             (unsigned long)raw_avg, out->mv, out->pct, chrg, stdby);
}
