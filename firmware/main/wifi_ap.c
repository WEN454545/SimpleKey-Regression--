/**
 * @file wifi_ap.c
 * @brief ESP32-S3 三模键盘 - WiFi SoftAP 热点管理（P5 方案 A，v1.8
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-21
 *
 * 实现要点
 * - SSID ?SoftAP 专属 MAC ?2 字节生成 4 ?hex（esp_read_mac ESP_MAC_WIFI_SOFTAP
 * - 密码?NVS（namespace "wifiap"，key "psk"），未配置时用默?simplekey
 * - start 幂等、stop 幂等：由 hid_manager 在模式切换时调用，与 BLE 栈互
 * - 启动后必须关省电：esp_wifi_set_ps(WIFI_PS_NONE)，否则按键延迟可?200ms+
 */

#include "wifi_ap.h"
#include <string.h>
#include <stdio.h>
#include "tasks_config.h"   // BLE_DEVICE_NAME：热?SSID 与蓝牙广播名一
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "WIFI_AP";

#define WIFI_NVS_NAMESPACE  "wifiap"
#define WIFI_NVS_KEY_PSK    "psk"

static bool s_started = false;
static esp_netif_t *s_ap_netif = NULL;
static char s_ssid[WIFI_AP_SSID_MAX] = {0};
static char s_psk[WIFI_AP_PSK_MAX + 1] = {0};

// ==================== 配置加载 ====================

static void load_psk_from_nvs(void) {
    // 默认值兜
    strlcpy(s_psk, WIFI_AP_PSK_DEFAULT, sizeof(s_psk));

    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char buf[WIFI_AP_PSK_MAX + 1] = {0};
    size_t len = sizeof(buf);
    if (nvs_get_str(h, WIFI_NVS_KEY_PSK, buf, &len) == ESP_OK && len >= WIFI_AP_PSK_MIN) {
        strlcpy(s_psk, buf, sizeof(s_psk));
    }
    nvs_close(h);
}

// ==================== 公开接口 ====================

bool wifi_ap_is_started(void) {
    return s_started;
}

void wifi_ap_get_ssid(char *out, size_t len) {
    strlcpy(out, s_ssid, len);
}

esp_err_t wifi_ap_set_psk(const char *psk) {
    if (psk == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(psk);
    if (len < WIFI_AP_PSK_MIN || len > WIFI_AP_PSK_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_str(h, WIFI_NVS_KEY_PSK, psk);
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    if (ret == ESP_OK) {
        strlcpy(s_psk, psk, sizeof(s_psk));
        ESP_LOGI(TAG, "PSK updated (len=%d)", (int)len);
    }
    return ret;
}

esp_err_t wifi_ap_start(void) {
    if (s_started) {
        return ESP_OK;
    }

    load_psk_from_nvs();

    // SSID：与蓝牙广播名一致（BLE_DEVICE_NAME，UTF-8 中文 SSID，长度需 ?2 字节
    strlcpy(s_ssid, BLE_DEVICE_NAME, sizeof(s_ssid));

    // 网络接口与事件循环（幂等：重复初始化返回 ESP_OK / ERR_INVALID_STATE 均忽略）
    esp_netif_init();
    esp_event_loop_create_default();

    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, s_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(s_ssid);
    strlcpy((char *)ap_config.ap.password, s_psk, sizeof(ap_config.ap.password));
    ap_config.ap.channel = 6;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.max_connection = 2;
    ap_config.ap.pmf_cfg.capable = false;
    ap_config.ap.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 低延迟关键：SoftAP 默认省电模式会让下行 TCP 延迟?200ms+
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_started = true;
    ESP_LOGI(TAG, "SoftAP up: ssid=%s ch=6 ip=192.168.4.1", s_ssid);
    return ESP_OK;
}

esp_err_t wifi_ap_stop(void) {
    if (!s_started) {
        return ESP_OK;
    }

    esp_err_t ret = esp_wifi_stop();
    esp_wifi_deinit();
    if (s_ap_netif != NULL) {
        esp_netif_destroy(s_ap_netif);  // 结束内部 esp_netif 任务，释放内
        s_ap_netif = NULL;
    }
    s_started = false;
    ESP_LOGI(TAG, "SoftAP down (stop=%s)", esp_err_to_name(ret));
    return ret;
}
