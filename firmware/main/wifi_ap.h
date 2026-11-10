/**
 * @file wifi_ap.h
 * @brief ESP32-S3 三模键盘 - WiFi SoftAP 热点管理（P5 方案 A，v1.8
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-21
 *
 * 键盘作为 SoftAP 提供热点
 * - SSID：SimpleKey-XXXX（XXXX = SoftAP MAC ?2 字节?4 位十六进制）
 * - 加密：WPA2-PSK，密码存 NVS（默?simplekey?~63 字符，串?wifi psk 命令修改
 * - DHCP：键?= 192.168.4.1（ESP-IDF SoftAP 默认地址），上位机连接后 TCP 192.168.4.1:8266
 * - 低延迟：启动?esp_wifi_set_ps(WIFI_PS_NONE)（默认省电模式延迟可?200ms+
 * - ?BLE 互斥：由 hid_manager 在模式切换时调用 start/stop
 */

#ifndef WIFI_AP_H
#define WIFI_AP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// ==================== 常量 ====================

#define WIFI_AP_PSK_DEFAULT     "simplekey"     // 默认密码（NVS 未配置时
#define WIFI_AP_PSK_MIN         8               // WPA2 密码最短长
#define WIFI_AP_PSK_MAX         63              // WPA2 密码最长长
#define WIFI_AP_SSID_MAX        32

// ==================== 函数声明 ====================

/**
 * @brief 启动 SoftAP（幂等：已启动直接返?ESP_OK? *
 * 内部完成 esp_netif/esp_wifi 初始化、SSID 生成（SimpleKey-XXXX）
 * NVS 读取密码、esp_wifi_start 与关省电模式
 */
esp_err_t wifi_ap_start(void);

/**
 * @brief 停止 SoftAP 并释放资源（幂等：未启动直接返回 ESP_OK? *
 * 切回 BLE 模式前必须调用（射频互斥? */
esp_err_t wifi_ap_stop(void);

/**
 * @brief 是否已启? */
bool wifi_ap_is_started(void);

/**
 * @brief 获取当前 SSID（SimpleKey-XXXX? *
 * @param out 输出缓冲
 * @param len 缓冲长度
 */
void wifi_ap_get_ssid(char *out, size_t len);

/**
 * @brief 设置并持久化 WPA2 密码（NVS? *
 * @param psk 新密码（8~63 字符
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 长度不合? */
esp_err_t wifi_ap_set_psk(const char *psk);

#endif // WIFI_AP_H
