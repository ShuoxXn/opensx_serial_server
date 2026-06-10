#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_async_uart.h"
#include "sx_utils.h"
#include "sx_work_mode_ids.h"
#include "sx_web_server.h"
#include "sx_work_mode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LED 14

char pubtopic[30] = {0};
char subtopic[30] = {0};
char mqtt_client_id[30] = {0};

// 声明外部变量
extern portMUX_TYPE uart_spinlock;

TaskHandle_t xMqttHandle = NULL;

static const char *TAG = "SX_MQTT_CLIENT";
esp_mqtt_client_handle_t client = NULL;

// 🔧 添加内存中的MQTT状态跟踪
static bool mqtt_actually_connected = false;

int mqtt_publish_topic(const char *topic, const char *data, int len, int qos,
                       int retain);

// 通过 MQTT 下发配置的简易处理：识别特定 JSON 载荷并写入 NVS
static bool mqtt_apply_config_payload(const char *payload, int payload_len) {
  if (payload == NULL || payload_len <= 0) {
    return false;
  }

  char *json_buf = malloc(payload_len + 1);
  if (json_buf == NULL) {
    ESP_LOGE(TAG, "无法为配置消息分配内存");
    return false;
  }
  memcpy(json_buf, payload, payload_len);
  json_buf[payload_len] = '\0';

  cJSON *root = cJSON_Parse(json_buf);
  if (root == NULL) {
    free(json_buf);
    return false;
  }

  // 约定：收到 {"action":"config","data":{...}} 或 {"cmd":"config",...}
  // 也接受顶层包含 "config"/"nvs" 对象的载荷
  bool declared_config = false;
  const char *config_keys[] = {"action", "cmd", "type"};
  for (size_t i = 0; i < sizeof(config_keys) / sizeof(config_keys[0]); i++) {
    cJSON *flag = cJSON_GetObjectItemCaseSensitive(root, config_keys[i]);
    if (cJSON_IsString(flag) && flag->valuestring &&
        strcmp(flag->valuestring, "config") == 0) {
      declared_config = true;
      break;
    }
  }

  cJSON *config_obj = NULL;
  if (declared_config) {
    config_obj = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (config_obj == NULL) {
      config_obj = cJSON_GetObjectItemCaseSensitive(root, "config");
    }
  }
  if (config_obj == NULL) {
    config_obj = cJSON_GetObjectItemCaseSensitive(root, "config");
  }
  if (config_obj == NULL) {
    config_obj = cJSON_GetObjectItemCaseSensitive(root, "nvs");
  }
  if (config_obj == NULL && declared_config) {
    config_obj = root; // 允许直接下发扁平键值
  }

  if (config_obj == NULL || !cJSON_IsObject(config_obj)) {
    cJSON_Delete(root);
    free(json_buf);
    return false;
  }

  bool is_config = true;
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "打开 NVS 失败: %s", esp_err_to_name(err));
    cJSON_Delete(root);
    free(json_buf);
    return is_config;
  }

  bool updated = false;
  bool committed = false;
  bool restart_needed = false;
  char selected_work_mode[32] = WORK_MODE_DEFAULT;
  nvs_get_work_mode(nvs_handle, selected_work_mode,
                    sizeof(selected_work_mode));
  cJSON *item = NULL;
  cJSON_ArrayForEach(item, config_obj) {
    if (item->string == NULL) {
      continue;
    }

    if (strcmp(item->string, "work_mode") == 0 ||
        strcmp(item->string, WORK_MODE_NVS_KEY) == 0) {
      if (cJSON_IsString(item) && item->valuestring != NULL) {
        if (is_valid_work_mode(item->valuestring)) {
          if (nvs_set_work_mode(nvs_handle, item->valuestring) == ESP_OK) {
            snprintf(selected_work_mode, sizeof(selected_work_mode), "%s",
                     item->valuestring);
            updated = true;
          }
        } else {
          ESP_LOGW(TAG, "忽略无效work_mode: %s", item->valuestring);
        }
      }
      continue;
    }

    if (strcmp(item->string, "poll_time") == 0) {
      int poll_time_ms = 0;
      if (cJSON_IsString(item) && item->valuestring != NULL) {
        poll_time_ms = atoi(item->valuestring);
      } else if (cJSON_IsNumber(item)) {
        poll_time_ms = (int)item->valuedouble;
      }

      if (poll_time_ms > 0) {
        if (poll_time_ms < 1000) {
          poll_time_ms = 1000;
        } else if (poll_time_ms > 3600000) {
          poll_time_ms = 3600000;
        }
        char poll_buf[16];
        snprintf(poll_buf, sizeof(poll_buf), "%d", poll_time_ms);
        if (nvs_set_str(nvs_handle, "poll_time", poll_buf) == ESP_OK) {
          updated = true;
        }
      }
      continue;
    }

    if (strcmp(item->string, "mode_config") == 0) {
      continue;
    }

    if (strcmp(item->string, "device_sn") == 0 ||
        strcmp(item->string, "mqttconn") == 0 ||
        strcmp(item->string, "mqtt_type") == 0 ||
        strcmp(item->string, "modbus_items") == 0 ||
        strcmp(item->string, "tcp_send") == 0) {
      ESP_LOGW(TAG, "忽略MQTT配置中的%s", item->string);
      continue;
    }

    const char *value_ptr = NULL;
    char number_buffer[32];

    if (cJSON_IsString(item) && item->valuestring != NULL) {
      value_ptr = item->valuestring;
    } else if (cJSON_IsNumber(item)) {
      snprintf(number_buffer, sizeof(number_buffer), "%.15g",
               item->valuedouble);
      value_ptr = number_buffer;
    } else if (cJSON_IsBool(item)) {
      value_ptr = cJSON_IsTrue(item) ? "1" : "0";
    } else {
      continue; // 仅处理可序列化为字符串的类型
    }

    err = nvs_set_str(nvs_handle, item->string, value_ptr);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "写入 NVS 键 %s 失败: %s", item->string,
               esp_err_to_name(err));
    } else {
      updated = true;
    }
  }

  cJSON *mode_config = cJSON_GetObjectItem(config_obj, "mode_config");
  if (mode_config != NULL) {
    err = sx_work_mode_save_config(selected_work_mode, nvs_handle, config_obj);
    if (err == ESP_OK) {
      updated = true;
    } else {
      ESP_LOGW(TAG, "忽略无效工作模式配置: %s (%s)", selected_work_mode,
               esp_err_to_name(err));
    }
  }

  if (updated) {
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "提交 NVS 失败: %s", esp_err_to_name(err));
    } else {
      committed = true;
      ESP_LOGI(TAG, "MQTT 配置已写入 NVS");
    }
  }

  if (committed) {
    char mqtt_sub_topic[128] = {0};
    char qos_str[8] = {0};
    char retain_str[8] = {0};
    size_t size = sizeof(mqtt_sub_topic);

    err = nvs_get_str(nvs_handle, "mqtt_sub_topic", mqtt_sub_topic, &size);
    if (err == ESP_OK && mqtt_sub_topic[0] != '\0') {
      size = sizeof(qos_str);
      if (nvs_get_str(nvs_handle, "qos", qos_str, &size) != ESP_OK) {
        strcpy(qos_str, "0");
      }

      size = sizeof(retain_str);
      if (nvs_get_str(nvs_handle, "retain", retain_str, &size) != ESP_OK) {
        strcpy(retain_str, "0");
      }

      const char *ack = "{\"code\":200,\"msg\":\"config updated\"}";
      mqtt_publish_topic(mqtt_sub_topic, ack, strlen(ack), atoi(qos_str),
                         atoi(retain_str));
    } else {
      ESP_LOGW(TAG, "未配置mqtt_sub_topic，无法发送配置成功响应");
    }
    restart_needed = true;
  }

  nvs_close(nvs_handle);
  cJSON_Delete(root);
  free(json_buf);
  if (restart_needed) {
    ESP_LOGI(TAG, "MQTT配置已更新，设备即将重启");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
  }
  return is_config;
}

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

int mqtt_publish_topic(const char *topic, const char *data, int len, int qos,
                       int retain) {
    if (!client) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return -1;
    }

    // 添加超时检查
    const int PUBLISH_TIMEOUT_MS = 1000; // 1秒超时
    int64_t start_time = esp_timer_get_time() / 1000;

    int msg_id = esp_mqtt_client_publish(client, topic, data, len, qos, retain);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish message");
        return -1;
    }

    // 检查是否超时
    if ((esp_timer_get_time() / 1000 - start_time) > PUBLISH_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Publish operation timeout");
    }

    return msg_id;
}

static void log_error_if_nonzero(const char *message, int error_code) {
  if (error_code != 0) {
    ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
  }
}


void mqtt_send_task(void *pvParameters) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  int task_mqtt_time = 1;
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));
  size_t use_mqtt, mqtt_clientid, mqtt_send, mqtt_time = 0;
  ret = nvs_get_str(nvs_handle, "use_mqtt", NULL, &use_mqtt);
  nvs_get_str(nvs_handle, "mqtt_clientid", NULL, &mqtt_clientid);
  nvs_get_str(nvs_handle, "mqtt_send", NULL, &mqtt_send);
  nvs_get_str(nvs_handle, "mqtt_time", NULL, &mqtt_time);
  switch (ret) {
  case ESP_OK:
    printf("HTTP set success");
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    printf("no HTTP set");
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_mqtt", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_clientid", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_send", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_time", ""));
    break;
  default:
    break;
  }
  if (ret != ESP_OK) {
    printf("Error getting size of 'HTTP': %s\n", esp_err_to_name(ret));
    nvs_close(nvs_handle);  // 确保关闭句柄
  } else {
    char *nvs_use_mqtt = malloc(use_mqtt);
    char *nvs_mqtt_clientid = malloc(mqtt_clientid);
    char *nvs_mqtt_send = malloc(mqtt_send);
    char *nvs_mqtt_time = malloc(mqtt_time);

    // 检查所有malloc是否成功
    if (nvs_use_mqtt == NULL || nvs_mqtt_clientid == NULL ||
        nvs_mqtt_send == NULL || nvs_mqtt_time == NULL) {
      printf("Memory allocation failed\n");
      // 释放已分配的内存
      if (nvs_use_mqtt) free(nvs_use_mqtt);
      if (nvs_mqtt_clientid) free(nvs_mqtt_clientid);
      if (nvs_mqtt_send) free(nvs_mqtt_send);
      if (nvs_mqtt_time) free(nvs_mqtt_time);
      nvs_close(nvs_handle);
    } else {
      ret = nvs_get_str(nvs_handle, "use_mqtt", nvs_use_mqtt, &use_mqtt);
      nvs_get_str(nvs_handle, "mqtt_clientid", nvs_mqtt_clientid,
                  &mqtt_clientid);
      nvs_get_str(nvs_handle, "mqtt_send", nvs_mqtt_send, &mqtt_send);
      nvs_get_str(nvs_handle, "mqtt_time", nvs_mqtt_time, &mqtt_time);

      if (ret == ESP_OK) {

        task_mqtt_time = atoi(nvs_mqtt_time);

      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
      free(nvs_use_mqtt);
      free(nvs_mqtt_clientid);
      free(nvs_mqtt_send);
      free(nvs_mqtt_time);
      nvs_close(nvs_handle);
    }
  }

  while (1) {
    vTaskDelay(1000 * task_mqtt_time / portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = event_data;
  esp_mqtt_client_handle_t client = event->client;
  int msg_id;

  ESP_LOGI(TAG, "MQTT事件处理 - 事件ID: %d", (int)event_id);

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));
  size_t use_mqtt, mqtt_clientid, mqtt_sub_topic, mqtt_pub_topic, qos, retain,
      mqtt_send, mqtt_time = 0;
  ret = nvs_get_str(nvs_handle, "use_mqtt", NULL, &use_mqtt);
  nvs_get_str(nvs_handle, "mqtt_clientid", NULL, &mqtt_clientid);
  nvs_get_str(nvs_handle, "mqtt_sub_topic", NULL, &mqtt_sub_topic);
  nvs_get_str(nvs_handle, "mqtt_pub_topic", NULL, &mqtt_pub_topic);
  nvs_get_str(nvs_handle, "qos", NULL, &qos);
  nvs_get_str(nvs_handle, "retain", NULL, &retain);
  nvs_get_str(nvs_handle, "mqtt_send", NULL, &mqtt_send);
  nvs_get_str(nvs_handle, "mqtt_time", NULL, &mqtt_time);
  switch (ret) {
  case ESP_OK:
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_mqtt", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_clientid", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_sub_topic", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_pub_topic", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "qos", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "retain", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_send", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_time", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqttconn", "0"));
    break;
  default:
    break;
  }
  if (ret != ESP_OK) {
    printf("Error getting size of 'my_key': %s\n", esp_err_to_name(ret));
  } else {
    // 分配缓冲区
    char *nvs_use_mqtt = malloc(use_mqtt);
    char *nvs_mqtt_clientid = malloc(mqtt_clientid);
    char *nvs_mqtt_sub_topic = malloc(mqtt_sub_topic);
    char *nvs_mqtt_pub_topic = malloc(mqtt_pub_topic);
    char *nvs_qos = malloc(qos);
    char *nvs_retain = malloc(retain);
    char *nvs_mqtt_send = malloc(mqtt_send);
    char *nvs_mqtt_time = malloc(mqtt_time);
    if (nvs_mqtt_clientid == NULL) {
      printf("Memory allocation failed\n");
    } else {
      // 读取字符串到分配的缓冲区
      ret = nvs_get_str(nvs_handle, "use_mqtt", nvs_use_mqtt, &use_mqtt);
      nvs_get_str(nvs_handle, "mqtt_clientid", nvs_mqtt_clientid,
                  &mqtt_clientid);
      nvs_get_str(nvs_handle, "mqtt_sub_topic", nvs_mqtt_sub_topic,
                  &mqtt_sub_topic);
      nvs_get_str(nvs_handle, "mqtt_pub_topic", nvs_mqtt_pub_topic,
                  &mqtt_pub_topic);
      nvs_get_str(nvs_handle, "qos", nvs_qos, &qos);
      nvs_get_str(nvs_handle, "retain", nvs_retain, &retain);
      nvs_get_str(nvs_handle, "mqtt_send", nvs_mqtt_send, &mqtt_send);
      nvs_get_str(nvs_handle, "mqtt_time", nvs_mqtt_time, &mqtt_time);
      if (ret == ESP_OK) {
        switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
          ESP_LOGI(TAG, "=== MQTT连接成功 ===");
          ESP_LOGI(TAG, "MQTT已连接，正在设置连接状态");

          // 🔧 立即设置内存中的连接状态
          mqtt_actually_connected = true;

          msg_id = esp_mqtt_client_subscribe(client, nvs_mqtt_pub_topic,
                                             atoi(nvs_qos));
          ESP_LOGI(TAG, "MQTT订阅成功，msg_id=%d, topic=%s", msg_id, nvs_mqtt_pub_topic);

          esp_err_t nvs_err = nvs_set_str(nvs_handle, "mqttconn", "1");
          if (nvs_err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
            ESP_LOGW(TAG, "⚠️ NVS空间不足，尝试清理并重试");
            // 尝试提交现有数据释放空间
            nvs_commit(nvs_handle);
            // 再次尝试设置
            nvs_err = nvs_set_str(nvs_handle, "mqttconn", "1");
            if (nvs_err != ESP_OK) {
              ESP_LOGE(TAG, "❌ NVS清理后仍然设置失败: %s", esp_err_to_name(nvs_err));
              // 即使NVS设置失败，MQTT仍然是连接的，我们在内存中记录状态
              ESP_LOGI(TAG, "📝 MQTT实际已连接，但NVS状态未保存");
            } else {
              ESP_LOGI(TAG, "✅ NVS清理后设置成功");
              nvs_commit(nvs_handle);
            }
          } else if (nvs_err != ESP_OK) {
            ESP_LOGE(TAG, "❌ 设置mqttconn失败: %s", esp_err_to_name(nvs_err));
            ESP_LOGI(TAG, "📝 MQTT实际已连接，但NVS状态未保存");
          } else {
            ESP_LOGI(TAG, "✅ MQTT连接状态已设置为1");
            // 立即提交更改到NVS
            esp_err_t commit_err = nvs_commit(nvs_handle);
            if (commit_err != ESP_OK) {
              ESP_LOGE(TAG, "❌ NVS提交失败: %s", esp_err_to_name(commit_err));
            } else {
              ESP_LOGI(TAG, "✅ NVS已提交，连接状态已保存");
            }
          }

          if (atoi(nvs_mqtt_send) == 2) {
            if (xMqttHandle == NULL) {
              xTaskCreate(&mqtt_send_task, "mqtt_send_task", 8192, NULL, 5,
                          &xMqttHandle);
            }
          }

          break;
        case MQTT_EVENT_DISCONNECTED:
          ESP_LOGI(TAG, "=== MQTT连接断开 ===");
          ESP_LOGI(TAG, "MQTT已断开，正在设置连接状态为0");

          // 🔧 立即设置内存中的连接状态
          mqtt_actually_connected = false;

          esp_err_t nvs_err_disconnect = nvs_set_str(nvs_handle, "mqttconn", "0");
          if (nvs_err_disconnect != ESP_OK) {
            ESP_LOGE(TAG, "❌ 设置mqttconn为0失败: %s", esp_err_to_name(nvs_err_disconnect));
          } else {
            ESP_LOGI(TAG, "✅ MQTT断开状态已设置为0");
            // 立即提交更改到NVS
            esp_err_t commit_err = nvs_commit(nvs_handle);
            if (commit_err != ESP_OK) {
              ESP_LOGE(TAG, "❌ NVS提交失败: %s", esp_err_to_name(commit_err));
            } else {
              ESP_LOGI(TAG, "✅ NVS已提交，断开状态已保存");
            }
          }
          break;

        case MQTT_EVENT_SUBSCRIBED:

          break;
        case MQTT_EVENT_UNSUBSCRIBED:

          // ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d",
          // event->msg_id);
          break;
        case MQTT_EVENT_PUBLISHED:
          // ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);

          break;
        case MQTT_EVENT_DATA:
          // 优先尝试处理配置 JSON，命中后直接返回，不做透传
          if (mqtt_apply_config_payload(event->data, event->data_len)) {
            ESP_LOGI(TAG, "MQTT配置消息处理完成，已写入NVS");
            break;
          }

          // ESP_LOGI(TAG, "MQTT_EVENT_DATA");
          printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
          printf("====================\n");
          printf("DATAHEX=\r\n");
          char topic[128] = {0};
          int topic_copy_len = event->topic_len;
          if (topic_copy_len >= (int)sizeof(topic)) {
            topic_copy_len = sizeof(topic) - 1;
          }
          if (topic_copy_len > 0 && event->topic != NULL) {
            memcpy(topic, event->topic, topic_copy_len);
            topic[topic_copy_len] = '\0';
          }

          uint8_t *u_data = malloc(event->data_len * sizeof(uint8_t));
          if (u_data == NULL) {
            printf("Memory allocation failed\n");
            return;
          }

          for (int i = 0; i < event->data_len; i++) {
            u_data[i] = event->data[i];
            printf("%02x ", u_data[i]);
          }
          printf("udata: %s\n", u_data);
          printf("event->data_len: %d\n", event->data_len);

          sx_work_mode_mqtt_handler_t handler =
              sx_work_mode_get_mqtt_handler();
          if (handler != NULL) {
            esp_err_t handler_err = handler(topic, u_data, event->data_len);
            if (handler_err != ESP_OK) {
              ESP_LOGW(TAG, "work mode MQTT handler failed: %s",
                       esp_err_to_name(handler_err));
            }
          } else {
            // 创建一个数据副本并发送到串口，然后释放原始数据
            uint8_t *uart_data_copy =
                malloc(event->data_len * sizeof(uint8_t));
            if (uart_data_copy != NULL) {
              memcpy(uart_data_copy, u_data, event->data_len);

              // 1. 发送数据到串口
              tx_tasks(uart_data_copy, event->data_len);  // tx_tasks会自行释放内存

              // 注意：不再将发送数据回环显示，只显示串口设备的真实响应
              // 真实的串口接收数据会在 sx_async_uart.c 的 rx_task 中处理并显示

              ESP_LOGI(TAG, "MQTT数据已发送到串口");
            } else {
              ESP_LOGE(TAG, "无法为串口数据分配内存");
            }
          }

          free(u_data);
          printf("====================\n");

          break;
        case MQTT_EVENT_ERROR:
          ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
          if (event->error_handle->error_type ==
              MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls",
                                 event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack",
                                 event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",
                                 event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)",
                     strerror(event->error_handle->esp_transport_sock_errno));
          }
          break;
        default:
          ESP_LOGI(TAG, "Other event id:%d", event->event_id);
          break;
        }
      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
      free(nvs_use_mqtt); // 释放缓冲区
      free(nvs_mqtt_clientid);
      free(nvs_mqtt_sub_topic);
      free(nvs_mqtt_pub_topic);
      free(nvs_qos);
      free(nvs_retain);
      free(nvs_mqtt_send);
      free(nvs_mqtt_time);
    }
  }

  nvs_close(nvs_handle);
}

bool mqtt_app_start(void) {
  if (client != NULL) {
    ESP_LOGI(TAG, "MQTT client already initialized");
    return true;
  }

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "NVS init failed before MQTT start: %s", esp_err_to_name(ret));
    return false;
  }

  nvs_handle_t nvs_handle;
  ret = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Open NVS failed before MQTT start: %s", esp_err_to_name(ret));
    return false;
  }

  bool started = false;

  size_t use_mqtt, mqtt_type, mqtt_server, mqtt_port, mqtt_username,
      mqtt_password, mqtt_clientid, mqtt_sub_topic, mqtt_pub_topic, qos,
      retain = 0;
  ret = nvs_get_str(nvs_handle, "use_mqtt", NULL, &use_mqtt);
  nvs_get_str(nvs_handle, "mqtt_type", NULL, &mqtt_type);
  nvs_get_str(nvs_handle, "mqtt_server", NULL, &mqtt_server);
  nvs_get_str(nvs_handle, "mqtt_port", NULL, &mqtt_port);
  nvs_get_str(nvs_handle, "mqtt_username", NULL, &mqtt_username);
  nvs_get_str(nvs_handle, "mqtt_password", NULL, &mqtt_password);
  nvs_get_str(nvs_handle, "mqtt_clientid", NULL, &mqtt_clientid);
  nvs_get_str(nvs_handle, "mqtt_sub_topic", NULL, &mqtt_sub_topic);
  nvs_get_str(nvs_handle, "mqtt_pub_topic", NULL, &mqtt_pub_topic);
  nvs_get_str(nvs_handle, "qos", NULL, &qos);
  nvs_get_str(nvs_handle, "retain", NULL, &retain);
  if (ret != ESP_OK) {
    printf("Error getting size of 'my_key': %s\n", esp_err_to_name(ret));
  } else {
    // 分配缓冲区
    char *nvs_use_mqtt = malloc(use_mqtt);
    char *nvs_mqtt_type = malloc(mqtt_type);
    char *nvs_mqtt_server = malloc(mqtt_server);
    char *nvs_mqtt_port = malloc(mqtt_port);
    char *nvs_mqtt_username = malloc(mqtt_username);
    char *nvs_mqtt_password = malloc(mqtt_password);
    char *nvs_mqtt_clientid = malloc(mqtt_clientid);
    char *nvs_mqtt_sub_topic = malloc(mqtt_sub_topic);
    char *nvs_mqtt_pub_topic = malloc(mqtt_pub_topic);
    char *nvs_qos = malloc(qos);
    char *nvs_retain = malloc(retain);
    if (nvs_use_mqtt == NULL || nvs_mqtt_type == NULL ||
        nvs_mqtt_server == NULL || nvs_mqtt_port == NULL ||
        nvs_mqtt_username == NULL || nvs_mqtt_password == NULL ||
        nvs_mqtt_clientid == NULL || nvs_mqtt_sub_topic == NULL ||
        nvs_mqtt_pub_topic == NULL || nvs_qos == NULL ||
        nvs_retain == NULL) {
      printf("Memory allocation failed\n");
      free(nvs_use_mqtt);
      free(nvs_mqtt_type);
      free(nvs_mqtt_server);
      free(nvs_mqtt_port);
      free(nvs_mqtt_username);
      free(nvs_mqtt_password);
      free(nvs_mqtt_clientid);
      free(nvs_mqtt_sub_topic);
      free(nvs_mqtt_pub_topic);
      free(nvs_qos);
      free(nvs_retain);
    } else {
      ret = nvs_get_str(nvs_handle, "use_mqtt", nvs_use_mqtt, &use_mqtt);
      nvs_get_str(nvs_handle, "mqtt_type", nvs_mqtt_type, &mqtt_type);
      nvs_get_str(nvs_handle, "mqtt_server", nvs_mqtt_server, &mqtt_server);
      nvs_get_str(nvs_handle, "mqtt_port", nvs_mqtt_port, &mqtt_port);
      nvs_get_str(nvs_handle, "mqtt_username", nvs_mqtt_username,
                  &mqtt_username);
      nvs_get_str(nvs_handle, "mqtt_password", nvs_mqtt_password,
                  &mqtt_password);
      nvs_get_str(nvs_handle, "mqtt_clientid", nvs_mqtt_clientid,
                  &mqtt_clientid);
      nvs_get_str(nvs_handle, "mqtt_sub_topic", nvs_mqtt_sub_topic,
                  &mqtt_sub_topic);
      nvs_get_str(nvs_handle, "mqtt_pub_topic", nvs_mqtt_pub_topic,
                  &mqtt_pub_topic);
      nvs_get_str(nvs_handle, "qos", nvs_qos, &qos);
      nvs_get_str(nvs_handle, "retain", nvs_retain, &retain);
      if (ret == ESP_OK) {
        char mqtturi[256];
        snprintf(mqtturi, sizeof(mqtturi), "%s%s%s%s%s%s%s%s", "mqtt://",
                 nvs_mqtt_username, ":", nvs_mqtt_password, "@",
                 nvs_mqtt_server, ":", nvs_mqtt_port);
        printf("Concatenated String: %s\n", mqtturi);
        esp_mqtt_client_config_t mqtt_cfg = {.broker.address.uri = mqtturi,
                                             .credentials.client_id =
                                                 nvs_mqtt_clientid};
        client = esp_mqtt_client_init(&mqtt_cfg);
        if (client == NULL) {
          ESP_LOGE(TAG, "esp_mqtt_client_init failed, free_heap=%u",
                   (unsigned)esp_get_free_heap_size());
        } else {
          ret = esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                               mqtt_event_handler, NULL);
          if (ret != ESP_OK) {
            ESP_LOGE(TAG, "MQTT register event failed: %s", esp_err_to_name(ret));
            esp_mqtt_client_destroy(client);
            client = NULL;
          } else {
            ret = esp_mqtt_client_start(client);
            if (ret == ESP_OK) {
              started = true;
            } else {
              ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s, free_heap=%u",
                       esp_err_to_name(ret),
                       (unsigned)esp_get_free_heap_size());
              esp_mqtt_client_destroy(client);
              client = NULL;
            }
          }
        }
      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
      free(nvs_use_mqtt);
      free(nvs_mqtt_type);
      free(nvs_mqtt_server);
      free(nvs_mqtt_port);
      free(nvs_mqtt_username);
      free(nvs_mqtt_password);
      free(nvs_mqtt_clientid);
      free(nvs_mqtt_sub_topic);
      free(nvs_mqtt_pub_topic);
      free(nvs_qos);
      free(nvs_retain);
    }
  }
  nvs_close(nvs_handle);
  return started;
}

void mqtt_app_stop(void) {
  if (xMqttHandle != NULL) {
    vTaskDelete(xMqttHandle);
    xMqttHandle = NULL;
  }

  if (client != NULL) {
    ESP_LOGI(TAG, "Stopping MQTT client");
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = NULL;
  }

  mqtt_actually_connected = false;

  nvs_handle_t nvs_handle;
  if (nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle) == ESP_OK) {
    nvs_set_str(nvs_handle, "mqttconn", "0");
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
  }
}

// 检查MQTT客户端实际连接状态
bool is_mqtt_actually_connected(void) {
  if (client == NULL) {
    ESP_LOGI(TAG, "MQTT客户端未初始化");
    return false;
  }

  // 返回内存中记录的连接状态
  ESP_LOGI(TAG, "MQTT实际连接状态: %s", mqtt_actually_connected ? "已连接" : "未连接");
  return mqtt_actually_connected;
}

// 🔧 添加获取MQTT连接状态的函数（供Web服务器调用）
bool get_mqtt_connection_status(void) {
  return mqtt_actually_connected;
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
