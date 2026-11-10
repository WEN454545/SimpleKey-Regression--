/**
 * @file cfg_frame.h
 * @brief ESP32-S3 三模键盘 - AA 55 二进制配置帧协议（多传输：串口/BLE/WiFi TCP）
 * @author Regression
 * @version 1.1.0
 * @date 2026-09-20
 *
 * 配置协议帧（与文本命令共存于同一流）：
 *   AA 55 | CMD(1B) | SEQ(2B LE) | LEN(2B LE) | PAYLOAD(LEN B) | CRC16(2B LE)
 * - CRC16：CRC-16/CCITT-FALSE（poly 0x1021，初值 0xFFFF，不反转），
 *   计算范围 CMD 起至 PAYLOAD 末（不含帧头与 CRC 本身）
 * - SEQ：命令方向（上位机→键盘）单调递增（模 65536），接收端滑动窗口 ±16 去重；
 *   PING 握手时重同步（last_seq 直接对齐到当前帧 SEQ）
 * - 应答帧 CMD|0x80 并回显请求 SEQ；**应答从帧到达的同一传输原路返回**
 * - 传输通路：串口 CDC（与文本行共存）/ BLE GATT 0xFFE1（Write 下行、Notify 上行）/ WiFi TCP :8266（P5 方案 A）
 * - 详见 docs/串口命令协议.md v1.5 §7
 */

#ifndef CFG_FRAME_H
#define CFG_FRAME_H

#include <stdint.h>
#include <stdbool.h>

// ==================== 帧参数 ====================

// 128 → 160（P6）：MACRO_GET/SET 应答/请求 payload = idx(1B) + macro_t(137B) = 138B
#define CFG_FRAME_MAX_PAYLOAD   160

// 整帧最大长度：帧头2 + CMD1 + SEQ2 + LEN2 + PAYLOAD + CRC2
#define CFG_FRAME_BUF_SIZE      (2 + 5 + CFG_FRAME_MAX_PAYLOAD + 2)

// ==================== 命令字（上位机 -> 键盘） ====================

#define CFG_CMD_PING            0x01    // 握手/重同步，应答 payload=[协议版本]
#define CFG_CMD_VERSION         0x02    // 读固件版本，应答 payload=版本字符串
#define CFG_CMD_KEYMAP_GET      0x03    // 读键位表，应答 payload=20B（行优先 5x4）
#define CFG_CMD_KEYMAP_SET      0x04    // 写单键，payload=[row,col,keycode]，应答 payload=[status]
#define CFG_CMD_KEYMAP_SAVE     0x05    // 保存当前 Profile 全部配置（键位+旋钮+宏）到 NVS，应答 payload=[status]（P6 语义扩展）
#define CFG_CMD_PROFILE_GET     0x06    // 读 Profile，payload=[index]，应答=[name(16B)+keymap(20B)+default_mode(1B)] 共 37B
#define CFG_CMD_PROFILE_SET     0x07    // 切换当前 Profile，payload=[index]，应答=[status]
#define CFG_CMD_PROFILE_WRITE   0x08    // 写 Profile（名称+键位），payload=[index+name(16B)+keymap(20B)]，应答=[status]（宏/旋钮不动）
#define CFG_CMD_POWER_GET       0x09    // 读电源状态，应答=[mv(2B LE)+pct(1B)+flags(1B)] 共 4B（flags bit0=充电中 bit1=已充满 bit2=USB在位）
#define CFG_CMD_MACRO_GET       0x0A    // 读宏（P6），payload=[idx 0-3]，应答=[idx(1B)+macro_t(137B)] 共 138B
#define CFG_CMD_MACRO_SET       0x0B    // 写宏（P6），payload=[idx(1B)+macro_t(137B)]，应答=[status]；仅写 RAM，靠 0x05 落 NVS
#define CFG_CMD_KNOB_GET        0x0C    // 读旋钮输出配置（P6），应答=[cw(1B)+ccw(1B)+press(1B)]（cw/ccw=0xC0~0xCF；press=0xFE 切 Profile/0xC0~0xCF/0xF0~0xF3，v1.9）
#define CFG_CMD_KNOB_SET        0x0D    // 写旋钮输出配置（P6），payload=[cw(1B)+ccw(1B)+press(1B)]（兼容旧 2B：短按不变），应答=[status]；仅写 RAM，靠 0x05 落 NVS
#define CFG_CMD_MACRO_REC       0x0E    // 宏录制控制（v2.2），payload=[slot(1B)+action(1B)]（action：0=开始 1=停止），应答=[status]；录制期间键盘抑制 HID 输出，仅发 [EVT] KEY 行，RAM 标志不持久化
#define CFG_CMD_CAL_GET         0x0F    // 读两点校准（P7），应答=[lo_meas(2B LE)+lo_true(2B)+hi_meas(2B)+hi_true(2B)] 共 8B（true=0 表示未校准）
#define CFG_CMD_CAL_SET         0x10    // 写两点校准（P7），payload 同 0x0F 应答 8B，应答=[status]；立即生效并写 NVS
#define CFG_CMD_SLEEP_GET       0x11    // 读休眠参数（P7），应答=[light_min(2B LE)+deep_hr(2B LE)] 共 4B
#define CFG_CMD_SLEEP_SET       0x12    // 写休眠参数（P7），payload=[light_min(2B LE)+deep_hr(2B LE)]，应答=[status]；立即生效并写 NVS
#define CFG_CMD_BLE_SLOT_GET    0x13    // 读 BLE 配对槽位列表，应答=[current_slot(1B)] + 3×[paired(1B)+peer_addr(6B)+addr_type(1B)+name(32B)] 共 121B
#define CFG_CMD_BLE_SLOT_CLEAR  0x14    // 清除槽位配对，payload=[slot(1B)]（0~2=单槽，0xFF=全部 3 槽），应答=[status]；已配对槽位同步删除 NimBLE 绑定

// ==================== 应答状态码（payload[0]，适用于 SET/SAVE/错误应答） ====================

#define CFG_ST_OK               0x00    // 成功
#define CFG_ST_ERR_INVALID      0x01    // 参数无效（row/col 越界等）
#define CFG_ST_ERR_SEQ          0x02    // SEQ 超出滑动窗口（未重同步）
#define CFG_ST_ERR_CRC          0x03    // CRC 校验失败
#define CFG_ST_ERR_LEN          0x04    // PAYLOAD 长度不符合命令要求
#define CFG_ST_ERR_CMD          0x05    // 未知命令字

// ==================== 传输通路 ====================

typedef enum {
    CFG_TRANSPORT_SERIAL = 0,   // USB CDC（与文本行共存）
    CFG_TRANSPORT_BLE = 1,      // BLE GATT 0xFFE1（Write 下行 / Notify 上行）
    CFG_TRANSPORT_NET = 2,      // WiFi TCP :8266（P5 方案 A，与串口/BLE 互斥激活）
} cfg_transport_t;

// 应答帧发送回调：把一帧完整报文从指定传输发出
typedef void (*cfg_frame_send_fn)(const uint8_t *frame, uint16_t len);

// 每传输一个独立解析状态（两条字节流互不干扰）
typedef struct {
    uint8_t state;                              // 内部状态机
    uint8_t buf[CFG_FRAME_BUF_SIZE];            // 收帧缓冲
    uint16_t pos;                               // 已收字节数
    uint16_t total;                             // 本帧应收总字节数
} cfg_frame_parser_t;

// ==================== 函数声明 ====================

/**
 * @brief 初始化一个帧解析器（复位状态机；SEQ 窗口为全局共享，不受影响）
 */
void cfg_frame_parser_init(cfg_frame_parser_t *p);

/**
 * @brief 向指定传输的帧解析器喂一个字节
 *
 * @param p              该传输的解析器状态
 * @param b              收到的原始字节
 * @param transport      传输标识（用于 SEQ 窗口日志与应答缓存归属）
 * @param sink           应答发送回调（应答原路返回）
 * @param on_frame_start 帧头确认回调（可为 NULL）。串口传输用它丢弃半行文本；
 *                       BLE 传输无文本行概念，传 NULL
 * @return true  字节被帧解析器消费（属于二进制帧或已丢弃）
 * @return false 字节不属于帧，应交给文本行解析（仅串口传输会出现）
 */
bool cfg_frame_feed(cfg_frame_parser_t *p, uint8_t b, cfg_transport_t transport,
                    cfg_frame_send_fn sink, void (*on_frame_start)(void));

/**
 * @brief 串口传输便捷封装（内部持有串口解析器实例，应答走 stdout）
 *
 * 字节流规则：帧头 0xAA 后必须跟 0x55，否则 0xAA 被丢弃；检测到帧头时
 * 丢弃当前未完成的文本行。仅串口任务调用。
 */
bool cfg_frame_feed_byte(uint8_t b);

#endif // CFG_FRAME_H
