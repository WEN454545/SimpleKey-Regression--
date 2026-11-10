/**
 * @file net_input.h
 * @brief ESP32-S3 三模键盘 - TCP 输入/配置通路（P5 方案 A，v1.8
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-21
 *
 * WiFi 模式下的数据通路（与串口 CDC / BLE GATT 并列的第三条）：
 * - 下行：上位机 TCP 客户??:8266 ?cfg_frame 帧解析（CFG_TRANSPORT_NET），
 *   应答帧原路（同一 TCP 连接）返
 * - 上行：键?[EVT] 文本??net_input_send_line() ?TCP 推送给上位机，
 *   上位?SendInput 注入器据此维?6KRO 快照注入输入
 * - 单客户端：同一时刻只接受一个连接，新连接前须等旧连接断开
 */

#ifndef NET_INPUT_H
#define NET_INPUT_H

#include <stdbool.h>
#include <stdint.h>

// ==================== 常量 ====================

#define NET_INPUT_PORT          8266    // TCP 服务端口（方?§12.8
#define NET_INPUT_LINE_MAX      160     // 上行单行最大字节数（含 \r\n?
// ==================== 函数声明 ====================

/**
 * @brief 启动 TCP 服务任务（监?0.0.0.0:8266，accept 循环? *
 * 幂等；可?WiFi AP 启动前调用（无客户端?accept 一直阻塞）
 */
void net_input_start(void);

/**
 * @brief 停止 TCP 服务任务并关闭所有套接字
 */
void net_input_stop(void);

/**
 * @brief 是否有上位机客户端已连接
 */
bool net_input_has_client(void);

/**
 * @brief 向已连接的客户端推送一行文本（自动追加 \r\n? *
 * @param line 内容行（?"[EVT] KEY DOWN 0 1 0x1E 1"），不带换行
 * @return true 已发送；false 无客户端或发送失? */
bool net_input_send_line(const char *line);

#endif // NET_INPUT_H
