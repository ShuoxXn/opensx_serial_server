/*
 * @Author: Orion
 * @Date: 2024-01-23 16:08:26
 * @LastEditors: Orion
 * @LastEditTime: 2025-02-13 11:59:00
 * @FilePath: \SERIIAL_SERVER\main\sx_tcp_client.c
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sx_async_uart.h"
#include "sx_utils.h"
#include "sx_work_mode.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const char *TAG = "TCPCLIENT";
TaskHandle_t xHandleTaskTCPClient = NULL;
TaskHandle_t xHandleTaskTCPHeartbeat = NULL;
int global_client_sock = -1;

bool tcp_server_conn = false;
int global_tcp_server = 0;
static SemaphoreHandle_t tcp_send_mutex = NULL;
static volatile bool s_tcp_client_stop_requested = false;

// 在文件开始初始化互斥锁
void init_tcp_client(void) {
    if (tcp_send_mutex == NULL) {
        tcp_send_mutex = xSemaphoreCreateMutex();
    }
}

// 将十六进制字符串转换为二进制数据
static int hex_to_bin(const char *hex_str, uint8_t *bin_data, int max_len) {
    if (hex_str == NULL || bin_data == NULL) {
        ESP_LOGE(TAG, "Null pointer in hex_to_bin");
        return 0;
    }

    int hex_len = strlen(hex_str);
    int bin_len = 0;
    const char *parse_str = hex_str;

    ESP_LOGI(TAG, "Converting hex string: '%s' (length: %d)", hex_str, hex_len);

    // 跳过可能的0x前缀
    if (hex_len >= 2 && hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
        parse_str += 2;
        hex_len -= 2;
        ESP_LOGI(TAG, "Skipped 0x prefix, remaining: '%s'", parse_str);
    }

    // 移除空格和其他分隔符
    char *clean_hex = malloc(hex_len + 1);
    if (clean_hex == NULL) {
        ESP_LOGE(TAG, "Memory allocation failed");
        return 0;
    }

    int clean_pos = 0;
    for (int i = 0; i < hex_len; i++) {
        char c = parse_str[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) {
            clean_hex[clean_pos++] = c;
        }
        // 忽略空格和其他分隔符
    }
    clean_hex[clean_pos] = '\0';

    ESP_LOGI(TAG, "Cleaned hex string: '%s' (length: %d)", clean_hex, clean_pos);

    // 确保长度是偶数
    if (clean_pos % 2 != 0) {
        ESP_LOGE(TAG, "Hex string length must be even, got: %d", clean_pos);
        free(clean_hex);
        return 0;
    }

    bin_len = clean_pos / 2;
    if (bin_len > max_len) {
        ESP_LOGE(TAG, "Hex string too long: %d bytes, max: %d", bin_len, max_len);
        free(clean_hex);
        return 0;
    }

    for (int i = 0; i < bin_len; i++) {
        char high = clean_hex[i * 2];
        char low = clean_hex[i * 2 + 1];

        // 转换高位
        if (high >= '0' && high <= '9') {
            bin_data[i] = (high - '0') << 4;
        } else if (high >= 'A' && high <= 'F') {
            bin_data[i] = (high - 'A' + 10) << 4;
        } else if (high >= 'a' && high <= 'f') {
            bin_data[i] = (high - 'a' + 10) << 4;
        } else {
            ESP_LOGE(TAG, "Invalid hex character: %c at position %d", high, i * 2);
            free(clean_hex);
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
            ESP_LOGE(TAG, "Invalid hex character: %c at position %d", low, i * 2 + 1);
            free(clean_hex);
            return 0;
        }
    }

    free(clean_hex);
    ESP_LOGI(TAG, "Successfully converted %d bytes", bin_len);
    return bin_len;
}

// 获取MAC地址字符串（不带冒号）
static void get_mac_str(char *mac_str, size_t max_len) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_str, max_len, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// 发送注册包
static void send_register_packet(int sock) {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    // 打开 NVS
    err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return;
    }

    // 获取注册包配置
    size_t reg_packet_size = 0;
    size_t reg_format_size = 0;

    err = nvs_get_str(nvs_handle, "reg_packet", NULL, &reg_packet_size);
    if (err != ESP_OK || reg_packet_size == 0) {
        // 如果没有配置注册包，使用默认的MAC地址
        char mac_str[13] = {0};
        get_mac_str(mac_str, sizeof(mac_str));

        uint8_t data[64] = {0};
        int len = strlen(mac_str);
        memcpy(data, mac_str, len);

        ESP_LOGI(TAG, "Sending default register packet: %s", mac_str);
        send(sock, data, len, 0);
        nvs_close(nvs_handle);
        return;
    }

    // 获取注册包格式
    err = nvs_get_str(nvs_handle, "reg_format", NULL, &reg_format_size);
    if (err != ESP_OK || reg_format_size == 0) {
        ESP_LOGE(TAG, "Failed to get register packet format");
        nvs_close(nvs_handle);
        return;
    }

    // 分配内存
    char *reg_packet = malloc(reg_packet_size);
    char *reg_format = malloc(reg_format_size);

    if (reg_packet == NULL || reg_format == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for register packet");
        free(reg_packet);
        free(reg_format);
        nvs_close(nvs_handle);
        return;
    }

    // 读取配置
    err = nvs_get_str(nvs_handle, "reg_packet", reg_packet, &reg_packet_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read register packet");
        free(reg_packet);
        free(reg_format);
        nvs_close(nvs_handle);
        return;
    }

    err = nvs_get_str(nvs_handle, "reg_format", reg_format, &reg_format_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read register packet format");
        free(reg_packet);
        free(reg_format);
        nvs_close(nvs_handle);
        return;
    }

    if (strcmp(reg_format, "none") == 0) {
        ESP_LOGI(TAG, "Register packet disabled, skipping send");
        free(reg_packet);
        free(reg_format);
        nvs_close(nvs_handle);
        return;
    }

    // 如果注册包为空字符串，使用MAC地址作为默认值
    // 如果用户主动设置为空，应该尊重用户选择，不发送注册包
    if (strlen(reg_packet) == 0) {
        char mac_str[13] = {0};
        get_mac_str(mac_str, sizeof(mac_str));
        strcpy(reg_packet, mac_str);
    }

    // 只有当注册包不为空字符串时才发送注册包
    if (strlen(reg_packet) > 0) {
        // 根据格式发送数据
        if (strcmp(reg_format, "hex") == 0) {
            // 十六进制格式
            uint8_t bin_data[128] = {0};
            int bin_len = hex_to_bin(reg_packet, bin_data, sizeof(bin_data));

            if (bin_len > 0) {
                ESP_LOGI(TAG, "Sending HEX register packet, length: %d", bin_len);
                send(sock, bin_data, bin_len, 0);
            } else {
                ESP_LOGE(TAG, "Failed to convert HEX register packet");
            }
        } else {
            // ASCII格式
            ESP_LOGI(TAG, "Sending ASCII register packet: %s", reg_packet);
            send(sock, reg_packet, strlen(reg_packet), 0);
        }
    } else {
        ESP_LOGI(TAG, "Register packet is empty, skipping send");
    }

    // 清理
    free(reg_packet);
    free(reg_format);
    nvs_close(nvs_handle);
}

// 心跳包任务
void tcp_heartbeat_task(void *pvParameters) {
    int sock = *((int*)pvParameters);
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

    // 读取配置
    if (heart_packet) {
        err = nvs_get_str(nvs_handle, "heart_packet", heart_packet, &heart_packet_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read heartbeat packet");
            free(heart_packet);
            heart_packet = NULL;
        }
    }

    if (heart_format) {
        err = nvs_get_str(nvs_handle, "heart_format", heart_format, &heart_format_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read heartbeat format");
            free(heart_format);
            heart_format = NULL;
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
        free(heart_interval_str);
        heart_interval_str = NULL;
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

    if (heart_format && strcmp(heart_format, "none") == 0) {
        ESP_LOGI(TAG, "Heartbeat disabled, stopping heartbeat task");
        if (heart_packet) {
            free(heart_packet);
        }
        free(heart_format);
        nvs_close(nvs_handle);
        xHandleTaskTCPHeartbeat = NULL;
        vTaskDelete(NULL);
        return;
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
    uint8_t bin_data[128] = {0};
    int bin_len = 0;

    while (1) {
        // 只有当heart_packet不为空字符串时才发送心跳包并记录日志
        if (heart_packet && strlen(heart_packet) > 0) {
            ESP_LOGI(TAG, "Sending heartbeat...");

            // 根据格式发送心跳包
            if (heart_format && strcmp(heart_format, "hex") == 0) {
                // 十六进制格式
                ESP_LOGI(TAG, "Converting HEX heartbeat: %s", heart_packet);
                bin_len = hex_to_bin(heart_packet, bin_data, sizeof(bin_data));
                if (bin_len > 0) {
                    ESP_LOGI(TAG, "Sending HEX heartbeat packet, length: %d", bin_len);
                    // 打印十六进制数据用于调试
                    char hex_debug[256] = {0};
                    for (int i = 0; i < bin_len && i < 32; i++) {
                        sprintf(hex_debug + strlen(hex_debug), "%02X ", bin_data[i]);
                    }
                    ESP_LOGI(TAG, "HEX data: %s", hex_debug);

                    if (xSemaphoreTake(tcp_send_mutex, portMAX_DELAY)) {
                        int sent = send(sock, bin_data, bin_len, 0);
                        if (sent > 0) {
                            ESP_LOGI(TAG, "HEX heartbeat sent successfully, %d bytes", sent);
                            xSemaphoreGive(tcp_send_mutex);
                        } else {
                            ESP_LOGE(TAG, "Failed to send HEX heartbeat: errno %d", errno);
                            xSemaphoreGive(tcp_send_mutex);

                            // 检查是否为致命的socket错误
                            if (errno == ENOTCONN || errno == EBADF || errno == EPIPE || errno == ECONNRESET) {
                                ESP_LOGE(TAG, "Socket disconnected, stopping heartbeat task");
                                vTaskDelete(NULL);
                                return;
                            }
                        }
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to convert HEX heartbeat packet: %s", heart_packet);
                }
            } else {
                // ASCII格式
                ESP_LOGI(TAG, "Sending ASCII heartbeat: %s", heart_packet);
                if (xSemaphoreTake(tcp_send_mutex, portMAX_DELAY)) {
                    int sent = send(sock, heart_packet, strlen(heart_packet), 0);
                    if (sent > 0) {
                        ESP_LOGI(TAG, "ASCII heartbeat sent successfully, %d bytes", sent);
                        xSemaphoreGive(tcp_send_mutex);
                    } else {
                        ESP_LOGE(TAG, "Failed to send ASCII heartbeat: errno %d", errno);
                        xSemaphoreGive(tcp_send_mutex);

                        // 检查是否为致命的socket错误
                        if (errno == ENOTCONN || errno == EBADF || errno == EPIPE || errno == ECONNRESET) {
                            ESP_LOGE(TAG, "Socket disconnected, stopping heartbeat task");
                            vTaskDelete(NULL);
                            return;
                        }
                    }
                }
            }
        }

        // 休眠指定时间
        vTaskDelay(heart_interval * 1000 / portTICK_PERIOD_MS);

        // 检查TCP服务器是否仍然连接
        if (!tcp_server_conn || sock != global_client_sock) {
            break;
        }
    }

    // 清理资源
    if (heart_packet) {
        free(heart_packet);
    }
    if (heart_format) {
        free(heart_format);
    }

    xHandleTaskTCPHeartbeat = NULL;
    vTaskDelete(NULL);
}

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

void tcpclient_message(char *data, int len) {
  static const char *TAG = "TCP_CLIENT";

  if (data == NULL || len <= 0) {
    ESP_LOGW(TAG, "Invalid TCP client payload: len=%d", len);
    return;
  }

  if (global_client_sock == -1) {
    ESP_LOGE(TAG, "No active socket to send message");
    return;
  }

  int written = send(global_client_sock, data, len, 0);
  if (written < 0) {
    ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
  }
}

void tcp_client(void *pvParameters) {
  char rx_buffer[256];
  int addr_family = 0;
  int ip_protocol = 0;
  int port = 0;
  char ip[32];
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
        strncpy(ip, nvs_tcp_server, sizeof(ip));
        ip[sizeof(ip) - 1] = '\0';
        if (nvs_tcp_port && nvs_tcp_port[0] != '\0') {
          port = atoi(nvs_tcp_port);
        } else {
          port = 8888;
        }
        printf("Copied string: %s\n", ip);
        printf("Copied port: %d\n", port);
      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
      free(nvs_use_tcp);
      free(nvs_tcp_server);
      free(nvs_tcp_port);
    }
  }
  nvs_close(nvs_handle);

  struct sockaddr_in dest_addr;
  inet_pton(AF_INET, ip, &dest_addr.sin_addr);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(port);
  addr_family = AF_INET;
  ip_protocol = IPPROTO_IP;

  while (!s_tcp_client_stop_requested) {
    int sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    global_client_sock = sock;
    if (sock < 0) {
      ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }
    ESP_LOGI(TAG, "Socket created, connecting to %s:%d", ip, port);

    int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
      ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
      if (!s_tcp_client_stop_requested) {
        close(sock);
      }
      global_client_sock = -1;
      tcp_server_conn = false;
      global_tcp_server = 0;
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    } else {
      ESP_LOGI(TAG, "Successfully connected");
      tcp_server_conn = true;
      global_tcp_server = 1;

      // 发送注册包
      send_register_packet(sock);

      // 启动心跳包任务
      if (xHandleTaskTCPHeartbeat == NULL) {
          xTaskCreate(tcp_heartbeat_task, "tcp_heartbeat", 4096, (void*)&global_client_sock, 5, &xHandleTaskTCPHeartbeat);
      }
    }

    while (tcp_server_conn && !s_tcp_client_stop_requested) {
      int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
      if (len < 0) {
        ESP_LOGE(TAG, "recv failed: errno %d", errno);
        if (!s_tcp_client_stop_requested) {
          close(sock);
        }
        global_client_sock = -1;
        tcp_server_conn = false;
        global_tcp_server = 0;
        break;
      } else if (len == 0) {
        ESP_LOGW(TAG, "Connection closed");
        if (!s_tcp_client_stop_requested) {
          close(sock);
        }
        global_client_sock = -1;
        tcp_server_conn = false;
        global_tcp_server = 0;
        break;
      } else {
        rx_buffer[len] = 0;
        ESP_LOGI(TAG, "Received %d bytes from %s:", len, ip);
        ESP_LOGI(TAG, "%s", rx_buffer);
        sx_work_mode_tcp_handler_t handler = sx_work_mode_get_tcp_handler();
        if (handler != NULL) {
          esp_err_t handler_err = handler((uint8_t *)rx_buffer, len);
          if (handler_err != ESP_OK) {
            ESP_LOGW(TAG, "work mode TCP handler failed: %s",
                     esp_err_to_name(handler_err));
          }
        } else {
          tx_task((uint8_t *)rx_buffer, len);
        }
      }
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
  if (global_client_sock != -1) {
    shutdown(global_client_sock, SHUT_RDWR);
    close(global_client_sock);
    global_client_sock = -1;
  }
  tcp_server_conn = false;
  global_tcp_server = 0;
  xHandleTaskTCPClient = NULL;
  s_tcp_client_stop_requested = false;
  vTaskDelete(NULL);
}

void start_tcp_client(void) {
  if (xHandleTaskTCPClient == NULL) {
    s_tcp_client_stop_requested = false;
    init_tcp_client();
    // 增加栈大小到8KB以支持大数据传输
    xTaskCreate(&tcp_client, "tcp_client", 8192, NULL, 5,
                &xHandleTaskTCPClient);
  }
}

void stop_tcp_client(void) {
  s_tcp_client_stop_requested = true;
  tcp_server_conn = false;
  global_tcp_server = 0;

  if (global_client_sock != -1) {
    shutdown(global_client_sock, SHUT_RDWR);
    close(global_client_sock);
    global_client_sock = -1;
  }

  if (xHandleTaskTCPHeartbeat != NULL) {
    vTaskDelete(xHandleTaskTCPHeartbeat);
    xHandleTaskTCPHeartbeat = NULL;
  }
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
