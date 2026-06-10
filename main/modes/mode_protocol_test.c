/*
 * 工作模式：协议测试
 *
 * 用于测试协议收发链路：
 * - TCP 收到什么就发回什么。
 * - MQTT 收到什么就发布回配置的 mqtt_sub_topic。
 * - HTTP 使用全局 HTTP 协议配置，每 1 秒 POST 一次 "test"。
 */

#include "mode_protocol_test.h"

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sx_protocol_api.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MODE_PROTOCOL_TEST";
static const uint8_t HTTP_TEST_PAYLOAD[] = "test";
static const TickType_t HTTP_TEST_INTERVAL_TICKS = pdMS_TO_TICKS(1000);

static TaskHandle_t s_http_test_task = NULL;
static volatile bool s_http_test_stop = false;

static bool nvs_read_string(nvs_handle_t handle, const char *key, char *buf,
                            size_t buf_len) {
  if (buf == NULL || buf_len == 0) {
    return false;
  }
  buf[0] = '\0';
  size_t len = buf_len;
  if (nvs_get_str(handle, key, buf, &len) != ESP_OK) {
    return false;
  }
  buf[buf_len - 1] = '\0';
  return true;
}

static int nvs_read_int(nvs_handle_t handle, const char *key, int fallback) {
  char value[16] = {0};
  if (!nvs_read_string(handle, key, value, sizeof(value))) {
    return fallback;
  }
  return atoi(value);
}

static void http_test_task(void *arg) {
  (void)arg;

  while (!s_http_test_stop) {
    esp_err_t err = sx_protocol_http_post_configured(
        HTTP_TEST_PAYLOAD, sizeof(HTTP_TEST_PAYLOAD) - 1, "text/plain");
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "HTTP test POST failed: %s", esp_err_to_name(err));
    }

    for (int i = 0; i < 10 && !s_http_test_stop; i++) {
      vTaskDelay(HTTP_TEST_INTERVAL_TICKS / 10);
    }
  }

  s_http_test_task = NULL;
  vTaskDelete(NULL);
}

esp_err_t mode_protocol_test_start(void) {
  ESP_LOGI(TAG, "protocol test mode started");
  if (s_http_test_task == NULL) {
    s_http_test_stop = false;
    BaseType_t ok =
        xTaskCreate(http_test_task, "protocol_http_test", 4096, NULL, 5,
                    &s_http_test_task);
    if (ok != pdPASS) {
      s_http_test_task = NULL;
      ESP_LOGE(TAG, "failed to create HTTP test task");
      return ESP_ERR_NO_MEM;
    }
  }
  return ESP_OK;
}

esp_err_t mode_protocol_test_stop(void) {
  ESP_LOGI(TAG, "protocol test mode stopped");
  s_http_test_stop = true;
  for (int i = 0; i < 60 && s_http_test_task != NULL; i++) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return ESP_OK;
}

esp_err_t mode_protocol_test_load_config(nvs_handle_t handle, cJSON *response) {
  if (response == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  char mqtt_echo_topic[128] = {0};
  nvs_read_string(handle, "mqtt_sub_topic", mqtt_echo_topic,
                  sizeof(mqtt_echo_topic));

  cJSON *mode_config = cJSON_CreateObject();
  if (mode_config == NULL) {
    return ESP_ERR_NO_MEM;
  }

  cJSON_AddBoolToObject(mode_config, "has_private_config", false);
  cJSON_AddStringToObject(mode_config, "mqtt_echo_topic", mqtt_echo_topic);
  cJSON_AddStringToObject(mode_config, "tcp_echo", "enabled");
  cJSON_AddStringToObject(mode_config, "http_test_payload", "test");
  cJSON_AddNumberToObject(mode_config, "http_test_interval_ms", 1000);
  cJSON_AddItemToObject(response, "mode_config", mode_config);
  return ESP_OK;
}

esp_err_t mode_protocol_test_save_config(nvs_handle_t handle,
                                         const cJSON *request) {
  (void)handle;
  if (request == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  const cJSON *mode_config = cJSON_GetObjectItem(request, "mode_config");
  if (mode_config != NULL && !cJSON_IsObject(mode_config)) {
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

esp_err_t mode_protocol_test_on_tcp_data(const uint8_t *data, size_t len) {
  ESP_LOGI(TAG, "TCP echo %u bytes", (unsigned)len);
  return sx_protocol_tcp_send(SX_PROTOCOL_TCP_AUTO, data, len);
}

esp_err_t mode_protocol_test_on_mqtt_data(const char *topic,
                                          const uint8_t *data, size_t len) {
  nvs_handle_t handle;
  char echo_topic[128] = {0};
  int qos = 0;
  bool retain = false;

  if (nvs_open("nvs_namespace", NVS_READONLY, &handle) == ESP_OK) {
    nvs_read_string(handle, "mqtt_sub_topic", echo_topic, sizeof(echo_topic));
    qos = nvs_read_int(handle, "qos", 0);
    retain = nvs_read_int(handle, "retain", 0) == 1;
    nvs_close(handle);
  }

  if (echo_topic[0] == '\0' && topic != NULL) {
    snprintf(echo_topic, sizeof(echo_topic), "%s", topic);
  }
  if (echo_topic[0] == '\0') {
    ESP_LOGW(TAG, "MQTT echo topic is empty");
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(TAG, "MQTT echo %u bytes to %s", (unsigned)len, echo_topic);
  return sx_protocol_mqtt_publish(echo_topic, data, len, qos, retain);
}
