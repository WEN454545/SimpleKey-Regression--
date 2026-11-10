/**
 * @file usb_hid.c
 * @brief ESP32-S3 三模键盘 - USB HID 设备
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 * 
 * 使用 TinyUSB 实现 USB HID 设备
 * - 键盘 HID Report：6 键无冲 + 修饰键
 * - Consumer Control：Volume Up/Down
 * 
 * 注意事项：
 * 1. USB 和 BLE 可能存在资源冲突，使用 hid_manager 统一管理
 * 2. 同一时间只能初始化一个 HID 接口
 */

#include "usb_hid.h"
#include "pin_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_types.h"
#include "tusb.h"
#include "class/hid/hid.h"
#include "class/hid/hid_device.h"
// v2.3 CDC 配置通路：USB HID 模式占用 GPIO19/20（USB-Serial-JTAG 不可用），
// 配置协议（文本命令 + AA 55 帧）改走 TinyUSB CDC 虚拟串口。
// 控制台三流（stdin/stdout/stderr）整体重定向到 CDC：serial_task 的 fgetc(stdin)、
// printf 应答/[EVT] 行、cfg_frame 的 fwrite(stdout) 应答一行不改即生效。
#include "tusb_cdc_acm.h"
#include "tusb_console.h"
#include "vfs_tinyusb.h"

static const char *TAG = "USB_HID";

// ==================== USB 描述符 ====================

// 【2026-09-28 定案】键盘 Report Descriptor 用 TinyUSB 宏写法（含 Report ID 1）。
// 实测二分结论（见 AGENTS.md 问题表）：手写「LED 页 5+3 位 Output」段会导致
// Windows 枚举完成后对首个 IN 报告触发总线复位（8/9 字节、有无 Report ID、
// mps 16/64、DMA/SLAVE、boot on/off 均已单变量排除），宏的 8 位 Output 写法正常。
// Consumer 沿用手写位域描述符（f7 版起在 EP82 实测稳定），独立接口/端点、无 Report ID。
// 【v2.3】复合设备扩为 4 接口：IF0 键盘 HID / IF1 Consumer HID / IF2+IF3 CDC 配置口。
// 端点分配：0x81 键盘 IN、0x82 Consumer IN、0x83 CDC 通知 IN、0x84 CDC 数据 IN、0x04 CDC 数据 OUT
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)

// 键盘 HID Report Descriptor（接口 0，Report ID=1，报告 9 字节 = ID + 8 字节）
static const uint8_t hid_report_desc_keyboard[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD)),
};

// Consumer Control HID Report Descriptor（接口 1，无 Report ID，报告 1 字节位域）
static const uint8_t hid_report_desc_consumer[] = {
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)

    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8) —— P6 扩展：3 位 → 8 位
    0x09, 0xE9,        //   Usage (Volume Increment)      bit0
    0x09, 0xEA,        //   Usage (Volume Decrement)      bit1
    0x09, 0xE2,        //   Usage (Mute)                  bit2
    0x09, 0xCD,        //   Usage (Play / Pause)          bit3
    0x09, 0xB5,        //   Usage (Scan Next Track)       bit4
    0x09, 0xB6,        //   Usage (Scan Previous Track)   bit5
    0x09, 0x6F,        //   Usage (Display Brightness Increment) bit6
    0x09, 0x70,        //   Usage (Display Brightness Decrement) bit7
    0x81, 0x02,        //   Input (Data, Variable, Absolute) 位域

    0xC0,              // End Collection
};

// 设备描述符
static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,      // Espressif VID
    .idProduct = 0x4006,     // v2.3：复合设备扩 CDC 接口（0x4005 为纯 HID 双接口布局），
                             // Windows 按 VID/PID 缓存描述符，换新 PID 强制重新枚举
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

// 字符串描述符
static const char *string_desc[] = {
    [0] = (const char[]){0x09, 0x04},  // English
    [1] = "Regression",                 // Manufacturer
    [2] = "SimpleKey Tri-mode",         // Product
    [3] = "000001",                     // Serial
    [4] = "SimpleKey Config",           // v2.3 CDC 配置口
};

// 配置描述符：4 接口（IF0=键盘 EP0x81 Report ID1，IF1=Consumer EP0x82，
// IF2/IF3=CDC 配置口，通知 EP0x83、数据 IN EP0x84 / OUT EP0x04）
static const uint8_t config_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, TUSB_DESC_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 0, true, sizeof(hid_report_desc_keyboard), 0x81, 64, 10),
    TUD_HID_DESCRIPTOR(1, 0, false, sizeof(hid_report_desc_consumer), 0x82, 64, 10),
    TUD_CDC_DESCRIPTOR(2, 4, 0x83, 64, 0x84, 0x04, 64),
};

// ==================== 全局变量 ====================
static bool usb_initialized = false;
static bool usb_connected = false;

// ==================== TinyUSB 回调函数 ====================

// 设备枚举完成回调
void tud_mount_cb(void) {
    ESP_LOGI(TAG, "USB mounted");
    usb_connected = true;
}

// 设备卸载回调
void tud_umount_cb(void) {
    ESP_LOGI(TAG, "USB unmounted");
    usb_connected = false;
}

// 挂起回调
void tud_suspend_cb(bool remote_wakeup_en) {
    ESP_LOGI(TAG, "USB suspended");
}

// 恢复回调
void tud_resume_cb(void) {
    ESP_LOGI(TAG, "USB resumed");
}

// HID 描述符报告回调（主机请求 Report Descriptor；实例 0=键盘 1=Consumer）
const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    return (instance == 0) ? hid_report_desc_keyboard : hid_report_desc_consumer;
}

// HID 获取报告回调
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

// HID 设置报告回调（接收 LED 状态等）
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

// ==================== 初始化函数 ====================
esp_err_t usb_hid_init(void) {
    ESP_LOGD(TAG, "Initializing USB HID...");
    
    if (usb_initialized) {
        ESP_LOGW(TAG, "USB HID already initialized");
        return ESP_OK;
    }
    
    // 配置 TinyUSB
    tinyusb_config_t tusb_cfg = {
        .device_descriptor = &device_descriptor,
        .string_descriptor = string_desc,
        .string_descriptor_count = sizeof(string_desc) / sizeof(string_desc[0]),
        .configuration_descriptor = config_descriptor,
        .self_powered = false,
        .vbus_monitor_io = -1,
    };
    
    // 安装 TinyUSB 驱动
    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TinyUSB driver: %s", esp_err_to_name(ret));
        return ret;
    }

    // v2.3：初始化 CDC ACM（配置通路），再把控制台三流重定向到 CDC。
    // 成功后 serial_task 的 fgetc(stdin) 与 printf/fwrite(stdout) 自动走 CDC，
    // 文本命令 / AA 55 帧 / [EVT] 日志在 USB 模式下经此虚拟串口收发。
    tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = NULL,             // 读走 VFS（stdin），无需回调
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ret = tusb_cdc_acm_init(&acm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init CDC ACM: %s", esp_err_to_name(ret));
        tinyusb_driver_uninstall();
        return ret;
    }
    ret = esp_tusb_init_console(TINYUSB_CDC_ACM_0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to redirect console to CDC: %s", esp_err_to_name(ret));
        tusb_cdc_acm_deinit(TINYUSB_CDC_ACM_0);
        tinyusb_driver_uninstall();
        return ret;
    }
    // 关闭行尾转换（默认 CRLF/CR 会改写二进制帧中的 0x0A/0x0D 字节损坏 CRC），
    // LF=收发均逐字节直通；文本行的 '\n' 保持原样，上位机按 '\n' 分行不受影响
    esp_vfs_tusb_cdc_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    esp_vfs_tusb_cdc_set_rx_line_endings(ESP_LINE_ENDINGS_LF);

    usb_initialized = true;
    ESP_LOGI(TAG, "USB HID+CDC initialized, VID=0x%04X PID=0x%04X (console -> CDC)",
             device_descriptor.idVendor, device_descriptor.idProduct);

    return ESP_OK;
}

esp_err_t usb_hid_deinit(void) {
    ESP_LOGD(TAG, "Deinitializing USB HID...");

    if (!usb_initialized) {
        ESP_LOGW(TAG, "USB HID not initialized");
        return ESP_OK;
    }

    // v2.3：先把控制台还原到 UART0（再卸载 CDC，保证还原时设备仍可用）
    esp_tusb_deinit_console(TINYUSB_CDC_ACM_0);
    tusb_cdc_acm_deinit(TINYUSB_CDC_ACM_0);

    // 卸载 TinyUSB 驱动
    esp_err_t ret = tinyusb_driver_uninstall();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to uninstall TinyUSB driver: %s", esp_err_to_name(ret));
        return ret;
    }
    
    usb_initialized = false;
    usb_connected = false;
    
    ESP_LOGD(TAG, "USB HID deinitialized");
    return ESP_OK;
}

// ==================== 发送函数 ====================
// 发送门控：必须用 tud_mounted()（cfg_num）而非自定义 usb_connected。
// 总线复位时 usbd_reset 立即清零 cfg_num 并复位 HID 接口（ep_in=0），
// 而 usb_connected 要等 tud_umount_cb 才清除——若仅查 usb_connected，
// 复位窗口内的报告会被排到 EP0（日志实测 "Queue EP 00 with 9 bytes"）
static bool usb_hid_can_send(void) {
    return usb_initialized && tud_mounted();
}

esp_err_t usb_hid_send_keyboard_report(const usb_hid_keyboard_report_t *report) {
    if (!usb_hid_can_send()) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // 发送 HID 键盘报告（实例 0，Report ID=1，9 字节 = ID + 8 字节）
    if (!tud_hid_n_report(0, 1, report, sizeof(usb_hid_keyboard_report_t))) {
        ESP_LOGW(TAG, "Failed to send keyboard report");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t usb_hid_send_consumer_report(const usb_hid_consumer_report_t *report) {
    if (!usb_hid_can_send()) {
        return ESP_ERR_INVALID_STATE;
    }

    // 发送 Consumer Control 报告（实例 1，无 Report ID，1 字节位域）
    if (!tud_hid_n_report(1, 0, report, sizeof(usb_hid_consumer_report_t))) {
        ESP_LOGW(TAG, "Failed to send consumer report");
        return ESP_FAIL;
    }

    return ESP_OK;
}

// ==================== 便捷函数 ====================
bool usb_hid_is_connected(void) {
    return usb_initialized && usb_connected;
}

esp_err_t usb_hid_send_key(uint8_t key_code, uint8_t modifiers, bool pressed) {
    usb_hid_keyboard_report_t report = {0};
    
    if (pressed) {
        report.modifiers = modifiers;
        report.keys[0] = key_code;
    }
    
    return usb_hid_send_keyboard_report(&report);
}

esp_err_t usb_hid_send_volume(bool volume_up) {
    usb_hid_consumer_report_t report = {
        .buttons = volume_up ? HID_CONSUMER_VOLUME_UP : HID_CONSUMER_VOLUME_DOWN,
    };

    esp_err_t ret = usb_hid_send_consumer_report(&report);

    // press 与 release 之间留间隔：背靠背报告可能被主机合并/漏识别，
    // 导致一次步进无效甚至按键卡住（参考 mands_typing 类实现均有间隔）
    vTaskDelay(pdMS_TO_TICKS(10));

    // 发送释放报告
    report.buttons = 0;
    usb_hid_send_consumer_report(&report);

    return ret;
}
