/**
 * @file serial_cmd.h
 * @brief ESP32-S3 三模键盘 - 串口命令
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-18
 *
 * 串口命令协议，详?docs/串口命令协议.md
 */

#ifndef SERIAL_CMD_H
#define SERIAL_CMD_H

#include <stdint.h>
#include <stdbool.h>

// ==================== 函数声明 ====================

/**
 * @brief 初始化串口命令模? *
 * 配置 UART，创建命令处理任? */
void serial_cmd_init(void);

/**
 * @brief 丢弃当前未完成的文本行缓? *
 * ?cfg_frame 在检测到二进制帧头时调用（帧与文本共存于同一流，
 * 帧到达时半行文本必然是不完整输入，直接丢弃防止混入下一行）
 */
void serial_cmd_discard_line(void);

/**
 * @brief 打印提示? */
void serial_cmd_print_prompt(void);

/**
 * @brief 串口任务入口函数
 *
 * 读取串口输入，解析命令并执行
 */
void serial_task(void *arg);

#endif // SERIAL_CMD_H
