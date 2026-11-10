/**
 * @file net_input.c
 * @brief ESP32-S3 三模键盘 - TCP 输入/配置通路（P5 方案 A，v1.8
 * @author Regression
 * @version 1.0.0
 * @date 2026-09-21
 *
 * 单客户端 TCP 服务端：
 * - 下行字节流逐字节喂 cfg_frame（CFG_TRANSPORT_NET），非帧字节静默丢弃
 *   （TCP 无串口文本命令概念，只承载二进制帧协议）
 * - 应答帧与 [EVT] 上行共用发送锁（recv 线程应答 / 其它任务推事件并发）
 * - TCP_NODELAY 关闭 Nagle 合并，保证按键事件低延迟
 * - 客户端断开后解析器复位，回?accept 等待新连? */

#include "net_input.h"
#include "cfg_frame.h"
#include <string.h>
#include <stdio.h>
#include "lwip/sockets.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "NET";

#define NET_TASK_STACK      4608
#define NET_TASK_PRIORITY   5
#define NET_RX_BUF_SIZE     256

static volatile bool s_running = false;
static volatile int s_listen_sock = -1;
static volatile int s_client_sock = -1;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_send_mtx = NULL;

// NET 传输独立解析器（与串?BLE 各一份，全局 SEQ 窗口共享
static cfg_frame_parser_t s_net_parser;

// ==================== 发?====================

// 应答帧发送回调（cfg_frame 原路返回
static void net_cfg_send(const uint8_t *frame, uint16_t len) {
    int sock = s_client_sock;
    if (sock < 0) {
        return;
    }
    xSemaphoreTake(s_send_mtx, portMAX_DELAY);
    int err = send(sock, frame, len, 0);
    xSemaphoreGive(s_send_mtx);
    if (err < 0) {
        ESP_LOGW(TAG, "send frame failed: errno=%d", errno);
    }
}

bool net_input_send_line(const char *line) {
    if (line == NULL) {
        return false;
    }
    int sock = s_client_sock;
    if (sock < 0) {
        return false;
    }

    char buf[NET_INPUT_LINE_MAX];
    int n = snprintf(buf, sizeof(buf), "%s\r\n", line);
    if (n <= 0 || n >= (int)sizeof(buf)) {
        return false;
    }

    xSemaphoreTake(s_send_mtx, portMAX_DELAY);
    int err = send(sock, buf, (size_t)n, 0);
    xSemaphoreGive(s_send_mtx);
    if (err < 0) {
        ESP_LOGW(TAG, "send line failed: errno=%d", errno);
        return false;
    }
    return true;
}

bool net_input_has_client(void) {
    return s_client_sock >= 0;
}

// ==================== 服务任务 ====================

static void client_session(int sock) {
    // ?Nagle：按键事件行必须立刻发出
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    s_client_sock = sock;
    ESP_LOGI(TAG, "Client connected");

    // 连接即复位解析器（新字节流；全局 SEQ 窗口?PING 重同步）
    cfg_frame_parser_init(&s_net_parser);

    uint8_t rx[NET_RX_BUF_SIZE];
    while (s_running) {
        int len = recv(sock, rx, sizeof(rx), 0);
        if (len <= 0) {
            if (len < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        for (int i = 0; i < len; i++) {
            // 非帧字节直接吞掉（TCP 通路无文本行解析
            cfg_frame_feed(&s_net_parser, rx[i], CFG_TRANSPORT_NET, net_cfg_send, NULL);
        }
    }

    s_client_sock = -1;
    close(sock);
    ESP_LOGI(TAG, "Client disconnected");
}

static void net_task(void *arg) {
    // 监听套接
    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(NET_INPUT_PORT);

    if (bind(s_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind :%d failed: errno=%d", NET_INPUT_PORT, errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        vTaskDelete(NULL);
        return;
    }
    if (listen(s_listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "listen failed: errno=%d", errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP server listening on :%d", NET_INPUT_PORT);

    while (s_running) {
        struct sockaddr_in cli = {0};
        socklen_t cli_len = sizeof(cli);
        int sock = accept(s_listen_sock, (struct sockaddr *)&cli, &cli_len);
        if (sock < 0) {
            if (!s_running) {
                break;  // stop() 关闭监听套接字导?accept 失败，正常退?
            }
            ESP_LOGW(TAG, "accept failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        client_session(sock);
    }

    vTaskDelete(NULL);
}

// ==================== 公开接口 ====================

void net_input_start(void) {
    if (s_running) {
        return;
    }
    if (s_send_mtx == NULL) {
        s_send_mtx = xSemaphoreCreateMutex();
    }
    s_running = true;
    xTaskCreate(net_task, "net_input", NET_TASK_STACK, NULL, NET_TASK_PRIORITY, &s_task);
    ESP_LOGD(TAG, "net_input started");
}

void net_input_stop(void) {
    if (!s_running) {
        return;
    }
    s_running = false;

    // 关闭监听与客户端套接字，解除 accept/recv 阻塞
    int listen_sock = s_listen_sock;
    if (listen_sock >= 0) {
        shutdown(listen_sock, 0);
        close(listen_sock);
        s_listen_sock = -1;
    }
    int client = s_client_sock;
    if (client >= 0) {
        shutdown(client, 0);
        close(client);
        s_client_sock = -1;
    }

    // 等任务自行退
    if (s_task != NULL) {
        for (int i = 0; i < 20 && eTaskGetState(s_task) != eDeleted; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        s_task = NULL;
    }
    ESP_LOGD(TAG, "net_input stopped");
}
