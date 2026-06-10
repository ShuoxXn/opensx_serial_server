/*
 * @Author: Orion
 * @Date: 2024-01-23 16:08:16
 * @LastEditors: Orion
 * @LastEditTime: 2025-05-20 21:42:17
 * @FilePath: \SERIIAL_SERVER\main\sx_udp_server.c
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>  // 添加此头文件以解决close函数未声明的问题

#include "cJSON.h"
#include "esp_timer.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "sx_async_uart.h"
#include "sx_utils.h"
#include "sx_web_server.h"  // 添加此头文件以获取网络变量声明
#include <lwip/netdb.h>


/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

static const char *TAG = "UDPSERVER";
#define UDP_SERVER_PORT 37210

// 声明外部变量
extern char got_ip_addrs[16];
extern char got_ip_netmask[16];
extern char got_ip_gw[16];

TaskHandle_t xHandleUDPServerTask;
static volatile bool s_udp_server_stop_requested = false;
static int s_udp_server_sock = -1;
typedef struct {
  bool active;
  uint32_t client_addr;
  int64_t expires_at_us;
} udp_auth_session_t;

static udp_auth_session_t auth_session = {0};
#define AUTH_SESSION_TIMEOUT_US (60LL * 1000 * 1000)

static void auth_session_clear(void) {
  memset(&auth_session, 0, sizeof(auth_session));
}

static void auth_session_start(uint32_t client_addr) {
  auth_session.active = true;
  auth_session.client_addr = client_addr;
  auth_session.expires_at_us = esp_timer_get_time() + AUTH_SESSION_TIMEOUT_US;
}

static bool auth_session_valid(uint32_t client_addr) {
  if (!auth_session.active) {
    return false;
  }
  int64_t now = esp_timer_get_time();
  if (now > auth_session.expires_at_us) {
    auth_session_clear();
    return false;
  }
  return auth_session.client_addr == client_addr;
}

static esp_err_t save_json_value_to_nvs(nvs_handle_t handle, cJSON *parent,
                                        const char *json_key,
                                        const char *nvs_key,
                                        bool *changed) {
  if (!parent || !json_key || !nvs_key) {
    return ESP_ERR_INVALID_ARG;
  }

  cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, json_key);
  if (!cJSON_IsString(item) && !cJSON_IsNumber(item) && !cJSON_IsBool(item)) {
    return ESP_OK;
  }

  char temp_buf[32] = {0};
  const char *value_to_write = NULL;
  if (cJSON_IsString(item) && item->valuestring) {
    value_to_write = item->valuestring;
  } else if (cJSON_IsNumber(item)) {
    snprintf(temp_buf, sizeof(temp_buf), "%d", item->valueint);
    value_to_write = temp_buf;
  } else if (cJSON_IsBool(item)) {
    value_to_write = cJSON_IsTrue(item) ? "1" : "0";
  }

  if (value_to_write == NULL) {
    return ESP_OK;
  }

  esp_err_t err = nvs_set_str(handle, nvs_key, value_to_write);
  if (err == ESP_OK && changed) {
    *changed = true;
  }
  return err;
}

static esp_err_t update_device_identity_from_parm(cJSON *parm,
                                                  bool *changed) {
  if (!parm || !cJSON_IsObject(parm)) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  bool local_changed = false;
  const struct {
    const char *json_key;
    const char *nvs_key;
  } mappings[] = {
      {"type", "device_type"},   {"model", "device_type"},
      {"sn", "device_sn"},       {"device_sn", "device_sn"},
      {"thsensor", "thsensor"},  {"psensor", "psensor"},
      {"adapter_id", "adapter_id"},
      {"interface", "interface"},{"power", "power"}};

  for (size_t i = 0; i < sizeof(mappings) / sizeof(mappings[0]); i++) {
    err = save_json_value_to_nvs(nvs_handle, parm, mappings[i].json_key,
                                 mappings[i].nvs_key, &local_changed);
    if (err != ESP_OK) {
      break;
    }
  }

  if (err == ESP_OK && local_changed) {
    err = nvs_commit(nvs_handle);
  }

  nvs_close(nvs_handle);

  if (err == ESP_OK && changed) {
    *changed = local_changed;
  }

  return err;
}

static char *process_exec_request(cJSON *root) {
  cJSON *parm = cJSON_GetObjectItem(root, "parm");
  if (cJSON_IsString(parm) && parm->valuestring) {
    if (strcmp(parm->valuestring, "device_info") == 0) {
      char *payload = send_device_ip();
      if (payload) {
        return payload;
      }
      return strdup("{\"error\":\"internal error\"}");
    }
    return strdup("{\"error\":\"unsupported command\"}");
  }
  return strdup("{\"error\":\"invalid parm\"}");
}

static esp_err_t save_network_settings_to_nvs(const char *is_dhcp,
                                              const char *name,
                                              const char *ip,
                                              const char *mask,
                                              const char *gateway,
                                              const char *primary_dns,
                                              const char *secondary_dns) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  err = nvs_set_str(nvs_handle, "is_dhcp", is_dhcp);
  if (err == ESP_OK)
    err = nvs_set_str(nvs_handle, "host_names", name);
  if (err == ESP_OK)
    err = nvs_set_str(nvs_handle, "static_ip", ip);
  if (err == ESP_OK)
    err = nvs_set_str(nvs_handle, "static_netmask", mask);
  if (err == ESP_OK)
    err = nvs_set_str(nvs_handle, "static_gateway", gateway);
  if (err == ESP_OK && primary_dns)
    err = nvs_set_str(nvs_handle, "primary_dns", primary_dns);
  if (err == ESP_OK && secondary_dns)
    err = nvs_set_str(nvs_handle, "secondary_dns", secondary_dns);

  if (err == ESP_OK) {
    err = nvs_commit(nvs_handle);
  }

  nvs_close(nvs_handle);
  return err;
}

static void udp_server_task(void *pvParameters) {
  char rx_buffer[512];
  char addr_str[128];
  int addr_family = AF_INET;
  int ip_protocol = 0;
  struct sockaddr_in dest_addr;
  struct sockaddr_in source_addr;

  while (!s_udp_server_stop_requested) {
    if (addr_family == AF_INET) {
      dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
      dest_addr.sin_family = AF_INET;
      dest_addr.sin_port = htons(UDP_SERVER_PORT);
      ip_protocol = IPPROTO_IP;
    }

    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0) {
      ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
      break;
    }
    s_udp_server_sock = sock;
    ESP_LOGI(TAG, "Socket created");

    // 设置SO_REUSEADDR选项
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 设置广播选项
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
      ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
      closesocket(sock);
      s_udp_server_sock = -1;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", UDP_SERVER_PORT);

    socklen_t socklen = sizeof(source_addr);

    while (!s_udp_server_stop_requested) {
      ESP_LOGI(TAG, "Waiting for data");
      int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                         (struct sockaddr *)&source_addr, &socklen);

      // Error occurred during receiving
      if (len < 0) {
        if (!s_udp_server_stop_requested) {
          ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
        }
        break;
      }
      // Data received
      else {
        // Get the sender's ip address as string
        if (source_addr.sin_family == AF_INET) {
          inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str,
                      sizeof(addr_str) - 1);
        }

        rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string...
        ESP_LOGI(TAG, "Received %d bytes from %s:", len, addr_str);
        ESP_LOGI(TAG, "%s", rx_buffer);

        // 显示本地网络信息，便于调试
        ESP_LOGI(TAG, "Local IP: %s, Netmask: %s, Gateway: %s",
                 got_ip_addrs, got_ip_netmask, got_ip_gw);

        cJSON *pJsonRoot = cJSON_Parse(rx_buffer);
        if (pJsonRoot == NULL) {
          const char *error_ptr = cJSON_GetErrorPtr();
          if (error_ptr != NULL) {
            ESP_LOGE(TAG, "Error before: %s", error_ptr);
          }
          continue;
        }

        bool restart_requested = false;
        char *udpmsg = NULL;

        cJSON *cmd = cJSON_GetObjectItemCaseSensitive(pJsonRoot, "cmd");
        cJSON *device = cJSON_GetObjectItemCaseSensitive(pJsonRoot, "device");
        bool command_present = (cJSON_IsString(cmd) && cmd->valuestring) ||
                               (cJSON_IsString(device) && device->valuestring);

        bool auth_ok = auth_session_valid(source_addr.sin_addr.s_addr);
        bool auth_payload = false;
        cJSON *uname = cJSON_GetObjectItem(pJsonRoot, "username");
        cJSON *passwd = cJSON_GetObjectItem(pJsonRoot, "password");
        if (cJSON_IsString(uname) && cJSON_IsString(passwd)) {
          auth_payload = true;
          esp_err_t rets = nvs_init();
          nvs_handle_t nvs_handle;
          ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));
          size_t lgname_len = 0, lgpwd_len = 0;
          rets = nvs_get_str(nvs_handle, "lgname", NULL, &lgname_len);
          rets = nvs_get_str(nvs_handle, "lgpwd", NULL, &lgpwd_len);
          switch (rets) {
          case ESP_OK:
            break;
          case ESP_ERR_NVS_NOT_FOUND:
            ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "lgname", "admin"));
            ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "lgpwd", "12345678"));
            ESP_ERROR_CHECK(nvs_commit(nvs_handle));
            break;
          default:
            ESP_LOGE(TAG, "Error getting NVS data: %s", esp_err_to_name(rets));
            break;
          }
          bool login_ok = false;
          if (rets == ESP_OK) {
            char *nvs_lgname = malloc(lgname_len);
            char *nvs_lgpwd = malloc(lgpwd_len);
            if (nvs_lgname && nvs_lgpwd) {
              rets = nvs_get_str(nvs_handle, "lgname", nvs_lgname, &lgname_len);
              rets = nvs_get_str(nvs_handle, "lgpwd", nvs_lgpwd, &lgpwd_len);
              if (rets == ESP_OK &&
                  strcmp(uname->valuestring, nvs_lgname) == 0 &&
                  strcmp(passwd->valuestring, nvs_lgpwd) == 0) {
                auth_session_start(source_addr.sin_addr.s_addr);
                login_ok = true;
              }
            } else {
              ESP_LOGE(TAG, "Memory allocation failed in login");
            }
            if (nvs_lgname)
              free(nvs_lgname);
            if (nvs_lgpwd)
              free(nvs_lgpwd);
          }
          nvs_close(nvs_handle);

          auth_ok = login_ok;
          if (!command_present) {
            udpmsg = strdup(login_ok ? "{\"result\":\"ok\"}"
                                     : "{\"result\":\"failed\"}");
          } else if (!login_ok) {
            udpmsg = strdup("{\"error\":\"auth failed\"}");
          }
        }

        bool cmd_processed = false;
        if (udpmsg == NULL && cJSON_IsString(cmd) && cmd->valuestring) {
          cmd_processed = true;
          if (strcmp(cmd->valuestring, "set_device_type") == 0) {
            cJSON *parm = cJSON_GetObjectItem(pJsonRoot, "parm");
            if (!auth_ok) {
              udpmsg = strdup("{\"error\":\"auth required\"}");
            } else if (!parm || !cJSON_IsObject(parm)) {
              udpmsg = strdup("{\"error\":\"invalid parm\"}");
            } else {
              bool updated = false;
              esp_err_t err = update_device_identity_from_parm(parm, &updated);
              if (err == ESP_OK && updated) {
                udpmsg = strdup("{\"result\":\"ok\"}");
              } else if (err == ESP_OK) {
                udpmsg = strdup("{\"error\":\"no valid fields\"}");
              } else {
                ESP_LOGE(TAG, "Failed to update device type: %s", esp_err_to_name(err));
                udpmsg = strdup("{\"error\":\"nvs failure\"}");
              }
            }
          } else {
            udpmsg = strdup("{\"error\":\"unsupported command\"}");
          }
        }

        if (!cmd_processed && udpmsg == NULL && cJSON_IsString(device) &&
            (device->valuestring != NULL)) {
          if (strcmp(device->valuestring, "scan") == 0) {
            // 设备搜索请求，返回设备信息
            ESP_LOGI(TAG, "Received scan command from tool, sending device info");
            udpmsg = send_device_ip();
            if (udpmsg) {
              ESP_LOGI(TAG, "Device info to send: %s", udpmsg);
              // 发送响应
              int err = sendto(sock, udpmsg, strlen(udpmsg), 0,
                              (struct sockaddr *)&source_addr, sizeof(source_addr));
              if (err < 0) {
                ESP_LOGE(TAG, "Error sending response: errno %d", errno);
              } else {
                ESP_LOGI(TAG, "Response sent successfully: %d bytes", err);
              }
              free(udpmsg);
              udpmsg = NULL;
            } else {
              ESP_LOGE(TAG, "Failed to generate device info response");
            }
          } else if (strcmp(device->valuestring, "exec") == 0) {
            udpmsg = process_exec_request(pJsonRoot);
          } else if (strcmp(device->valuestring, "set") == 0) {
            cJSON *parm = cJSON_GetObjectItem(pJsonRoot, "parm");
            if (parm == NULL) {
              ESP_LOGE(TAG, "Missing parm field in set command");
              udpmsg = strdup("{\"change\":\"failed\"}");
            } else if (!auth_ok) {
              ESP_LOGW(TAG, "Set command rejected: authentication required");
              udpmsg = strdup("{\"change\":\"failed\"}");
            } else {
              char *parm_str = cJSON_Print(parm);
              if (parm_str) {
                ESP_LOGI(TAG, "Received network settings parameters: %s", parm_str);
                free(parm_str);
              }

              cJSON *command = cJSON_GetObjectItem(parm, "command");
              if (cJSON_IsString(command) && command->valuestring) {
                if (strcmp(command->valuestring, "reboot") == 0) {
                  udpmsg = strdup("{\"change\":\"ok\"}");
                  restart_requested = true;
                  auth_session_clear();
                } else {
                  ESP_LOGW(TAG, "Unsupported command: %s", command->valuestring);
                  udpmsg = strdup("{\"error\":\"unsupported command\"}");
                }
              } else {
                cJSON *name = cJSON_GetObjectItem(parm, "name");
                cJSON *ip = cJSON_GetObjectItem(parm, "ip");
                cJSON *mask = cJSON_GetObjectItem(parm, "mask");
                cJSON *gateway = cJSON_GetObjectItem(parm, "gateway");
                cJSON *primary_dns = cJSON_GetObjectItem(parm, "primary_dns");
                cJSON *secondary_dns = cJSON_GetObjectItem(parm, "secondary_dns");
                cJSON *is_dhcp = cJSON_GetObjectItem(parm, "is_dhcp");

                ESP_LOGI(TAG, "name field: %s, valid: %d",
                         name ? (name->valuestring ? name->valuestring : "NULL") : "NOT FOUND",
                         (name && cJSON_IsString(name) && name->valuestring));
                ESP_LOGI(TAG, "ip field: %s, valid: %d",
                         ip ? (ip->valuestring ? ip->valuestring : "NULL") : "NOT FOUND",
                         (ip && cJSON_IsString(ip) && ip->valuestring));
                ESP_LOGI(TAG, "mask field: %s, valid: %d",
                         mask ? (mask->valuestring ? mask->valuestring : "NULL") : "NOT FOUND",
                         (mask && cJSON_IsString(mask) && mask->valuestring));
                ESP_LOGI(TAG, "gateway field: %s, valid: %d",
                         gateway ? (gateway->valuestring ? gateway->valuestring : "NULL") : "NOT FOUND",
                         (gateway && cJSON_IsString(gateway) && gateway->valuestring));
                ESP_LOGI(TAG, "primary_dns field: %s, valid: %d",
                         primary_dns ? (primary_dns->valuestring ? primary_dns->valuestring : "NULL") : "NOT FOUND",
                         (primary_dns && cJSON_IsString(primary_dns) && primary_dns->valuestring));
                ESP_LOGI(TAG, "secondary_dns field: %s, valid: %d",
                         secondary_dns ? (secondary_dns->valuestring ? secondary_dns->valuestring : "NULL") : "NOT FOUND",
                         (secondary_dns && cJSON_IsString(secondary_dns) && secondary_dns->valuestring));
                ESP_LOGI(TAG, "is_dhcp field: %s, valid: %d",
                         is_dhcp ? (is_dhcp->valuestring ? is_dhcp->valuestring : "NULL") : "NOT FOUND",
                         (is_dhcp && cJSON_IsString(is_dhcp) && is_dhcp->valuestring));

                bool network_present = name || ip || mask || gateway ||
                                       primary_dns || secondary_dns || is_dhcp;

                if (network_present) {
                  if (cJSON_IsString(name) && cJSON_IsString(ip) &&
                      cJSON_IsString(mask) && cJSON_IsString(gateway)) {

                    const char *primary_dns_value = "8.8.8.8";
                    const char *secondary_dns_value = "114.114.114.114";
                    const char *is_dhcp_value = "2"; // 默认静态IP

                    if (primary_dns && cJSON_IsString(primary_dns) && primary_dns->valuestring) {
                      primary_dns_value = primary_dns->valuestring;
                    }

                    if (secondary_dns && cJSON_IsString(secondary_dns) && secondary_dns->valuestring) {
                      secondary_dns_value = secondary_dns->valuestring;
                    }

                    if (is_dhcp && cJSON_IsString(is_dhcp) && is_dhcp->valuestring) {
                      is_dhcp_value = is_dhcp->valuestring;
                    }

                    ESP_LOGI(TAG, "Persist network settings - Name: %s, IP: %s, Mask: %s, Gateway: %s, Primary DNS: %s, Secondary DNS: %s, IP Type: %s",
                             name->valuestring, ip->valuestring, mask->valuestring, gateway->valuestring,
                             primary_dns_value, secondary_dns_value, is_dhcp_value);

                    esp_err_t save_ret =
                        save_network_settings_to_nvs(is_dhcp_value, name->valuestring,
                                                     ip->valuestring, mask->valuestring,
                                                     gateway->valuestring, primary_dns_value,
                                                     secondary_dns_value);
                    if (save_ret == ESP_OK) {
                      udpmsg = strdup("{\"change\":\"ok\"}");
                      restart_requested = true;
                      auth_session_clear();
                    } else {
                      ESP_LOGE(TAG, "Failed to save network settings: %s", esp_err_to_name(save_ret));
                      udpmsg = strdup("{\"change\":\"failed\"}");
                    }
                  } else {
                    ESP_LOGE(TAG, "Partial network parameters received, rejecting");
                    udpmsg = strdup("{\"error\":\"invalid network params\"}");
                  }
                }

                if (udpmsg == NULL) {
                  bool updated = false;
                  esp_err_t err = update_device_identity_from_parm(parm, &updated);
                  if (err == ESP_OK && updated) {
                    udpmsg = strdup("{\"change\":\"ok\"}");
                  } else if (err == ESP_OK) {
                    udpmsg = strdup("{\"error\":\"invalid parm\"}");
                  } else {
                    ESP_LOGE(TAG, "Failed to update device metadata: %s",
                             esp_err_to_name(err));
                    udpmsg = strdup("{\"error\":\"nvs failure\"}");
                  }
                }
              }
            }
          }
        } else if (!cmd_processed && udpmsg == NULL && auth_payload) {
          // 带鉴权信息但没有有效命令，保持鉴权回复
          udpmsg = strdup(auth_ok ? "{\"result\":\"ok\"}"
                                   : "{\"result\":\"failed\"}");
        }

        cJSON_Delete(pJsonRoot);

        if (udpmsg != NULL) {
          int err = sendto(sock, udpmsg, strlen(udpmsg), 0,
                         (struct sockaddr *)&source_addr, sizeof(source_addr));
          if (err < 0) {
            ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
          }
          free(udpmsg);
        }

        if (restart_requested) {
          vTaskDelay(500 / portTICK_PERIOD_MS);
          ESP_LOGI(TAG, "Reboot requested after configuration update");
          esp_restart();
        }
      }
    }

    if (sock != -1) {
      ESP_LOGI(TAG, "Shutting down UDP socket");
      shutdown(sock, 0);
      closesocket(sock);
      if (s_udp_server_sock == sock) {
        s_udp_server_sock = -1;
      }
    }
  }
  xHandleUDPServerTask = NULL;
  s_udp_server_sock = -1;
  s_udp_server_stop_requested = false;
  vTaskDelete(NULL);
}

void start_udp_server(void) {
  if (xHandleUDPServerTask != NULL) {
    ESP_LOGI(TAG, "UDP server already running");
    return;
  }
  s_udp_server_stop_requested = false;
  xTaskCreate(udp_server_task, "udp_server", 4096, (void *)AF_INET, 5, &xHandleUDPServerTask);
}

void kill_udp_server(void) {
  if (xHandleUDPServerTask == NULL) {
    return;
  }

  s_udp_server_stop_requested = true;
  if (s_udp_server_sock >= 0) {
    shutdown(s_udp_server_sock, SHUT_RDWR);
    closesocket(s_udp_server_sock);
    s_udp_server_sock = -1;
  }
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
