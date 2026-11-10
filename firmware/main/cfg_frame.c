/**
 * @file cfg_frame.c
 * @brief ESP32-S3 三模键盘 - AA 55 二进制配置帧协议（多传输）
 * @author Regression
 * @version 1.1.0
 * @date 2026-09-20
 *
 * 帧格式：AA 55 | CMD | SEQ(2B LE) | LEN(2B LE) | PAYLOAD | CRC16(2B LE)
 * - 每条传输通路一个独立字节流解析器（串口与 BLE 的流互不干扰）
 * - SEQ 滑动窗口全局共享一份（同一上位机跨通路使用同一发号器）；
 *   重复帧的重发应答从「该重复帧到达的传输」原路返回
 * - 应答 = CMD|0x80，回显请求 SEQ，从帧到达的同一传输发出
 */

#include "cfg_frame.h"
#include "serial_cmd.h"
#include "keymap.h"
#include "profile_store.h"
#include "power_mgr.h"
#include "macro.h"
#include "sleep_mgr.h"
#include "ble_hid.h"
#include "tasks_config.h"   // BLE_MAX_SLOTS：BLE 配对槽位数
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "CFG";

// 协议版本（PING 应答 payload[0]）
#define CFG_PROTOCOL_VERSION    0x01

// 帧固定字段长度：CMD(1) + SEQ(2) + LEN(2)
#define CFG_FIXED_LEN           5

// ==================== 解析状态机（内部） ====================

typedef enum {
    ST_IDLE = 0,    // 等待 0xAA
    ST_HDR1,        // 已收 0xAA，等待 0x55
    ST_COLLECT,     // 已收帧头，继续收 CMD/SEQ/LEN/PAYLOAD/CRC
} parse_state_t;

// state 在结构体中是 uint8_t（cfg_frame.h），必须按 1 字节指针访问；
// 若按 4 字节 parse_state_t* 读写会越界踩到 buf[]，导致帧字节被静默吞掉
#define PARSER_STATE(p) ((uint8_t *)&(p)->state)

// ==================== SEQ 滑动窗口（全局共享） ====================

static uint16_t last_seq = 0;       // 最近接受的命令帧 SEQ
static bool seq_valid = false;      // 首帧接受（未握手时接受首帧并以其为基准）

// 最近应答缓存（按 SEQ 索引的环形数组）：重复 SEQ 到达时按 SEQ 精确重发该帧
// 自己的应答（而非「最近一次」应答——否则间隔重发旧 SEQ 会回错应答，回显 SEQ 不匹配）。
// 容量 32 > 滑动窗口宽度 17（±16），窗口内重复帧的应答必在缓存中
#define RESP_CACHE_SIZE     32
typedef struct {
    bool valid;
    uint16_t seq;
    uint8_t frame[CFG_FRAME_BUF_SIZE];
    uint16_t len;
    cfg_frame_send_fn sink;     // 应答原路返回（重复帧从到达的传输重发）
} resp_cache_entry_t;
static resp_cache_entry_t resp_cache[RESP_CACHE_SIZE];
static uint8_t resp_cache_next = 0;

// ==================== CRC16（CRC-16/CCITT-FALSE） ====================

static uint16_t crc16_ccitt(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}

// ==================== 帧组装 ====================

// 组装完整帧到 dst，返回总长（2 + 5 + len + 2）
static uint16_t build_frame(uint8_t *dst, uint8_t cmd, uint16_t seq,
                            const uint8_t *payload, uint16_t len) {
    uint16_t pos = 0;
    dst[pos++] = 0xAA;
    dst[pos++] = 0x55;
    dst[pos++] = cmd;
    dst[pos++] = (uint8_t)(seq & 0xFF);
    dst[pos++] = (uint8_t)(seq >> 8);
    dst[pos++] = (uint8_t)(len & 0xFF);
    dst[pos++] = (uint8_t)(len >> 8);
    if (len > 0 && payload != NULL) {
        memcpy(&dst[pos], payload, len);
        pos += len;
    }
    uint16_t crc = crc16_ccitt(&dst[2], pos - 2);   // CMD 起至 PAYLOAD 末
    dst[pos++] = (uint8_t)(crc & 0xFF);
    dst[pos++] = (uint8_t)(crc >> 8);
    return pos;
}

// 应答 = 命令字 | 0x80，回显请求 SEQ，从原传输发出并按 SEQ 缓存（重复 SEQ 精确重发用）
static void send_response(uint8_t req_cmd, uint16_t req_seq,
                          const uint8_t *payload, uint16_t len,
                          cfg_frame_send_fn sink) {
    uint8_t buf[CFG_FRAME_BUF_SIZE];
    uint16_t total = build_frame(buf, req_cmd | 0x80, req_seq, payload, len);
    sink(buf, total);

    // 按 SEQ 查找已有缓存项（重发同一 SEQ 的应答更新），否则占用下一个环形槽位
    resp_cache_entry_t *e = NULL;
    for (int i = 0; i < RESP_CACHE_SIZE; i++) {
        if (resp_cache[i].valid && resp_cache[i].seq == req_seq) {
            e = &resp_cache[i];
            break;
        }
    }
    if (e == NULL) {
        e = &resp_cache[resp_cache_next];
        resp_cache_next = (uint8_t)((resp_cache_next + 1) % RESP_CACHE_SIZE);
    }
    e->valid = true;
    e->seq = req_seq;
    memcpy(e->frame, buf, total);
    e->len = total;
    e->sink = sink;
}

// ==================== 命令处理 ====================

static void process_frame(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t len,
                          cfg_transport_t transport, cfg_frame_send_fn sink) {
    ESP_LOGI(TAG, "RX frame cmd=0x%02X seq=%u len=%u transport=%d", cmd, seq, len, transport);

    switch (cmd) {
    case CFG_CMD_PING: {
        // 握手/重同步：last_seq 对齐当前 SEQ
        uint8_t resp[1] = {CFG_PROTOCOL_VERSION};
        last_seq = seq;
        seq_valid = true;
        send_response(cmd, seq, resp, sizeof(resp), sink);
        break;
    }
    case CFG_CMD_VERSION: {
        const char *ver = "SimpleKey Tri-mode v1.0.0";
        send_response(cmd, seq, (const uint8_t *)ver, (uint16_t)strlen(ver), sink);
        break;
    }
    case CFG_CMD_KEYMAP_GET: {
        // 行优先 5x4 共 20 字节
        const uint8_t *km = keymap_get_current();
        send_response(cmd, seq, km, KEYMAP_ROWS * KEYMAP_COLS, sink);
        break;
    }
    case CFG_CMD_KEYMAP_SET: {
        // payload=[row, col, keycode]
        uint8_t status = CFG_ST_ERR_LEN;
        if (len == 3) {
            uint8_t row = payload[0], col = payload[1], kc = payload[2];
            if (row < KEYMAP_ROWS && col < KEYMAP_COLS) {
                keymap_set_key(row, col, kc);
                status = CFG_ST_OK;
            } else {
                status = CFG_ST_ERR_INVALID;
            }
        }
        send_response(cmd, seq, &status, 1, sink);
        break;
    }
    case CFG_CMD_KEYMAP_SAVE: {
        // 保存当前 Profile 全部配置到 NVS（P6 语义扩展：键位+旋钮+宏同属 profile_t，一并落盘）
        profile_t *p = profile_get_current();
        memcpy(p->keymap, keymap_get_current(), sizeof(p->keymap));
        profile_save();
        uint8_t status = CFG_ST_OK;
        send_response(cmd, seq, &status, 1, sink);
        break;
    }
    case CFG_CMD_PROFILE_GET: {
        // payload=[index]；应答 = name(16B) + keymap(20B) + default_mode(1B) 共 37B
        uint8_t resp[1 + PROFILE_NAME_LEN + KEYMAP_ROWS * KEYMAP_COLS + 1];
        uint16_t resp_len = 1;
        if (len != 1) {
            resp[0] = CFG_ST_ERR_LEN;
        } else {
            const profile_t *slot = profile_get_slot(payload[0]);
            if (slot == NULL) {
                resp[0] = CFG_ST_ERR_INVALID;
            } else {
                resp[0] = CFG_ST_OK;
                memcpy(&resp[1], slot->name, PROFILE_NAME_LEN);
                memcpy(&resp[1 + PROFILE_NAME_LEN], slot->keymap, KEYMAP_ROWS * KEYMAP_COLS);
                resp[1 + PROFILE_NAME_LEN + KEYMAP_ROWS * KEYMAP_COLS] = slot->default_mode;
                resp_len = sizeof(resp);
            }
        }
        send_response(cmd, seq, resp, resp_len, sink);
        break;
    }
    case CFG_CMD_PROFILE_SET: {
        // payload=[index]：切换当前 Profile（keymap 重载 + LED 快闪 N 次，颜色跟随当前模式，闪烁在 profile_set_current 内）
        uint8_t status = CFG_ST_ERR_LEN;
        if (len == 1) {
            if (payload[0] < PROFILE_COUNT) {
                profile_set_current(payload[0]);
                macro_rec_stop();   // Profile 切换自动退出录制
                status = CFG_ST_OK;
            } else {
                status = CFG_ST_ERR_INVALID;
            }
        }
        send_response(cmd, seq, &status, 1, sink);
        break;
    }
    case CFG_CMD_PROFILE_WRITE: {
        // payload=[index + name(16B) + keymap(20B)] 共 37B
        uint8_t status = CFG_ST_ERR_LEN;
        if (len == 1 + PROFILE_NAME_LEN + KEYMAP_ROWS * KEYMAP_COLS) {
            uint8_t idx = payload[0];
            // 名称强制以 \0 结尾（固件侧防御截断，防止越界读）
            char name[PROFILE_NAME_LEN];
            memcpy(name, &payload[1], PROFILE_NAME_LEN);
            name[PROFILE_NAME_LEN - 1] = '\0';
            if (profile_write_slot(idx, name, &payload[1 + PROFILE_NAME_LEN])) {
                status = CFG_ST_OK;
            } else {
                status = CFG_ST_ERR_INVALID;
            }
        }
        send_response(cmd, seq, &status, 1, sink);
        break;
    }
    case CFG_CMD_POWER_GET: {
        // 应答 = mv(2B LE) + pct(1B) + flags(1B) 共 4B
        power_status_t pw;
        power_mgr_get(&pw);
        uint8_t resp[4] = {
            (uint8_t)(pw.mv & 0xFF),
            (uint8_t)(pw.mv >> 8),
            pw.pct,
            (uint8_t)((pw.charging ? 0x01 : 0) | (pw.full ? 0x02 : 0) | (pw.usb_present ? 0x04 : 0)),
        };
        send_response(cmd, seq, resp, sizeof(resp), sink);
        break;
    }
    case CFG_CMD_MACRO_GET: {
        // payload=[idx]；应答 = idx(1B) + macro_t(137B) 共 138B
        uint8_t status = CFG_ST_ERR_LEN;
        if (len == 1) {
            macro_t *m = profile_get_macro(payload[0]);
            if (m == NULL) {
                status = CFG_ST_ERR_INVALID;
                send_response(cmd, seq, &status, 1, sink);
            } else {
                uint8_t resp[1 + sizeof(macro_t)];
                resp[0] = payload[0];
                memcpy(&resp[1], m, sizeof(macro_t));
                send_response(cmd, seq, resp, sizeof(resp), sink);
            }
            return;
        }
        send_response(cmd, seq, &status, 1, sink);
        break;
    }
    case CFG_CMD_MACRO_SET: {
        // payload=[idx(1B) + macro_t(137B)]；仅写 RAM，靠 0x05 落 NVS
        uint8_t status = CFG_ST_ERR_LEN;
        if (len == 1 + sizeof(macro_t)) {
            macro_t m;
            memcpy(&m, &payload[1], sizeof(macro_t));
            // 名称强制以 \0 结尾（固件侧防御截断）
            m.name[MACRO_NAME_LEN - 1] = '\0';
            if (profile_set_macro(payload[0], &m)) {
                status = CFG_ST_OK;
            } else {
                status = CFG_ST_ERR_INVALID;
            }
        }
        send_response(cmd, seq, &status, 1, sink);
        break;
    }
    case CFG_CMD_KNOB_GET: {
        // 应答 = cw(1B) + ccw(1B) + press(1B) 共 3B（v1.9：新增短按功能码）
        uint8_t cw, ccw, press;
        profile_get_knob(&cw, &ccw, &press);
        uint8_t resp[3] = {cw, ccw, press};
        send_response(cmd, seq, resp, sizeof(resp), sink);
        break;
    }
    case CFG_CMD_KNOB_SET: {
        // payload=[cw(1B)+ccw(1B)+press(1B)] 3B（v1.9）；兼容旧 2B（短按保持不变）；仅写 RAM，靠 0x05 落 NVS
        uint8_t status = CFG_ST_ERR_LEN;
        if (len == 3) {
            if (profile_set_knob(payload[0], payload[1], payload[2])) {
                status = CFG_ST_OK;
            } else {
                status = CFG_ST_ERR_INVALID;
            }
        } else if (len == 2) {
            // 旧版上位机（仅顺/逆时针）：短按配置保持原值
            uint8_t cw, ccw, press;
            profile_get_knob(&cw, &ccw, &press);
            if (profile_set_knob(payload[0], payload[1], press)) {
                status = CFG_ST_OK;
            } else {
                status = CFG_ST_ERR_INVALID;
            }
        }
        send_response(cmd, seq, &status, 1, sink);
        break;
    }
    case CFG_CMD_MACRO_REC: {
        // payload=[slot(1B)+action(1B)]：action 0=开始录制 1=停止录制（v2.2）
        // 录制状态为 RAM 标志：键盘侧只负责抑制 HID 输出并保留 [EVT] KEY 行，
        // 步骤收集与时间戳由上位机完成；slot 仅用于协议自描述，不落盘
        uint8_t status = CFG_ST_ERR_LEN;
        if (len == 2) {
            uint8_t action = payload[1];
            if (payload[0] >= MACRO_MAX || action > 1) {
                status = CFG_ST_ERR_INVALID;
            } else if (action == 0) {
                status = macro_rec_start() ? CFG_ST_OK : CFG_ST_ERR_INVALID;
            } else {
                macro_rec_stop();   // 未在录制时停止也返回 OK（幂等）
                status = CFG_ST_OK;
            }
        }
        send_response(cmd, seq, &status, 1, sink);
        break;
    }
    case CFG_CMD_CAL_GET: {
        // 应答 = lo_meas(2B LE)+lo_true(2B)+hi_meas(2B)+hi_true(2B) 共 8B（true=0 未校准）
        power_cal_point_t lo, hi;
        power_mgr_cal_get(&lo, &hi);
        uint8_t resp[8] = {
            (uint8_t)(lo.meas_mv & 0xFF), (uint8_t)(lo.meas_mv >> 8),
            (uint8_t)(lo.true_mv & 0xFF), (uint8_t)(lo.true_mv >> 8),
            (uint8_t)(hi.meas_mv & 0xFF), (uint8_t)(hi.meas_mv >> 8),
            (uint8_t)(hi.true_mv & 0xFF), (uint8_t)(hi.true_mv >> 8),
        };
        send_response(cmd, seq, resp, sizeof(resp), sink);
        break;
    }
    case CFG_CMD_CAL_SET: {
        // payload 同 0x0F 应答 8B：写两点校准（立即生效 + NVS）
        uint8_t status = CFG_ST_ERR_LEN;
        if (len == 8) {
            uint16_t lm = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            uint16_t lt = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
            uint16_t hm = (uint16_t)payload[4] | ((uint16_t)payload[5] << 8);
            uint16_t ht = (uint16_t)payload[6] | ((uint16_t)payload[7] << 8);
            status = power_mgr_cal_set(lm, lt, hm, ht) ? CFG_ST_OK : CFG_ST_ERR_INVALID;
        }
        send_response(cmd, seq, &status, 1, sink);
        break;
    }
    case CFG_CMD_SLEEP_GET: {
        // 应答 = light_min(2B LE) + deep_hr(2B LE) 共 4B
        uint16_t lm = sleep_mgr_get_light_min();
        uint16_t dh = sleep_mgr_get_deep_hr();
        uint8_t resp[4] = {
            (uint8_t)(lm & 0xFF), (uint8_t)(lm >> 8),
            (uint8_t)(dh & 0xFF), (uint8_t)(dh >> 8),
        };
        send_response(cmd, seq, resp, sizeof(resp), sink);
        break;
    }
    case CFG_CMD_SLEEP_SET: {
        // payload=[light_min(2B LE)+deep_hr(2B LE)]：写休眠参数（立即生效 + NVS）
        uint8_t status = CFG_ST_ERR_LEN;
        if (len == 4) {
            uint16_t lm = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
            uint16_t dh = (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
            status = sleep_mgr_set_params(lm, dh) ? CFG_ST_OK : CFG_ST_ERR_INVALID;
        }
        send_response(cmd, seq, &status, 1, sink);
        break;
    }
    case CFG_CMD_BLE_SLOT_GET: {
        // 应答 = current_slot(1B) + 3×[paired(1B)+peer_addr(6B)+addr_type(1B)+name(32B)] 共 121B
        // peer_addr 按槽位信息原样输出（NimBLE 地址字节序）；name 为 UTF-8 定长 32B，\0 结尾
        uint8_t resp[1 + BLE_MAX_SLOTS * 40] = {0};
        resp[0] = ble_hid_get_current_slot();
        for (int i = 0; i < BLE_MAX_SLOTS; i++) {
            ble_slot_info_t info;
            if (ble_hid_get_slot_info((uint8_t)i, &info) == ESP_OK) {
                uint16_t off = 1 + (uint16_t)i * 40;
                resp[off] = info.paired ? 1 : 0;
                memcpy(&resp[off + 1], info.peer_addr, 6);
                resp[off + 7] = info.addr_type;
                memcpy(&resp[off + 8], info.device_name, sizeof(info.device_name));
            }
        }
        send_response(cmd, seq, resp, sizeof(resp), sink);
        break;
    }
    case CFG_CMD_BLE_SLOT_CLEAR: {
        // payload=[slot(1B)]：0~2=单槽，0xFF=全部 3 槽；已配对槽位删 NimBLE 绑定（ble_hid 内部处理）
        uint8_t status = CFG_ST_ERR_LEN;
        if (len == 1) {
            if (payload[0] == 0xFF) {
                for (int i = 0; i < BLE_MAX_SLOTS; i++) {
                    ble_hid_clear_slot((uint8_t)i);
                }
                status = CFG_ST_OK;
            } else if (payload[0] < BLE_MAX_SLOTS) {
                status = (ble_hid_clear_slot(payload[0]) == ESP_OK) ? CFG_ST_OK : CFG_ST_ERR_INVALID;
            } else {
                status = CFG_ST_ERR_INVALID;
            }
        }
        send_response(cmd, seq, &status, 1, sink);
        break;
    }
    default:
        // 未知命令字：仍按规范应答 CMD|0x80
        ESP_LOGW(TAG, "Unknown cmd 0x%02X", cmd);
        uint8_t status = CFG_ST_ERR_CMD;
        send_response(cmd, seq, &status, 1, sink);
        break;
    }
}

// ==================== 传输解析 ====================

bool cfg_frame_feed(cfg_frame_parser_t *p, uint8_t b, cfg_transport_t transport,
                    cfg_frame_send_fn sink, void (*on_frame_start)(void)) {
    uint8_t *state = PARSER_STATE(p);

    switch (*state) {
    case ST_IDLE:
        if (b == 0xAA) {
            *state = ST_HDR1;
            return true;    // 0xAA 不属于 ASCII 文本，暂扣待定
        }
        return false;       // 文本字节

    case ST_HDR1:
        if (b == 0x55) {
            // 帧头确认：丢弃该传输未完成的文本行（仅串口有；BLE 无文本概念）
            if (on_frame_start != NULL) {
                on_frame_start();
            }
            *state = ST_COLLECT;
            p->pos = 0;
            p->total = 0;
            return true;
        }
        // 0xAA 0xAA：停留在 HDR1 继续等 0x55；其它字节丢弃
        if (b != 0xAA) {
            *state = ST_IDLE;
        }
        return true;

    case ST_COLLECT:
        if (p->pos >= CFG_FRAME_BUF_SIZE) {
            // 异常保护：超长帧直接丢弃复位
            ESP_LOGW(TAG, "Frame overflow, reset parser");
            *state = ST_IDLE;
            return true;
        }
        p->buf[p->pos++] = b;

        if (p->pos == CFG_FIXED_LEN) {
            // 收齐 CMD+SEQ+LEN：计算帧总长
            uint16_t payload_len = (uint16_t)p->buf[3] | ((uint16_t)p->buf[4] << 8);
            if (payload_len > CFG_FRAME_MAX_PAYLOAD) {
                ESP_LOGW(TAG, "Payload too large: %u", payload_len);
                *state = ST_IDLE;
                return true;
            }
            p->total = CFG_FIXED_LEN + payload_len + 2;
        }

        if (p->total > 0 && p->pos >= p->total) {
            // 整帧收齐：CRC 校验（CMD 起至 PAYLOAD 末，即 buf[0..total-3]）
            *state = ST_IDLE;
            uint8_t cmd = p->buf[0];
            uint16_t seq = (uint16_t)p->buf[1] | ((uint16_t)p->buf[2] << 8);
            uint16_t payload_len = (uint16_t)p->buf[3] | ((uint16_t)p->buf[4] << 8);
            const uint8_t *payload = &p->buf[CFG_FIXED_LEN];

            uint16_t crc_calc = crc16_ccitt(p->buf, p->total - 2);
            uint16_t crc_recv = (uint16_t)p->buf[p->total - 2] |
                                ((uint16_t)p->buf[p->total - 1] << 8);
            if (crc_calc != crc_recv) {
                ESP_LOGW(TAG, "CRC mismatch (calc=0x%04X recv=0x%04X)", crc_calc, crc_recv);
                uint8_t status = CFG_ST_ERR_CRC;
                send_response(cmd, seq, &status, 1, sink);
                return true;
            }

            // 配置帧活动刷新（P7，CRC 通过的帧才算有效活动）；
            // 0x09 电量轮询除外：那是上位机连接期间的 5s 自动轮询，不是用户活动——
            // 若刷新则上位机 BLE 在线期间闲置计时每 5s 清零一次，键盘永不睡眠
            // （2026-10-11 实测：BLE 连接 + 上位机在线改 1min 阈值也不进入 light sleep）
            if (cmd != CFG_CMD_POWER_GET) {
                sleep_mgr_activity();
            }

            // SEQ 滑动窗口去重（±16）
            if (cmd == CFG_CMD_PING) {
                // PING 握手：无条件接受并重同步
                process_frame(cmd, seq, payload, payload_len, transport, sink);
            } else if (!seq_valid) {
                // 首个非 PING 帧：接受并以此为基准（兼容未握手的简易上位机）
                last_seq = seq;
                seq_valid = true;
                process_frame(cmd, seq, payload, payload_len, transport, sink);
            } else {
                int16_t diff = (int16_t)(seq - last_seq);
                if (diff >= 1 && diff <= 16) {
                    last_seq = seq;
                    process_frame(cmd, seq, payload, payload_len, transport, sink);
                } else if (diff <= 0 && diff >= -16) {
                    // 重复帧（重传/应答丢失）：按 SEQ 查找其应答，从原传输原样重发，不重复执行
                    ESP_LOGD(TAG, "Duplicate seq=%u, replay its own response", seq);
                    resp_cache_entry_t *e = NULL;
                    for (int i = 0; i < RESP_CACHE_SIZE; i++) {
                        if (resp_cache[i].valid && resp_cache[i].seq == seq) {
                            e = &resp_cache[i];
                            break;
                        }
                    }
                    if (e != NULL && e->sink != NULL) {
                        e->sink(e->frame, e->len);
                    }
                } else {
                    // 超出窗口：拒绝执行
                    ESP_LOGW(TAG, "Seq out of window: seq=%u last=%u", seq, last_seq);
                    uint8_t status = CFG_ST_ERR_SEQ;
                    send_response(cmd, seq, &status, 1, sink);
                }
            }
        }
        return true;
    }

    return true;
}

// ==================== 串口传输封装 ====================

static cfg_frame_parser_t serial_parser;

// 串口应答：直接写 stdout（与文本日志同流，一次性写出并立刻 flush）
static void serial_cfg_send(const uint8_t *frame, uint16_t len) {
    fwrite(frame, 1, len, stdout);
    fflush(stdout);
}

bool cfg_frame_feed_byte(uint8_t b) {
    return cfg_frame_feed(&serial_parser, b, CFG_TRANSPORT_SERIAL,
                          serial_cfg_send, serial_cmd_discard_line);
}

void cfg_frame_parser_init(cfg_frame_parser_t *p) {
    memset(p, 0, sizeof(*p));
    // state=0 即 ST_IDLE；注意：全局 SEQ 窗口不在此重置（重同步只由 PING 触发），
    // 否则初始化第二条传输的解析器会把第一条的窗口连带清掉
}
