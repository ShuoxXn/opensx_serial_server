/*
 * @Author: Orion
 * @Date: 2024-01-23 16:08:36
 * @LastEditors: Orion
 * @LastEditTime: 2025-06-11 19:07:21
 * @FilePath: \SERIIAL_SERVER\main\sx_tcp_server.c
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include "sx_async_uart.h"
#include "sx_utils.h"
#include "sx_work_mode.h"
#include <lwip/netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>  // For close() function

#define KEEPALIVE_IDLE 5
#define KEEPALIVE_INTERVAL 5
#define KEEPALIVE_COUNT 3
int global_sock = -1;

int tcp_client_count = 0;

static const char *TAG = "TCPSERVER";
// 添加互斥锁
static SemaphoreHandle_t tcp_send_mutex = NULL;
// 心跳包任务句柄
TaskHandle_t xHandleTaskTCPServerHeartbeat = NULL;
static TaskHandle_t xHandleTaskTCPServer = NULL;
static volatile bool s_tcp_server_stop_requested = false;
static int s_tcp_server_listen_sock = -1;

// Socket清理队列
#define CLEANUP_QUEUE_SIZE 10
static QueueHandle_t socket_cleanup_queue = NULL;
static TaskHandle_t socket_cleanup_task_handle = NULL;

typedef struct {
    int socket_fd;
} cleanup_item_t;

// Socket清理任务
static void socket_cleanup_task(void *pvParameters) {
    cleanup_item_t cleanup_item;

    while (1) {
        // 等待需要清理的socket
        if (xQueueReceive(socket_cleanup_queue, &cleanup_item, portMAX_DELAY) == pdTRUE) {
            if (cleanup_item.socket_fd >= 0) {
                ESP_LOGI(TAG, "Cleaning up socket %d", cleanup_item.socket_fd);
                shutdown(cleanup_item.socket_fd, SHUT_RDWR);
                vTaskDelay(pdMS_TO_TICKS(50)); // 给TCP栈时间处理
                close(cleanup_item.socket_fd);
                ESP_LOGI(TAG, "Socket %d cleaned up", cleanup_item.socket_fd);
            }
        }
    }
}

// 安全关闭socket的函数
static void safe_close_socket(int socket_fd) {
    if (socket_fd < 0) {
        return;
    }

    // 修复：验证socket是否仍然有效，避免双重关闭
    int flags = fcntl(socket_fd, F_GETFD);
    if (flags == -1 && errno == EBADF) {
        ESP_LOGW(TAG, "Socket %d 已经无效，跳过关闭", socket_fd);
        return;
    }

    if (socket_cleanup_queue == NULL) {
        // 如果队列不存在，直接关闭
        shutdown(socket_fd, SHUT_RDWR);
        vTaskDelay(pdMS_TO_TICKS(10));
        close(socket_fd);
        ESP_LOGD(TAG, "Socket %d 直接关闭", socket_fd);
        return;
    }

    cleanup_item_t cleanup_item = { .socket_fd = socket_fd };

    // 尝试将socket添加到清理队列
    if (xQueueSend(socket_cleanup_queue, &cleanup_item, 0) != pdTRUE) {
        // 如果队列满了，直接关闭
        ESP_LOGW(TAG, "Cleanup queue full, closing socket directly");
        shutdown(socket_fd, SHUT_RDWR);
        vTaskDelay(pdMS_TO_TICKS(10));
        close(socket_fd);
    }
}

#define MAX_CLIENTS 6  // 保留heap/socket给Web、MQTT和平台检测
#define TCP_CLIENT_TASK_STACK_SIZE 8192
#define TCP_SERVER_MIN_FREE_HEAP 16000

// 客户端连接结构体
typedef struct {
    int sock;
    TaskHandle_t task_handle;
    bool in_use;
} client_conn_t;

// 客户端连接数组
static client_conn_t clients[MAX_CLIENTS];
static SemaphoreHandle_t clients_mutex = NULL;

static bool ensure_tcp_server_runtime(void) {
    if (tcp_send_mutex == NULL) {
        tcp_send_mutex = xSemaphoreCreateMutex();
    }
    if (clients_mutex == NULL) {
        clients_mutex = xSemaphoreCreateMutex();
    }
    if (tcp_send_mutex == NULL || clients_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create TCP server mutexes");
        return false;
    }

    if (socket_cleanup_queue == NULL) {
        socket_cleanup_queue = xQueueCreate(CLEANUP_QUEUE_SIZE, sizeof(cleanup_item_t));
        if (socket_cleanup_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create socket cleanup queue");
            return false;
        }
    }

    if (socket_cleanup_task_handle == NULL) {
        BaseType_t ret = xTaskCreate(socket_cleanup_task, "socket_cleanup",
                                     2048, NULL, 5,
                                     &socket_cleanup_task_handle);
        if (ret != pdPASS) {
            socket_cleanup_task_handle = NULL;
            ESP_LOGE(TAG, "Failed to create socket cleanup task");
            return false;
        }
    }

    return true;
}

static bool tcp_server_can_accept_client(size_t *free_heap_out) {
    size_t free_heap = esp_get_free_heap_size();
    if (free_heap_out) {
        *free_heap_out = free_heap;
    }

    if (free_heap < TCP_SERVER_MIN_FREE_HEAP) {
        ESP_LOGW(TAG, "TCP Server拒绝新连接: free_heap=%u低于保留阈值%u",
                 (unsigned)free_heap, TCP_SERVER_MIN_FREE_HEAP);
        return false;
    }

    if (tcp_client_count >= MAX_CLIENTS) {
        ESP_LOGW(TAG, "TCP Server连接数已满: %d/%d",
                 tcp_client_count, MAX_CLIENTS);
        return false;
    }

    return true;
}

static void close_all_clients(void) {
    int sockets_to_close[MAX_CLIENTS];
    int close_count = 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        sockets_to_close[i] = -1;
    }

    if (clients_mutex && xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].in_use && clients[i].sock >= 0) {
                sockets_to_close[close_count++] = clients[i].sock;
                clients[i].sock = -1;
                clients[i].task_handle = NULL;
                clients[i].in_use = false;
            }
        }
        tcp_client_count = 0;
        xSemaphoreGive(clients_mutex);
    }

    for (int i = 0; i < close_count; i++) {
        safe_close_socket(sockets_to_close[i]);
    }
}

// 初始化客户端数组
static void init_clients(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].sock = -1;
        clients[i].task_handle = NULL;
        clients[i].in_use = false;
    }
}

// 添加客户端连接
static int add_client(int sock, TaskHandle_t handle) {
    if (xSemaphoreTake(clients_mutex, portMAX_DELAY)) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].in_use) {
                clients[i].sock = sock;
                clients[i].task_handle = handle;
                clients[i].in_use = true;
                tcp_client_count++;
                xSemaphoreGive(clients_mutex);
                return i;
            }
        }
        xSemaphoreGive(clients_mutex);
    }
    return -1;
}

// 移除客户端连接
static void remove_client(int index) {
    if (index < 0 || index >= MAX_CLIENTS) {
        return;
    }

    if (xSemaphoreTake(clients_mutex, portMAX_DELAY)) {
        if (clients[index].in_use) {
            int sock_to_close = -1;

            // 先标记为不可用，避免其他线程访问
            clients[index].in_use = false;
            sock_to_close = clients[index].sock;
            clients[index].sock = -1;
            clients[index].task_handle = NULL;
            tcp_client_count--;

            xSemaphoreGive(clients_mutex);

            // 使用安全关闭函数
            safe_close_socket(sock_to_close);
        } else {
            xSemaphoreGive(clients_mutex);
        }
    }
}

// 将十六进制字符串转换为二进制数据
static int hex_to_bin(const char *hex_str, uint8_t *bin_data, int max_len) {
    int hex_len = strlen(hex_str);
    int bin_len = 0;

    // 跳过可能的0x前缀
    if (hex_len >= 2 && hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
        hex_str += 2;
        hex_len -= 2;
    }

    // 确保长度是偶数
    if (hex_len % 2 != 0) {
        ESP_LOGE(TAG, "Hex string length must be even");
        return 0;
    }

    bin_len = hex_len / 2;
    if (bin_len > max_len) {
        ESP_LOGE(TAG, "Hex string too long");
        return 0;
    }

    for (int i = 0; i < bin_len; i++) {
        char high = hex_str[i * 2];
        char low = hex_str[i * 2 + 1];

        // 转换高位
        if (high >= '0' && high <= '9') {
            bin_data[i] = (high - '0') << 4;
        } else if (high >= 'A' && high <= 'F') {
            bin_data[i] = (high - 'A' + 10) << 4;
        } else if (high >= 'a' && high <= 'f') {
            bin_data[i] = (high - 'a' + 10) << 4;
        } else {
            ESP_LOGE(TAG, "Invalid hex character: %c", high);
            return 0;
        }

        // 转换低位
        if (low >= '0' && low <= '9') {
            bin_data[i] |= (low - '0');
        } else if (low >= 'A' && low <= 'F') {
            bin_data[i] |= (low - 'A' + 10);
        } else if (low >= 'a' && low <= 'f') {
            bin_data[i] |= (low - 'a' + 10);
        } else {
            ESP_LOGE(TAG, "Invalid hex character: %c", low);
            return 0;
        }
    }

    return bin_len;
}

// 获取MAC地址字符串（不带冒号）
static void get_mac_str(char *mac_str, size_t max_len) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_str, max_len, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ============================================================================
// 辅助函数：发送数据到单个客户端
// ============================================================================
static int send_to_client(int client_sock, const void *data, size_t len) {
    size_t total_sent = 0;

    while (total_sent < len) {
        int written = send(client_sock, (const char *)data + total_sent,
                          len - total_sent, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            ESP_LOGE(TAG, "Send failed: errno %d", errno);
            return -1;
        }
        total_sent += written;
    }
    return total_sent;
}

// ============================================================================
// 辅助函数：广播数据到所有客户端（透传模式）
// ============================================================================
static void broadcast_to_all_clients(const char *data, int len) {
    bool any_client_connected = false;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].in_use && clients[i].sock >= 0) {
            any_client_connected = true;

            if (send_to_client(clients[i].sock, data, len) < 0) {
                ESP_LOGE(TAG, "Failed to send to client %d, marking for removal", i);
                clients[i].in_use = false;
                int socket_to_close = clients[i].sock;
                clients[i].sock = -1;
                tcp_client_count--;

                // 释放锁后关闭socket
                xSemaphoreGive(clients_mutex);
                safe_close_socket(socket_to_close);
                if (xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
                    ESP_LOGE(TAG, "Failed to reacquire clients mutex");
                    return;
                }
            }
        }
    }

    if (!any_client_connected) {
        ESP_LOGW(TAG, "No TCP clients connected");
    }
}

// 心跳包任务
void tcp_server_heartbeat_task(void *pvParameters) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // 打开 NVS
    err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    // 获取心跳包配置
    size_t heart_packet_size = 0;
    size_t heart_format_size = 0;
    size_t heart_interval_size = 0;

    err = nvs_get_str(nvs_handle, "heart_packet", NULL, &heart_packet_size);
    if (err != ESP_OK || heart_packet_size == 0) {
        ESP_LOGI(TAG, "No heartbeat packet configured");
    }

    err = nvs_get_str(nvs_handle, "heart_format", NULL, &heart_format_size);
    if (err != ESP_OK || heart_format_size == 0) {
        ESP_LOGI(TAG, "No heartbeat format configured");
    }

    err = nvs_get_str(nvs_handle, "heart_interval", NULL, &heart_interval_size);
    if (err != ESP_OK || heart_interval_size == 0) {
        ESP_LOGI(TAG, "No heartbeat interval configured");
    }

    // 分配内存
    char *heart_packet = heart_packet_size > 0 ? malloc(heart_packet_size) : NULL;
    char *heart_format = heart_format_size > 0 ? malloc(heart_format_size) : NULL;
    char *heart_interval_str = heart_interval_size > 0 ? malloc(heart_interval_size) : NULL;
    bool heartbeat_disabled = false;

    // 读取配置
    if (heart_packet) {
        err = nvs_get_str(nvs_handle, "heart_packet", heart_packet, &heart_packet_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read heartbeat packet");
        }
    }

    if (heart_format) {
        err = nvs_get_str(nvs_handle, "heart_format", heart_format, &heart_format_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read heartbeat format");
        } else if (strcmp(heart_format, "none") == 0) {
            heartbeat_disabled = true;
        }
    }

    int heart_interval = 30; // 默认30秒
    if (heart_interval_str) {
        err = nvs_get_str(nvs_handle, "heart_interval", heart_interval_str, &heart_interval_size);
        if (err == ESP_OK) {
            heart_interval = atoi(heart_interval_str);
            if (heart_interval <= 0) {
                heart_interval = 30;
            }
        }
    }

    if (heartbeat_disabled) {
        ESP_LOGI(TAG, "Heartbeat disabled for TCP Server, stopping heartbeat task");
        if (heart_packet) {
            free(heart_packet);
            heart_packet = NULL;
        }
        if (heart_format) {
            free(heart_format);
            heart_format = NULL;
        }
        if (heart_interval_str) {
            free(heart_interval_str);
            heart_interval_str = NULL;
        }
        nvs_close(nvs_handle);
        xHandleTaskTCPServerHeartbeat = NULL;
        vTaskDelete(NULL);
        return;
    }

    // 只有在没有读取到heart_packet配置时才使用MAC地址作为默认值
    // 如果用户设置了空字符串，应该尊重用户选择，不发送心跳包
    if (heart_packet == NULL) {
        // 首次使用，没有配置，使用MAC地址作为默认值
        heart_packet = malloc(13);
        if (heart_packet) {
            get_mac_str(heart_packet, 13);
        }
    }

    // 默认ASCII格式
    if (heart_format == NULL) {
        heart_format = malloc(6);
        if (heart_format) {
            strcpy(heart_format, "ascii");
        }
    }

    nvs_close(nvs_handle);

    // 心跳包循环
    while (1) {
        // 获取互斥锁
        if (xSemaphoreTake(clients_mutex, portMAX_DELAY)) {
            // 遍历所有客户端连接
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].in_use && clients[i].sock >= 0) {
                    // 只有当heart_packet不为空字符串时才发送心跳包
                    if (heart_packet && heart_format && strlen(heart_packet) > 0) {
                        int send_result = -1;
                        if (strcmp(heart_format, "hex") == 0) {
                            // 十六进制格式
                            uint8_t bin_data[128] = {0};
                            int bin_len = hex_to_bin(heart_packet, bin_data, sizeof(bin_data));

                            if (bin_len > 0) {
                                ESP_LOGI(TAG, "Sending HEX heartbeat packet to client %d, length: %d", i, bin_len);
                                send_result = send(clients[i].sock, bin_data, bin_len, MSG_NOSIGNAL);
                            } else {
                                ESP_LOGE(TAG, "Failed to convert HEX heartbeat packet");
                            }
                        } else {
                            // ASCII格式
                            ESP_LOGI(TAG, "Sending ASCII heartbeat packet to client %d: %s", i, heart_packet);
                            send_result = send(clients[i].sock, heart_packet, strlen(heart_packet), MSG_NOSIGNAL);
                        }

                        // 检查发送结果
                        if (send_result < 0) {
                            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                ESP_LOGE(TAG, "Heartbeat send failed to client %d: errno %d", i, errno);
                                // 标记客户端为失效，但不在此处关闭socket
                                clients[i].in_use = false;
                                int socket_to_close = clients[i].sock;
                                clients[i].sock = -1;
                                tcp_client_count--;

                                // 在释放锁后安全关闭socket
                                xSemaphoreGive(clients_mutex);
                                safe_close_socket(socket_to_close);
                                if (xSemaphoreTake(clients_mutex, portMAX_DELAY) == pdFALSE) {
                                    break; // 如果无法重新获取锁，退出循环
                                }
                            }
                        }
                    }
                }
            }
            xSemaphoreGive(clients_mutex);
        }

        // 等待指定的间隔时间
        vTaskDelay(heart_interval * 1000 / portTICK_PERIOD_MS);
    }

    // 清理
    if (heart_packet) free(heart_packet);
    if (heart_format) free(heart_format);
    if (heart_interval_str) free(heart_interval_str);

    xHandleTaskTCPServerHeartbeat = NULL;
    vTaskDelete(NULL);
}

// 在文件开始初始化互斥锁
void init_tcp_server(void) {
    if (!ensure_tcp_server_runtime()) {
        return;
    }

    init_clients();

    if (xHandleTaskTCPServerHeartbeat == NULL) {
        xTaskCreate(tcp_server_heartbeat_task, "tcp_server_heartbeat", 4096, NULL, 5, &xHandleTaskTCPServerHeartbeat);
    }
}
// ============================================================================
// 主接收处理函数
// ============================================================================
static void do_retransmit(const int sock, int client_id) {
    int len;
    uint8_t rx_buffer[512];
    ESP_LOGI(TAG, "Client %d connected, TCP transparent forwarding enabled",
             client_id);

    do {
        len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
        if (len < 0) {
            ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
        } else if (len == 0) {
            ESP_LOGI(TAG, "Connection closed by client %d", client_id);
        } else {
            ESP_LOGI(TAG, "Received %d bytes from client %d", len, client_id);
            ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buffer, len, ESP_LOG_INFO);

            sx_work_mode_tcp_handler_t handler =
                sx_work_mode_get_tcp_handler();
            if (handler != NULL) {
                esp_err_t err = handler(rx_buffer, len);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "work mode TCP handler failed: %s",
                             esp_err_to_name(err));
                }
            } else {
                tx_task(rx_buffer, len);
            }
        }
    } while (len > 0);
}

// 客户端处理任务
static void handle_client_task(void *pvParameters) {
    int client_id = *((int *)pvParameters);
    free(pvParameters); // 释放参数内存

    int sock = -1;

    // 获取套接字
    if (xSemaphoreTake(clients_mutex, portMAX_DELAY)) {
        if (client_id >= 0 && client_id < MAX_CLIENTS && clients[client_id].in_use) {
            sock = clients[client_id].sock;
        }
        xSemaphoreGive(clients_mutex);
    }

    if (sock < 0) {
        ESP_LOGE(TAG, "Invalid socket in client task");
        remove_client(client_id);
        vTaskDelete(NULL);
        return;
    }

    do_retransmit(sock, client_id);

    // 移除客户端
    remove_client(client_id);
    vTaskDelete(NULL);
}

// ============================================================================
// 主发送函数：从串口接收数据后发送到TCP客户端
// ============================================================================
void tcpserver_message(char *data, int len) {
    static const char *TAG = "TCP_SERVER";
    // 检查互斥锁是否已初始化
    if (tcp_send_mutex == NULL) {
        ESP_LOGE(TAG, "TCP send mutex not initialized");
        return;
    }

    // 获取发送互斥锁
    if (xSemaphoreTake(tcp_send_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire tcp_send_mutex");
        return;
    }

    // 获取客户端互斥锁
    if (xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire clients_mutex");
        xSemaphoreGive(tcp_send_mutex);
        return;
    }

    // 透传模式：广播到所有客户端
    ESP_LOGI(TAG, "Broadcasting data to all clients (%d bytes)", len);
    broadcast_to_all_clients(data, len);

    xSemaphoreGive(clients_mutex);
    xSemaphoreGive(tcp_send_mutex);
}
static void tcp_server_task(void *pvParameters) {
  int addr_family = (int)pvParameters;
  int ip_protocol = 0;
  int keepAlive = 1;
  int keepIdle = KEEPALIVE_IDLE;
  int keepInterval = KEEPALIVE_INTERVAL;
  int keepCount = KEEPALIVE_COUNT;
  struct sockaddr_storage dest_addr;

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));

  size_t use_tcp, tcp_server, tcp_port;
  ret = nvs_get_str(nvs_handle, "use_tcp", NULL, &use_tcp);
  nvs_get_str(nvs_handle, "tcp_server", NULL, &tcp_server);
  nvs_get_str(nvs_handle, "tcp_port", NULL, &tcp_port);
  if (ret != ESP_OK) {
    printf("Error getting size of 'my_key': %s\n", esp_err_to_name(ret));
  } else {
    char *nvs_use_tcp = malloc(use_tcp);
    char *nvs_tcp_server = malloc(tcp_server);
    char *nvs_tcp_port = malloc(tcp_port);

    if (nvs_use_tcp == NULL || nvs_tcp_server == NULL || nvs_tcp_port == NULL) {
      printf("Memory allocation failed\n");
      free(nvs_use_tcp);
      free(nvs_tcp_server);
      free(nvs_tcp_port);
    } else {
      ret = nvs_get_str(nvs_handle, "use_tcp", nvs_use_tcp, &use_tcp);
      nvs_get_str(nvs_handle, "tcp_server", nvs_tcp_server, &tcp_server);
      nvs_get_str(nvs_handle, "tcp_port", nvs_tcp_port, &tcp_port);
      if (ret == ESP_OK) {
        if (addr_family == AF_INET) {
          struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
          dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
          dest_addr_ip4->sin_family = AF_INET;
          const char *port_str =
              (nvs_tcp_port && nvs_tcp_port[0] != '\0')
                  ? nvs_tcp_port
                  : "8888";
          int port_num = atoi(port_str);
          dest_addr_ip4->sin_port = htons(port_num);
          ip_protocol = IPPROTO_IP;
          ESP_LOGI(TAG, "Socket bound, port %d", port_num);
        }
      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
      free(nvs_use_tcp);
      free(nvs_tcp_server);
      free(nvs_tcp_port);
    }
  }
  nvs_close(nvs_handle);

  int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
  if (listen_sock < 0) {
    ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    xHandleTaskTCPServer = NULL;
    vTaskDelete(NULL);
    return;
  }
  s_tcp_server_listen_sock = listen_sock;

  int opt = 1;
  setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  if (addr_family == AF_INET6) {
    setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
  }

  ESP_LOGI(TAG, "Socket created");

  int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
  if (err != 0) {
    ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
    ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
    goto CLEAN_UP;
  }

  err = listen(listen_sock, MAX_CLIENTS);
  if (err != 0) {
    ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
    goto CLEAN_UP;
  }

      while (!s_tcp_server_stop_requested) {
        ESP_LOGI(TAG, "Socket listening");
        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            if (s_tcp_server_stop_requested) {
                ESP_LOGI(TAG, "TCP Server停止请求已收到，退出accept循环");
                break;
            }
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            continue;
        }

        size_t free_heap = 0;
        if (!tcp_server_can_accept_client(&free_heap)) {
            ESP_LOGE(TAG, "Rejecting TCP client, clients=%d/%d, free_heap=%u",
                     tcp_client_count, MAX_CLIENTS, (unsigned)free_heap);
            safe_close_socket(sock);
            continue;
        }

        // 设置socket选项
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));

        int *client_id = malloc(sizeof(int));
        if (client_id == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for client ID");
            safe_close_socket(sock);
            continue;
        }
        *client_id = add_client(sock, NULL);
        if (*client_id < 0) {
            ESP_LOGE(TAG, "No free client slot, closing socket");
            free(client_id);
            safe_close_socket(sock);
            continue;
        }

        // 创建客户端处理任务。
        TaskHandle_t task_handle = NULL;
        BaseType_t task_ret = xTaskCreate(handle_client_task, "tcp_srv_client",
                                          TCP_CLIENT_TASK_STACK_SIZE,
                                          client_id, 5, &task_handle);
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create client task for client %d, free heap=%u",
                     *client_id, (unsigned)esp_get_free_heap_size());
            int failed_client_id = *client_id;
            free(client_id);
            remove_client(failed_client_id);
            continue;
        }

        // 更新任务句柄
        if (xSemaphoreTake(clients_mutex, portMAX_DELAY)) {
            if (*client_id >= 0 && *client_id < MAX_CLIENTS && clients[*client_id].in_use) {
                clients[*client_id].task_handle = task_handle;
            }
            xSemaphoreGive(clients_mutex);
        }
    }

CLEAN_UP:
  safe_close_socket(listen_sock);
  s_tcp_server_listen_sock = -1;
  xHandleTaskTCPServer = NULL;
  s_tcp_server_stop_requested = false;
  vTaskDelete(NULL);
}

void start_tcp_server(void) {
    if (xHandleTaskTCPServer != NULL) {
        ESP_LOGI(TAG, "TCP Server already running");
        return;
    }
    s_tcp_server_stop_requested = false;
    init_tcp_server();
    xTaskCreate(tcp_server_task, "tcp_server", 4096, (void *)AF_INET, 5,
                &xHandleTaskTCPServer);
}

void stop_tcp_server(void) {
    s_tcp_server_stop_requested = true;

    if (s_tcp_server_listen_sock >= 0) {
        shutdown(s_tcp_server_listen_sock, SHUT_RDWR);
        close(s_tcp_server_listen_sock);
        s_tcp_server_listen_sock = -1;
    }

    close_all_clients();

    if (xHandleTaskTCPServerHeartbeat != NULL) {
        vTaskDelete(xHandleTaskTCPServerHeartbeat);
        xHandleTaskTCPServerHeartbeat = NULL;
    }
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
