/**
 * @file serial_cmd.c
 * @brief ESP32-S3 三模键盘 - 串口命令
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 *
 * 串口命令协议实现 */

#include "serial_cmd.h"
#include "cfg_frame.h"
#include "hid_manager.h"
#include "ble_hid.h"
#include "wifi_ap.h"
#include "net_input.h"
#include "profile_store.h"
#include "power_mgr.h"
#include "keymap.h"
#include "led_driver.h"
#include "encoder_driver.h"
#include "matrix_driver.h"
#include "tasks_config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

// 模式三模循环：USB -> BLE -> WiFi（P5 方案 A
static hid_mode_t next_mode(hid_mode_t current) {
    if (current == HID_MODE_USB) return HID_MODE_BLE;
    if (current == HID_MODE_BLE) return HID_MODE_WIFI;
    return HID_MODE_USB;
}

static const char *TAG = "SERIAL";

static char cmd_buffer[128] = {0};
static int cmd_index = 0;

static void print_help(void) {
    printf("===========================================\n");
    printf("  SimpleKey Tri-mode Keyboard Commands\n");
    printf("===========================================\n");
    printf("System:\n");
    printf("  help       - Show this help\n");
    printf("  version    - Show firmware version\n");
    printf("  status     - Show system status\n");
    printf("  reboot     - Reboot system\n");
    printf("  factory    - Factory reset\n");
    printf("Mode:\n");
    printf("  mode       - Show current mode\n");
    printf("  mode <usb|ble|wifi|next> - Set mode\n");
    printf("WiFi:\n");
    printf("  wifi       - Show WiFi AP status\n");
    printf("  wifi psk <8-63 chars> - Set WiFi password (NVS)\n");
    printf("BLE:\n");
    printf("  ble clear  - Clear all BLE bonds (fix pairing issues)\n");
    printf("Profile:\n");
    printf("  profile    - Show current profile\n");
    printf("  profile <next|set N> - Switch profile\n");
    printf("Keymap:\n");
    printf("  keymap     - Show keymap\n");
    printf("LED:\n");
    printf("  led <usb|ble|2g4> <on|off> - Control LED\n");
    printf("Encoder:\n");
    printf("  enc        - Show encoder count\n");
    printf("===========================================\n");
}

static void print_version(void) {
    printf("[INFO] Firmware: SimpleKey Tri-mode v1.0.0\n");
    printf("[INFO] Build: %s %s\n", __DATE__, __TIME__);
    printf("[INFO] ESP-IDF: v5.4\n");
    printf("[INFO] Chip: ESP32-S3\n");
}

static void print_status(void) {
    const char *mode_name = hid_manager_get_mode_name(hid_manager_get_mode());
    bool connected = hid_manager_is_connected();
    profile_t *prof = profile_get_current();

    printf("[INFO] Mode: %s (%s)\n", mode_name, connected ? "Connected" : "Not connected");
    printf("[INFO] Profile: %d (%s)\n", profile_get_current_index() + 1, prof->name);
    printf("[INFO] Encoder: %ld\n", encoder_get_count());
}

static void process_cmd(char *cmd) {
    cmd[strcspn(cmd, "\r\n")] = 0;
    if (strlen(cmd) == 0) {
        serial_cmd_print_prompt();
        return;
    }

    ESP_LOGI(TAG, "Command: %s", cmd);

    if (strcmp(cmd, "help") == 0) {
        print_help();
    } else if (strcmp(cmd, "version") == 0) {
        print_version();
    } else if (strcmp(cmd, "status") == 0) {
        print_status();
    } else if (strcmp(cmd, "reboot") == 0) {
        printf("[INFO] Rebooting...\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else if (strcmp(cmd, "factory") == 0) {
        printf("[INFO] Factory reset...\n");
        profile_factory_reset();
        printf("[OK] Factory reset complete\n");
    } else if (strcmp(cmd, "mode") == 0) {
        printf("[INFO] Current mode: %s\n", hid_manager_get_mode_name(hid_manager_get_mode()));
    } else if (strcmp(cmd, "mode next") == 0) {
        hid_mode_t next = next_mode(hid_manager_get_mode());
        printf("[INFO] Switching to %s...\n", hid_manager_get_mode_name(next));
        esp_err_t ret = hid_manager_switch_mode(next);
        if (ret == ESP_OK) {
            profile_set_default_mode((uint8_t)next);  // 持久化：重启后回到该模式
            printf("[OK] Mode changed to %s\n", hid_manager_get_mode_name(next));
        } else {
            printf("[ERR] Mode switch failed\n");
        }
    } else if (strcmp(cmd, "mode usb") == 0) {
        printf("[INFO] Switching to USB...\n");
        esp_err_t ret = hid_manager_switch_mode(HID_MODE_USB);
        if (ret == ESP_OK) {
            profile_set_default_mode((uint8_t)HID_MODE_USB);
            printf("[OK] Mode changed to USB\n");
        } else {
            printf("[ERR] Mode switch failed\n");
        }
    } else if (strcmp(cmd, "mode ble") == 0) {
        printf("[INFO] Switching to BLE...\n");
        esp_err_t ret = hid_manager_switch_mode(HID_MODE_BLE);
        if (ret == ESP_OK) {
            profile_set_default_mode((uint8_t)HID_MODE_BLE);
            printf("[OK] Mode changed to BLE\n");
        } else {
            printf("[ERR] Mode switch failed\n");
        }
    } else if (strcmp(cmd, "mode wifi") == 0) {
        printf("[INFO] Switching to WiFi...\n");
        esp_err_t ret = hid_manager_switch_mode(HID_MODE_WIFI);
        if (ret == ESP_OK) {
            profile_set_default_mode((uint8_t)HID_MODE_WIFI);
            printf("[OK] Mode changed to WIFI\n");
        } else {
            printf("[ERR] Mode switch failed\n");
        }
    } else if (strcmp(cmd, "wifi") == 0) {
        char ssid[WIFI_AP_SSID_MAX] = {0};
        wifi_ap_get_ssid(ssid, sizeof(ssid));
        printf("[INFO] WiFi AP: %s (started=%d)\n", ssid, wifi_ap_is_started());
        printf("[INFO] TCP :%d client=%s (ip=192.168.4.1)\n", NET_INPUT_PORT,
               net_input_has_client() ? "connected" : "none");
    } else if (strncmp(cmd, "wifi psk ", 9) == 0) {
        const char *psk = cmd + 9;
        esp_err_t ret = wifi_ap_set_psk(psk);
        if (ret == ESP_OK) {
            printf("[OK] WiFi PSK updated (takes effect on next AP start)\n");
        } else {
            printf("[ERR] Invalid PSK length (%d-%d)\n", WIFI_AP_PSK_MIN, WIFI_AP_PSK_MAX);
        }
    } else if (strcmp(cmd, "ble clear") == 0) {
        // 清空所有 BLE 配对绑定（调试逃生口：主机/设备单侧删除配对后无法重连时用）
        printf("[INFO] Clearing all BLE bonds...\n");
        ble_hid_clear_bonds();
        printf("[OK] BLE bonds cleared\n");
    } else if (strcmp(cmd, "profile") == 0) {
        printf("[INFO] Current Profile: %d (%s)\n", profile_get_current_index() + 1,
               profile_get_current()->name);
    } else if (strcmp(cmd, "profile next") == 0) {
        profile_switch_next();
        printf("[OK] Profile changed to %d (%s)\n", profile_get_current_index() + 1,
               profile_get_current()->name);
    } else if (strncmp(cmd, "profile set ", 12) == 0) {
        int idx = atoi(cmd + 12) - 1;
        if (idx >= 0 && idx < PROFILE_COUNT) {
            profile_set_current(idx);
            printf("[OK] Profile set to %d (%s)\n", idx + 1, profile_get_current()->name);
        } else {
            printf("[ERR] Invalid profile number (1-%d)\n", PROFILE_COUNT);
        }
    } else if (strcmp(cmd, "keymap") == 0) {
        keymap_print();
    } else if (strncmp(cmd, "keymap set ", 11) == 0) {
        // keymap set <row> <col> <keycode>：修改 RAM 键位（立即生效，save 前掉电丢失）
        // keycode 支持 0x 十六进制或十进制，0x00 表示清除该键
        int row, col;
        char kc_str[8] = {0};
        if (sscanf(cmd + 11, "%d %d %7s", &row, &col, kc_str) == 3 &&
            row >= 0 && row < KEYMAP_ROWS && col >= 0 && col < KEYMAP_COLS) {
            uint8_t keycode = (uint8_t)strtoul(kc_str, NULL, 0);
            keymap_set_key((uint8_t)row, (uint8_t)col, keycode);
            printf("[OK] keymap [%d][%d] = 0x%02X (%s)\n", row, col, keycode,
                   keymap_get_key_name(keycode));
        } else {
            printf("[ERR] Usage: keymap set <row 0-%d> <col 0-%d> <0xNN>\n",
                   KEYMAP_ROWS - 1, KEYMAP_COLS - 1);
        }
    } else if (strcmp(cmd, "keymap save") == 0) {
        // 保存当前键位到当前 Profile（NVS 持久化，重启/切换 Profile 后恢复）
        profile_t *p = profile_get_current();
        memcpy(p->keymap, keymap_get_current(), sizeof(p->keymap));
        profile_save();
        printf("[OK] Keymap saved to Profile %d (%s)\n", profile_get_current_index() + 1, p->name);
    } else if (strcmp(cmd, "enc") == 0) {
        printf("[INFO] Encoder count: %ld\n", encoder_get_count());
    } else if (strncmp(cmd, "led ", 4) == 0) {
        printf("[OK] LED command received\n");
    } else if (strcmp(cmd, "power") == 0) {
        power_status_t pw;
        power_mgr_get(&pw);
        printf("[INFO] Battery: %u mV (%u%%)\n", pw.mv, pw.pct);
        printf("[INFO] Charge: %s %s%s\n",
               pw.usb_present ? "USB present" : "USB absent",
               pw.charging ? "[charging] " : "",
               pw.full ? "[full]" : "");
    } else {
        printf("[ERR] Unknown command: '%s'. Type 'help' for commands.\n", cmd);
    }

    serial_cmd_print_prompt();
}

void serial_cmd_init(void) {
    ESP_LOGD(TAG, "Initializing serial command...");
    cmd_index = 0;
    memset(cmd_buffer, 0, sizeof(cmd_buffer));
}

void serial_cmd_discard_line(void) {
    cmd_index = 0;
    memset(cmd_buffer, 0, sizeof(cmd_buffer));
}

void serial_cmd_print_prompt(void) {
    printf(">");
    fflush(stdout);
}

void serial_task(void *arg) {
    ESP_LOGD(TAG, "Serial task started");

    serial_cmd_print_prompt();

    int c;
    while (1) {
        c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 二进制配置帧优先：被帧状态机消费的字节不进入文本行缓?        //（AA 55 帧与文本命令共存于同一 CDC 流，?cfg_frame.c / 协议文档 §7
        if (cfg_frame_feed_byte((uint8_t)c)) {
            continue;
        }

        if (c == '\n' || c == '\r') {
            if (cmd_index > 0) {
                cmd_buffer[cmd_index] = '\0';
                process_cmd(cmd_buffer);
                cmd_index = 0;
                memset(cmd_buffer, 0, sizeof(cmd_buffer));
            }
        } else if (c == '\b' || c == 127) {
            if (cmd_index > 0) {
                cmd_index--;
                cmd_buffer[cmd_index] = '\0';
                printf("\b \b");
                fflush(stdout);
            }
        } else if (cmd_index < sizeof(cmd_buffer) - 1) {
            cmd_buffer[cmd_index++] = c;
            cmd_buffer[cmd_index] = '\0';
        }
    }
}
