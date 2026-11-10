/**
 * @file ble_hid.c
 * @brief ESP32-S3 三模键盘 - BLE HID 设备
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 * 
 * 使用 NimBLE 实现 BLE HID 设备
 * - HID over GATT 服务
 * - 3 个配对槽位，独立虚拟 MAC
 * - 自动重连上次配对的设备
 * 
 * 注意事项：
 * 1. USB 和 BLE 可能存在资源冲突，使用 hid_manager 统一管理
 * 2. 同一时间只能初始化一个 HID 接口
 */

#include "ble_hid.h"
#include "pin_config.h"
#include "tasks_config.h"
#include "cfg_frame.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "store/config/ble_store_config.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_att.h"

static const char *TAG = "BLE_HID";

// IDF v5.4 的 ble_store_config.h 未导出该函数原型（实现在 ble_store_config.c）
extern void ble_store_config_init(void);

// ==================== 全局变量 ====================
static bool ble_initialized = false;
static ble_state_t ble_state = BLE_STATE_IDLE;
static uint8_t current_slot = 0;
static ble_slot_info_t slot_info[BLE_MAX_SLOTS] = {0};

// BLE 地址（每个槽位使用独立 MAC）
// 注意：随机静态地址最高字节的高两位必须为 1（0xC0~0xFF），否则 ble_hs_id_set_rnd 会拒绝
static uint8_t ble_addr[BLE_MAX_SLOTS][6] = {
    {0x11, 0x22, 0x33, 0x44, 0x55, 0xC1},  // 槽位 1
    {0x11, 0x22, 0x33, 0x44, 0x55, 0xC2},  // 槽位 2
    {0x11, 0x22, 0x33, 0x44, 0x55, 0xC3},  // 槽位 3
};

// HID Generic Appearance 值（蓝牙 SIG 分配）
#define BLE_APPEARANCE_HID_GENERIC  0x03C1

// ==================== HID over GATT 数据定义 ====================
// HID Information：bcdHID=1.1, bCountryCode=0, Flags=0x02（Normally Connectable）
static const uint8_t hid_info_val[4] = {0x01, 0x01, 0x00, 0x02};
// Protocol Mode：0x00=Boot，0x01=Report（默认 Report 协议）
static uint8_t proto_mode_val = 0x01;
// Report Reference 描述符值：[Report ID, Report Type(Input=1)]
static const uint8_t kb_rpt_ref_val[2] = {0x01, 0x01};          // ID 1 = 键盘
static const uint8_t consumer_rpt_ref_val[2] = {0x02, 0x01};    // ID 2 = Consumer

// 特征值当前数据（notify 用；输入报告值含 Report ID 前缀）
static uint8_t kb_rpt_val[9] = {0x01, 0, 0, 0, 0, 0, 0, 0, 0};          // ID 1 键盘
static uint8_t consumer_rpt_val[2] = {0x02, 0};                         // ID 2 Consumer（位域）
static uint8_t boot_kb_in_val[8] = {0};                                 // Boot 键盘输入
static uint8_t boot_kb_out_val = 0;                                     // Boot 键盘输出（主机 LED 状态）

/**
 * @brief BLE HID Report Map（功能与 USB 描述符一致，增加 Report ID）
 */
static const uint8_t hid_report_map[] = {
    // 键盘（Report ID 1）
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute) 修饰键
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Constant) 保留字节
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x05,        //   Report Count (5)
    0x91, 0x02,        //   Output (Data, Variable, Absolute) 键盘 LED
    0x75, 0x03,        //   Report Size (3)
    0x95, 0x01,        //   Report Count (1)
    0x91, 0x03,        //   Output (Constant) 填充
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0xFF,        //   Usage Maximum (255)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x06,        //   Report Count (6)
    0x81, 0x00,        //   Input (Data, Array) 按键码
    0xC0,              // End Collection
    // Consumer Control（Report ID 2）
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
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

// 可读特征值访问辅助结构（access_cb 的 arg）
struct hid_chr_value {
    const void *data;
    uint16_t len;
};
static const struct hid_chr_value hid_info_acc = {hid_info_val, sizeof(hid_info_val)};
static const struct hid_chr_value report_map_acc = {hid_report_map, sizeof(hid_report_map)};
static const struct hid_chr_value kb_rpt_acc = {kb_rpt_val, sizeof(kb_rpt_val)};
static const struct hid_chr_value consumer_rpt_acc = {consumer_rpt_val, sizeof(consumer_rpt_val)};
static const struct hid_chr_value boot_kb_in_acc = {boot_kb_in_val, sizeof(boot_kb_in_val)};

// ==================== NVS 操作 ====================
static esp_err_t ble_load_slot_info(void) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open("ble_slots", NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No slot info found in NVS");
        return ret;
    }
    
    size_t size = sizeof(slot_info);
    ret = nvs_get_blob(handle, "slots", slot_info, &size);
    nvs_close(handle);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Loaded slot info from NVS");
        for (int i = 0; i < BLE_MAX_SLOTS; i++) {
            if (slot_info[i].paired) {
                ESP_LOGD(TAG, "  Slot %d: %s", i + 1, slot_info[i].device_name);
            }
        }
    }
    
    return ret;
}

static esp_err_t ble_save_slot_info(void) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open("ble_slots", NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS");
        return ret;
    }
    
    ret = nvs_set_blob(handle, "slots", slot_info, sizeof(slot_info));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    return ret;
}

// ==================== 对端设备名读取 ====================
/**
 * @brief GATT client 读取对端 GAP 设备名（0x2A00）的异步回调
 *
 * 直接对全句柄区间发 Read By Type 请求，无需服务发现；名称超长时截断。
 * 读失败（error 非零或 attr 为空）时静默忽略，名称留空，下次连接自动重试。
 */
static int ble_peer_name_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg) {
    (void)conn_handle;
    int slot = (int)(intptr_t)arg;
    if ((error != NULL && error->status != 0) || attr == NULL || attr->om == NULL) {
        ESP_LOGD(TAG, "Peer name read failed rc=%d (slot %d)",
                 error ? error->status : BLE_HS_EUNKNOWN, slot + 1);
        return 0;
    }
    uint16_t len = OS_MBUF_PKTLEN(attr->om);
    if (len == 0) {
        return 0;
    }
    if (len > sizeof(slot_info[slot].device_name) - 1) {
        len = sizeof(slot_info[slot].device_name) - 1;
    }
    char name[sizeof(slot_info[slot].device_name)];
    os_mbuf_copydata(attr->om, 0, len, name);
    name[len] = '\0';

    // 名称变化才写 NVS（避免每次连接的无效写入）
    if (strcmp(name, slot_info[slot].device_name) != 0) {
        memcpy(slot_info[slot].device_name, name, len + 1);
        int rc = ble_save_slot_info();
        ESP_LOGI(TAG, "Peer name '%s' recorded to slot %d (save rc=%d)",
                 name, slot + 1, rc);
    }
    return 0;
}

/**
 * @brief 异步读取对端设备名并写入指定槽位（连接建立且加密成功后调用）
 */
static void ble_peer_name_read_start(uint16_t conn_handle, int slot) {
    if (slot < 0 || slot >= BLE_MAX_SLOTS) {
        return;
    }
    ble_uuid16_t uuid = BLE_UUID16_INIT(0x2A00);
    int rc = ble_gattc_read_by_uuid(conn_handle, 1, 0xFFFF, &uuid.u,
                                    ble_peer_name_cb, (void *)(intptr_t)slot);
    if (rc != 0) {
        ESP_LOGD(TAG, "Peer name read start failed rc=%d (slot %d)", rc, slot + 1);
    }
}

// ==================== 配对槽位记录 ====================
/**
 * @brief 配对/绑定成功后将对端记录到槽位（GAP 加密成功事件时调用）
 *
 * NimBLE 无独立配对完成事件，加密成功（ENC_CHANGE status=0）即配对/绑定成功：
 * 新配对完成与旧密钥重连均会触发，本函数幂等。槽位分配策略（简单确定性）：
 *   a. 槽位中已有相同对端（addr_type+peer_addr 完全一致）→ 刷新该槽位（信息不变，免写 NVS）；
 *   b. current_slot 为空 → 记录到 current_slot；
 *   c. 否则记录到第一个空槽；
 *   d. 全满且非已知对端 → 覆盖记录到 current_slot。
 *
 * 对端地址优先取 ID 地址（配对后交换的长期身份地址，RPA 场景下稳定不变），
 * ID 地址为全零时退回 OTA 地址；device_name 由 GATT client 异步读取对端
 * GAP 服务名（0x2A00）后回填并持久化（旧数据名称空缺的槽位重连时自动补齐）。
 */
static void ble_record_pairing(uint16_t conn_handle) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return;
    }

    ble_addr_t peer = desc.peer_id_addr;
    static const uint8_t zero_addr[6] = {0};
    if (memcmp(peer.val, zero_addr, sizeof(zero_addr)) == 0) {
        peer = desc.peer_ota_addr;
    }

    int slot = -1;
    // 策略 a：已知对端 → 刷新原槽位（地址与类型均未变，无需重写 NVS）
    for (int i = 0; i < BLE_MAX_SLOTS; i++) {
        if (slot_info[i].paired &&
            slot_info[i].addr_type == peer.type &&
            memcmp(slot_info[i].peer_addr, peer.val, sizeof(peer.val)) == 0) {
            slot = i;
            // 名称空缺（旧版 NVS 数据）→ 本次连接自动补齐
            if (slot_info[slot].device_name[0] == '\0') {
                ble_peer_name_read_start(conn_handle, slot);
            }
            ESP_LOGD(TAG, "Peer already recorded in slot %d, refresh skipped", slot + 1);
            break;
        }
    }

    if (slot < 0) {
        // 策略 b/c/d：空 current_slot → 第一个空槽 → 覆盖 current_slot
        if (!slot_info[current_slot].paired) {
            slot = current_slot;
        } else {
            for (int i = 0; i < BLE_MAX_SLOTS; i++) {
                if (!slot_info[i].paired) {
                    slot = i;
                    break;
                }
            }
        }
        if (slot < 0) {
            slot = current_slot;  // 全满：覆盖当前槽位
        }
        slot_info[slot].paired = true;
        slot_info[slot].addr_type = peer.type;
        memcpy(slot_info[slot].peer_addr, peer.val, sizeof(slot_info[slot].peer_addr));
        slot_info[slot].device_name[0] = '\0';
        esp_err_t ret = ble_save_slot_info();
        ESP_LOGI(TAG, "Pairing recorded to slot %d (%02X:%02X:%02X:%02X:%02X:%02X), save rc=%d",
                 slot + 1, peer.val[5], peer.val[4], peer.val[3],
                 peer.val[2], peer.val[1], peer.val[0], ret);
        // 异步读取对端设备名回填（GATT client 读对端 GAP 服务 0x2A00）
        ble_peer_name_read_start(conn_handle, slot);
    }
}

// ==================== NimBLE 回调函数 ====================
static void ble_start_advertising(void);  // 前置声明（断线重连时使用）

// 连接与订阅状态
static uint16_t ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;  // 当前连接句柄
static uint16_t kb_rpt_handle = 0;          // 键盘 Input Report 特征值句柄
static uint16_t consumer_rpt_handle = 0;    // Consumer Input Report 特征值句柄
static uint16_t boot_kb_in_handle = 0;      // Boot 键盘输入特征值句柄
static bool kb_subscribed = false;          // 键盘报告 CCCD 已订阅
static bool consumer_subscribed = false;    // Consumer 报告 CCCD 已订阅
static uint16_t monitor_rpt_handle = 0;     // 监控特征值句柄（0xFFE1）
static bool monitor_subscribed = false;     // 监控特征值 CCCD 已订阅

// BLE 配置通路（AA 55 帧协议走 0xFFE1：Write 下行、Notify 上行）
static cfg_frame_parser_t ble_cfg_parser;
static void ble_cfg_send(const uint8_t *frame, uint16_t len);  // 应答发送（供 cfg_frame 回调）

// GAP 事件回调
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ble_conn_handle = event->connect.conn_handle;
                ESP_LOGI(TAG, "Connected to device");
                ble_state = BLE_STATE_CONNECTED;
                // 主动发起配对/加密（HID 需要加密链路；主机通常也会发起，此处兜底）
                ble_gap_security_initiate(ble_conn_handle);
            } else {
                ESP_LOGW(TAG, "Connection failed, status: %d", event->connect.status);
                ble_state = BLE_STATE_ADVERTISING;
                // 继续广播
                ble_start_advertising();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected, reason: %d", event->disconnect.reason);
            // 认证/配对类失败（SM 层）：解除该对端绑定，允许主机删除配对后重新配对
            if (event->disconnect.reason >= BLE_HS_ERR_SM_US_BASE &&
                event->disconnect.reason < BLE_HS_ERR_SM_PEER_BASE + 0x100) {
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(event->disconnect.conn.conn_handle, &desc) == 0) {
                    ESP_LOGW(TAG, "Pairing/auth failure, unpairing stale peer");
                    ble_gap_unpair(&desc.peer_id_addr);
                }
            }
            ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            kb_subscribed = false;
            consumer_subscribed = false;
            monitor_subscribed = false;
            ble_state = BLE_STATE_ADVERTISING;
            // 重新开始广播
            ble_start_advertising();
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGD(TAG, "Advertising complete");
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == kb_rpt_handle) {
                kb_subscribed = (event->subscribe.cur_notify != 0);
                ESP_LOGI(TAG, "Keyboard report %s", kb_subscribed ? "subscribed" : "unsubscribed");
            } else if (event->subscribe.attr_handle == consumer_rpt_handle) {
                consumer_subscribed = (event->subscribe.cur_notify != 0);
                ESP_LOGI(TAG, "Consumer report %s", consumer_subscribed ? "subscribed" : "unsubscribed");
            } else if (event->subscribe.attr_handle == monitor_rpt_handle) {
                monitor_subscribed = (event->subscribe.cur_notify != 0);
                ESP_LOGI(TAG, "Monitor report %s", monitor_subscribed ? "subscribed" : "unsubscribed");
            } else {
                ESP_LOGD(TAG, "Subscribe: handle=%u notify=%u",
                         event->subscribe.attr_handle, event->subscribe.cur_notify);
            }
            break;

        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "Encryption change: status=%d", event->enc_change.status);
            if (event->enc_change.status != 0) {
                // 加密失败（典型场景：主机侧删除配对后设备仍持有旧密钥）：
                // 解除对该对端的绑定并重新广播，允许主机重新配对
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                    ESP_LOGW(TAG, "Encryption failed, unpairing stale peer");
                    ble_gap_unpair(&desc.peer_id_addr);
                }
            } else {
                // 加密成功 = 配对/绑定完成（NimBLE 无独立配对完成事件；旧密钥重连亦触发，
                // 幂等）：将对端记录到配对槽位并持久化（0x13 读槽位列表的数据来源）
                ble_record_pairing(event->enc_change.conn_handle);
            }
            break;
    }
    
    return 0;
}

// ==================== GATT 访问回调 ====================

/**
 * @brief 可读特征值通用回调（arg = struct hid_chr_value）
 */
static int hid_read_cb(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    const struct hid_chr_value *val = (const struct hid_chr_value *)arg;
    if (val == NULL) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, val->data, val->len);
}

/**
 * @brief Protocol Mode 回调（读 + 无响应写）
 */
static int hid_proto_mode_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &proto_mode_val, 1);
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ctxt->om->om_len >= 1) {
        proto_mode_val = (ctxt->om->om_data[0] == 0x00) ? 0x00 : 0x01;
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/**
 * @brief Boot 键盘输出回调（主机下发键盘 LED 状态，当前仅接收）
 */
static int hid_boot_out_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &boot_kb_out_val, 1);
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ctxt->om->om_len >= 1) {
        boot_kb_out_val = ctxt->om->om_data[0];
        ESP_LOGD(TAG, "Boot keyboard output (LED): 0x%02X", boot_kb_out_val);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/**
 * @brief Control Point 回调（SUSPEND/RESUME，当前仅记录）
 */
static int hid_ctrl_pt_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ctxt->om->om_len >= 1) {
        ESP_LOGD(TAG, "HID control point: 0x%02X", ctxt->om->om_data[0]);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/**
 * @brief Report Reference 描述符回调（arg = 2 字节描述符值）
 */
static int hid_rpt_ref_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC && arg != NULL) {
        return os_mbuf_append(ctxt->om, arg, 2);
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/**
 * @brief 监控特征值访问回调（0xFFE1：READ 空值 + WRITE 喂配置帧解析器）
 *
 * 数据下行：上位机 Write 0xFFE1 → 遍历 om 链逐字节喂入 cfg_frame（BLE 传输），
 * 应答经 ble_cfg_send 以 Notify 原路返回（AA 55 帧协议，与串口同一协议）
 */
static int monitor_read_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0;  // 空值，事件经 notify 推送
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // om 是链表，长写可能跨多个 mbuf，需逐段遍历
        for (struct os_mbuf *om = ctxt->om; om != NULL; om = SLIST_NEXT(om, om_next)) {
            for (int i = 0; i < om->om_len; i++) {
                cfg_frame_feed(&ble_cfg_parser, om->om_data[i], CFG_TRANSPORT_BLE,
                               ble_cfg_send, NULL);
            }
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// HID Service (0x1812) GATT 服务表
static const struct ble_gatt_svc_def hid_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {   // HID Information（只读）
                .uuid = BLE_UUID16_DECLARE(0x2A4A),
                .access_cb = hid_read_cb,
                .arg = (void *)&hid_info_acc,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {   // Report Map（只读）
                .uuid = BLE_UUID16_DECLARE(0x2A4B),
                .access_cb = hid_read_cb,
                .arg = (void *)&report_map_acc,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {   // Control Point（无响应写）
                .uuid = BLE_UUID16_DECLARE(0x2A4C),
                .access_cb = hid_ctrl_pt_cb,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {   // Protocol Mode（读 + 无响应写）
                .uuid = BLE_UUID16_DECLARE(0x2A4E),
                .access_cb = hid_proto_mode_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {   // 键盘 Input Report（通知，Report ID 1）
                .uuid = BLE_UUID16_DECLARE(0x2A4D),
                .access_cb = hid_read_cb,
                .arg = (void *)&kb_rpt_acc,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &kb_rpt_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(0x2908),
                        .access_cb = hid_rpt_ref_cb,
                        .arg = (void *)kb_rpt_ref_val,
                        .att_flags = BLE_ATT_F_READ,
                    },
                    { 0 },
                },
            },
            {   // Consumer Input Report（通知，Report ID 2）
                .uuid = BLE_UUID16_DECLARE(0x2A4D),
                .access_cb = hid_read_cb,
                .arg = (void *)&consumer_rpt_acc,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &consumer_rpt_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(0x2908),
                        .access_cb = hid_rpt_ref_cb,
                        .arg = (void *)consumer_rpt_ref_val,
                        .att_flags = BLE_ATT_F_READ,
                    },
                    { 0 },
                },
            },
            {   // Boot 键盘输入（通知）
                .uuid = BLE_UUID16_DECLARE(0x2A22),
                .access_cb = hid_read_cb,
                .arg = (void *)&boot_kb_in_acc,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &boot_kb_in_handle,
            },
            {   // Boot 键盘输出（读写，主机下发 LED 状态）
                .uuid = BLE_UUID16_DECLARE(0x2A32),
                .access_cb = hid_boot_out_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 },
        },
    },
    {   // 调试监控服务（自定义，非 HID：系统 HID 驱动独占 0x1812，第三方应用
        //     无法订阅其 Input Report，故用独立服务承担监控 + 配置通路）
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFFE0),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0xFFE1),
                .access_cb = monitor_read_cb,
                // READ+NOTIFY=监控事件上行；WRITE/WRITE_NO_RSP=AA 55 配置帧下行
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &monitor_rpt_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

// GATT 服务初始化
static void ble_gatt_init(void) {
    // 注册标准 GAP/GATT 服务（0x1800/0x1801）
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // 预分配属性表资源并注册 HID 服务
    int rc = ble_gatts_count_cfg(hid_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to count GATT svcs: %d", rc);
        return;
    }

    rc = ble_gatts_add_svcs(hid_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to add GATT svcs: %d", rc);
    }
}

// BLE 主机任务
static void ble_host_task(void *param) {
    ESP_LOGD(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ==================== 广播 ====================
/**
 * @brief 设置广播数据并开始广播
 * 广播包含：AD Flags、完整设备名、Appearance（HID）、HID 服务 UUID 0x1812
 */
static void ble_start_advertising(void) {
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    // AD Flags：通用可发现 + 不支持 BR/EDR（纯 BLE 设备）
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    // 完整设备名
    fields.name = (const uint8_t *)BLE_DEVICE_NAME;
    fields.name_len = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;

    // Appearance：HID Generic（主机据此显示键盘图标）
    fields.appearance = BLE_APPEARANCE_HID_GENERIC;
    fields.appearance_is_present = 1;

    // HID 服务 UUID 0x1812
    fields.uuids16 = (ble_uuid16_t[]) {
        BLE_UUID16_INIT(0x1812)
    };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set adv fields: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {
        .itvl_min = BLE_ADV_INTERVAL_MIN,
        .itvl_max = BLE_ADV_INTERVAL_MAX,
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };

    rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start advertising: %d", rc);
        return;
    }

    ble_state = BLE_STATE_ADVERTISING;
    ESP_LOGI(TAG, "Advertising started: %s", BLE_DEVICE_NAME);
}

// BLE 同步回调
static void ble_on_sync(void) {
    ESP_LOGD(TAG, "BLE synced");

    // 设置设备名称
    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);

    // 设置随机静态地址
    ble_addr_t addr = {
        .type = BLE_ADDR_RANDOM,
    };
    memcpy(addr.val, ble_addr[current_slot], 6);
    int rc = ble_hs_id_set_rnd(addr.val);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set random address: %d", rc);
    }

    // 开始广播
    ble_start_advertising();
}

// BLE 重置回调
static void ble_on_reset(int reason) {
    ESP_LOGE(TAG, "BLE reset, reason: %d", reason);
}

// ==================== 初始化函数 ====================
/**
 * @brief NimBLE 实际初始化（重量级，需在大栈上下文执行）
 */
static esp_err_t ble_hid_init_impl(void) {
    ESP_LOGD(TAG, "Initializing BLE HID...");

    // 初始化 NimBLE
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE: %s", esp_err_to_name(ret));
        return ret;
    }

    // 配置 NimBLE
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    // 安全与绑定配置（HID 设备需要配对加密）
    ble_hs_cfg.sm_bonding = 1;                  // 允许绑定
    ble_hs_cfg.sm_mitm = 0;                     // NoInputNoOutput 无法提供 MITM 保护
    ble_hs_cfg.sm_sc = 1;                       // LE Secure Connections
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO; // 无输入无输出能力
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    // 绑定信息持久化到 NVS（需 CONFIG_BT_NIMBLE_NVS_PERSIST=y）
    ble_store_config_init();

    // 初始化 GATT 服务
    ble_gatt_init();

    // 复位 BLE 配置帧解析器（仅状态机；SEQ 窗口全局共享，由 PING 重同步）
    cfg_frame_parser_init(&ble_cfg_parser);

    // 启动 NimBLE 主机任务
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE HID initialized");

    return ESP_OK;
}

// 大栈工作线程上下文：NimBLE 初始化栈需求远大于 serial_task/main_task 的栈
typedef struct {
    SemaphoreHandle_t done;   // 完成信号量
    esp_err_t result;         // 初始化结果
} ble_init_ctx_t;

static void ble_init_worker(void *arg) {
    ble_init_ctx_t *ctx = (ble_init_ctx_t *)arg;
    ctx->result = ble_hid_init_impl();
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

esp_err_t ble_hid_init(void) {
    ESP_LOGD(TAG, "Initializing BLE HID...");

    if (ble_initialized) {
        ESP_LOGW(TAG, "BLE HID already initialized");
        return ESP_OK;
    }

    // 加载槽位信息
    ble_load_slot_info();

    // 在大栈临时任务中执行初始化，避免调用者任务（serial_task/main_task）栈溢出
    ble_init_ctx_t ctx = {
        .done = xSemaphoreCreateBinary(),
        .result = ESP_FAIL,
    };
    if (ctx.done == NULL) {
        ESP_LOGE(TAG, "Failed to create init semaphore");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(ble_init_worker, "ble_init", 8192, &ctx,
                                TASK_PRIORITY_BLE_HID, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create init worker task");
        vSemaphoreDelete(ctx.done);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(ctx.done, portMAX_DELAY);
    vSemaphoreDelete(ctx.done);

    if (ctx.result == ESP_OK) {
        ble_initialized = true;
    }
    return ctx.result;
}

esp_err_t ble_hid_deinit(void) {
    ESP_LOGD(TAG, "Deinitializing BLE HID...");
    
    if (!ble_initialized) {
        ESP_LOGW(TAG, "BLE HID not initialized");
        return ESP_OK;
    }
    
    // 停止广播
    ble_gap_adv_stop();
    
    // 断开连接
    if (ble_state == BLE_STATE_CONNECTED) {
        ble_gap_terminate(0, BLE_ERR_REM_USER_CONN_TERM);
    }
    
    // 反初始化 NimBLE：必须先 port_stop 让 host 任务（nimble_port_run 循环）优雅退出，
    // 再 port_deinit 拆资源。原实现直接 deinit，host 任务仍在运行时 ble_hs_mutex 等
    // 资源被拆毁，切到 WiFi 模式后残留任务处理 HCI 事件即触发 spinlock assert 崩溃
    // 重启（2026-10-10 coredump 定位：assert failed: spinlock_release lock->count < 0x100）
    ble_initialized = false;   // 先关门：阻止其他任务在拆解期间并发调用 BLE API
    ble_state = BLE_STATE_IDLE;
    esp_err_t stop_ret = nimble_port_stop();
    if (stop_ret == ESP_OK) {
        nimble_port_deinit();
    } else {
        ESP_LOGW(TAG, "nimble_port_stop failed: %d", stop_ret);
    }
    
    ESP_LOGD(TAG, "BLE HID deinitialized");
    return ESP_OK;
}

// ==================== 发送函数 ====================
/**
 * @brief 通过 notify 发送报告特征值
 */
static esp_err_t ble_hid_notify(uint16_t attr_handle, const uint8_t *data, size_t len) {
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int rc = ble_gatts_notify_custom(ble_conn_handle, attr_handle, om);
    if (rc != 0) {
        // 发送失败时释放 mbuf（成功时由协议栈接管）
        os_mbuf_free_chain(om);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ble_hid_send_keyboard_report(const uint8_t *report, size_t len) {
    if (!ble_initialized || ble_state != BLE_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!kb_subscribed) {
        // 主机未订阅键盘报告（正常 HID 主机连接后必然订阅；未订阅多为 GATT 缓存
        // 过期——删除配对重配即可）。静默丢弃会导致"按键无输出"难以排查，故告警
        ESP_LOGW(TAG, "Key report dropped: host not subscribed (delete pairing and re-pair)");
        return ESP_OK;
    }

    if (proto_mode_val == 0x00) {
        // Boot 协议：8 字节，无 Report ID 前缀
        memcpy(boot_kb_in_val, report, 8);
        return ble_hid_notify(boot_kb_in_handle, boot_kb_in_val, 8);
    }

    // Report 协议：8 字节原始报告（**不加** Report ID 前缀——BLE 传输中特征值与
    // 报告的绑定关系由 Report Reference 描述符描述，manuelbl/wakwak-koba 等
    // Windows 验证可用的实现均直接发送原始报告；加前缀时首字节 0x01 会被主机
    // 当作修饰键（Ctrl 按下）且键码整体错位，导致按键无输出）
    memcpy(kb_rpt_val, report, 8);
    return ble_hid_notify(kb_rpt_handle, kb_rpt_val, 8);
}

esp_err_t ble_hid_send_monitor(const uint8_t *data, size_t len) {
    if (!ble_initialized || ble_state != BLE_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!monitor_subscribed) {
        return ESP_OK;  // 上位机未订阅监控时无需发送
    }
    if (data == NULL || len == 0 || len > 20) {
        return ESP_ERR_INVALID_ARG;  // 不超过默认 ATT MTU 23 的 notify 载荷上限
    }
    return ble_hid_notify(monitor_rpt_handle, data, len);
}

/**
 * @brief 配置帧应答发送（cfg_frame BLE 传输的 sink 回调）
 *
 * 与 ble_hid_send_monitor 不同：AA 55 应答帧可达 29B（KEYMAP_GET），
 * 超过默认 MTU 23 的 notify 载荷上限，需按协商后的 MTU 判断；
 * Windows 连接后会主动协商 MTU（可达 512），正常远大于 29
 */
static void ble_cfg_send(const uint8_t *frame, uint16_t len) {
    if (!ble_initialized || ble_state != BLE_STATE_CONNECTED) {
        return;  // 未连接时丢弃应答（帧解析已计入状态，无需补救）
    }
    if (!monitor_subscribed) {
        ESP_LOGW(TAG, "CFG resp dropped: monitor not subscribed");
        return;
    }
    uint16_t mtu = ble_att_mtu(ble_conn_handle);
    if (mtu < len + 3u) {
        ESP_LOGW(TAG, "CFG resp dropped: MTU %u too small for %u bytes", mtu, len);
        return;
    }
    ble_hid_notify(monitor_rpt_handle, frame, len);
}

esp_err_t ble_hid_send_consumer_report(const uint8_t *report, size_t len) {
    if (!ble_initialized || ble_state != BLE_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!consumer_subscribed) {
        return ESP_OK;  // 主机未订阅报告时无需发送
    }

    // Report 协议：1 字节位域原始报告（**不加** Report ID 前缀，原因同键盘报告；
    // 加前缀时首字节 0x02 会被主机当作位域 bit1=音量减，导致音量只减不增）
    consumer_rpt_val[0] = report[0];
    return ble_hid_notify(consumer_rpt_handle, consumer_rpt_val, 1);
}

// ==================== 便捷函数 ====================
bool ble_hid_is_connected(void) {
    return ble_initialized && (ble_state == BLE_STATE_CONNECTED);
}

ble_state_t ble_hid_get_state(void) {
    return ble_state;
}

esp_err_t ble_hid_send_key(uint8_t key_code, uint8_t modifiers, bool pressed) {
    uint8_t report[8] = {0};
    
    if (pressed) {
        report[0] = modifiers;
        report[2] = key_code;
    }
    
    return ble_hid_send_keyboard_report(report, sizeof(report));
}

esp_err_t ble_hid_send_volume(bool volume_up) {
    // 位域：bit0=Vol+ bit1=Vol-（与报告描述符一致）
    uint8_t report[1] = {volume_up ? 0x01 : 0x02};

    esp_err_t ret = ble_hid_send_consumer_report(report, sizeof(report));

    // press 与 release 之间留间隔：背靠背 notify 可能被主机合并/漏识别，
    // 导致步进无效甚至按键卡住（参考 mands_typing 类实现均有间隔）
    vTaskDelay(pdMS_TO_TICKS(10));

    // 发送释放报告
    report[0] = 0;
    ble_hid_send_consumer_report(report, sizeof(report));

    return ret;
}

// ==================== 配对槽位管理 ====================
esp_err_t ble_hid_get_slot_info(uint8_t slot, ble_slot_info_t *info) {
    if (slot >= BLE_MAX_SLOTS || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(info, &slot_info[slot], sizeof(ble_slot_info_t));
    return ESP_OK;
}

esp_err_t ble_hid_clear_slot(uint8_t slot) {
    if (slot >= BLE_MAX_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }

    // 已配对槽位：先删该对端的 NimBLE 绑定（密钥+CCCD），再清槽位信息，
    // 否则只清 slot_info 会让主机侧仍持有密钥而设备侧丢失，重连必触发加密失败
    if (slot_info[slot].paired) {
        // 该槽位正处于连接中：先优雅断开（远端用户终止），断连事件随后异步到达
        if (slot == current_slot && ble_hid_is_connected() &&
            ble_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGI(TAG, "Clear slot %d: terminating active connection", slot + 1);
            ble_gap_terminate(ble_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        // 删除该对端全部绑定记录（NimBLE 标准 API，与 GAP 事件里的解除绑定同源）
        ble_addr_t peer;
        peer.type = slot_info[slot].addr_type;
        memcpy(peer.val, slot_info[slot].peer_addr, sizeof(peer.val));
        int rc = ble_gap_unpair(&peer);
        ESP_LOGI(TAG, "Clear slot %d (%s): unpair rc=%d", slot + 1,
                 slot_info[slot].device_name, rc);
    }

    memset(&slot_info[slot], 0, sizeof(ble_slot_info_t));
    return ble_save_slot_info();
}

esp_err_t ble_hid_connect_slot(uint8_t slot) {
    if (slot >= BLE_MAX_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!slot_info[slot].paired) {
        ESP_LOGW(TAG, "Slot %d not paired", slot + 1);
        return ESP_ERR_NOT_FOUND;
    }
    
    current_slot = slot;
    ESP_LOGI(TAG, "Connecting to slot %d: %s", slot + 1, slot_info[slot].device_name);
    
    // TODO: 实现连接逻辑
    
    return ESP_OK;
}

uint8_t ble_hid_get_current_slot(void) {
    return current_slot;
}

void ble_hid_set_current_slot(uint8_t slot) {
    if (slot < BLE_MAX_SLOTS) {
        current_slot = slot;
    }
}

void ble_hid_clear_bonds(void) {
    ble_store_clear();
    ESP_LOGI(TAG, "All BLE bonds cleared");
}
