/*
 * Example work mode for OpenShuoxin Serial Firmware.
 *
 * Copy this file and mode_protocol_report_example.h to main/modes/ before use.
 * This example shows how a mode can:
 * - receive UART data through on_uart_data
 * - process data in its own task
 * - call sx_protocol_api.h for TCP, MQTT, HTTP, and UDP
 * - save and load private mode_config fields
 */

#include "mode_protocol_report_example.h"

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sx_protocol_api.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MODE_PROTOCOL_REPORT";

#define MODE_PROTOCOL_REPORT_QUEUE_SIZE 10
#define MODE_PROTOCOL_REPORT_TASK_STACK 4096

#define NVS_KEY_USE_MQTT "protocol_report_use_mqtt"
#define NVS_KEY_MQTT_TOPIC "protocol_report_mqtt_topic"
#define NVS_KEY_USE_HTTP "protocol_report_use_http"
#define NVS_KEY_USE_UDP "protocol_report_use_udp"
#define NVS_KEY_UDP_HOST "protocol_report_udp_host"
#define NVS_KEY_UDP_PORT "protocol_report_udp_port"

typedef struct {
  uint8_t *data;
  size_t len;
} protocol_report_msg_t;

typedef struct {
  bool use_mqtt;
  char mqtt_topic[128];
  bool use_http;
  bool use_udp;
  char udp_host[64];
  uint16_t udp_port;
} protocol_report_config_t;

static TaskHandle_t s_task = NULL;
static QueueHandle_t s_queue = NULL;

static bool nvs_read_string(nvs_handle_t handle, const char *key, char *buf,
                            size_t buf_len, const char *fallback) {
  if (buf == NULL || buf_len == 0) {
    return false;
  }
  buf[0] = '\0';

  size_t len = buf_len;
  if (nvs_get_str(handle, key, buf, &len) == ESP_OK) {
    buf[buf_len - 1] = '\0';
    return true;
  }

  if (fallback != NULL) {
    snprintf(buf, buf_len, "%s", fallback);
  }
  return false;
}

static bool nvs_read_bool(nvs_handle_t handle, const char *key,
                          bool fallback) {
  char value[8] = {0};
  nvs_read_string(handle, key, value, sizeof(value), fallback ? "1" : "0");
  return atoi(value) == 1;
}

static uint16_t nvs_read_u16(nvs_handle_t handle, const char *key,
                             uint16_t fallback) {
  char value[16] = {0};
  char fallback_text[16];
  snprintf(fallback_text, sizeof(fallback_text), "%u", (unsigned)fallback);
  nvs_read_string(handle, key, value, sizeof(value), fallback_text);
  int parsed = atoi(value);
  if (parsed <= 0 || parsed > 65535) {
    return fallback;
  }
  return (uint16_t)parsed;
}

static void load_config_from_nvs(nvs_handle_t handle,
                                 protocol_report_config_t *config) {
  memset(config, 0, sizeof(*config));
  config->use_mqtt = nvs_read_bool(handle, NVS_KEY_USE_MQTT, false);
  nvs_read_string(handle, NVS_KEY_MQTT_TOPIC, config->mqtt_topic,
                  sizeof(config->mqtt_topic), "/device/data");
  config->use_http = nvs_read_bool(handle, NVS_KEY_USE_HTTP, false);
  config->use_udp = nvs_read_bool(handle, NVS_KEY_USE_UDP, false);
  nvs_read_string(handle, NVS_KEY_UDP_HOST, config->udp_host,
                  sizeof(config->udp_host), "192.168.1.100");
  config->udp_port = nvs_read_u16(handle, NVS_KEY_UDP_PORT, 6000);
}

static esp_err_t save_bool(nvs_handle_t handle, const char *key,
                           const cJSON *value) {
  if (value == NULL) {
    return ESP_OK;
  }
  if (!cJSON_IsBool(value)) {
    return ESP_ERR_INVALID_ARG;
  }
  return nvs_set_str(handle, key, cJSON_IsTrue(value) ? "1" : "0");
}

static esp_err_t save_string(nvs_handle_t handle, const char *key,
                             const cJSON *value) {
  if (value == NULL) {
    return ESP_OK;
  }
  if (!cJSON_IsString(value) || value->valuestring == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  return nvs_set_str(handle, key, value->valuestring);
}

static esp_err_t save_u16(nvs_handle_t handle, const char *key,
                          const cJSON *value) {
  if (value == NULL) {
    return ESP_OK;
  }
  if (!cJSON_IsNumber(value) || value->valueint <= 0 ||
      value->valueint > 65535) {
    return ESP_ERR_INVALID_ARG;
  }

  char text[16];
  snprintf(text, sizeof(text), "%d", value->valueint);
  return nvs_set_str(handle, key, text);
}

esp_err_t mode_protocol_report_example_load_config(nvs_handle_t handle,
                                                   cJSON *response) {
  if (response == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  protocol_report_config_t config;
  load_config_from_nvs(handle, &config);

  cJSON *mode_config = cJSON_CreateObject();
  if (mode_config == NULL) {
    return ESP_ERR_NO_MEM;
  }

  cJSON_AddBoolToObject(mode_config, "use_mqtt", config.use_mqtt);
  cJSON_AddStringToObject(mode_config, "mqtt_topic", config.mqtt_topic);
  cJSON_AddBoolToObject(mode_config, "use_http", config.use_http);
  cJSON_AddBoolToObject(mode_config, "use_udp", config.use_udp);
  cJSON_AddStringToObject(mode_config, "udp_host", config.udp_host);
  cJSON_AddNumberToObject(mode_config, "udp_port", config.udp_port);
  cJSON_AddItemToObject(response, "mode_config", mode_config);
  return ESP_OK;
}

esp_err_t mode_protocol_report_example_save_config(nvs_handle_t handle,
                                                   const cJSON *request) {
  if (request == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  const cJSON *mode_config = cJSON_GetObjectItem(request, "mode_config");
  if (mode_config == NULL) {
    return ESP_OK;
  }
  if (!cJSON_IsObject(mode_config)) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t err;
  err = save_bool(handle, NVS_KEY_USE_MQTT,
                  cJSON_GetObjectItem(mode_config, "use_mqtt"));
  if (err != ESP_OK) {
    return err;
  }
  err = save_string(handle, NVS_KEY_MQTT_TOPIC,
                    cJSON_GetObjectItem(mode_config, "mqtt_topic"));
  if (err != ESP_OK) {
    return err;
  }
  err = save_bool(handle, NVS_KEY_USE_HTTP,
                  cJSON_GetObjectItem(mode_config, "use_http"));
  if (err != ESP_OK) {
    return err;
  }
  err = save_bool(handle, NVS_KEY_USE_UDP,
                  cJSON_GetObjectItem(mode_config, "use_udp"));
  if (err != ESP_OK) {
    return err;
  }
  err = save_string(handle, NVS_KEY_UDP_HOST,
                    cJSON_GetObjectItem(mode_config, "udp_host"));
  if (err != ESP_OK) {
    return err;
  }
  return save_u16(handle, NVS_KEY_UDP_PORT,
                  cJSON_GetObjectItem(mode_config, "udp_port"));
}

void mode_protocol_report_example_on_uart_data(const uint8_t *data,
                                               size_t len) {
  if (s_queue == NULL || data == NULL || len == 0) {
    return;
  }

  uint8_t *copy = malloc(len);
  if (copy == NULL) {
    ESP_LOGE(TAG, "failed to allocate UART payload");
    return;
  }
  memcpy(copy, data, len);

  protocol_report_msg_t msg = {
      .data = copy,
      .len = len,
  };
  if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
    ESP_LOGW(TAG, "queue full, dropping UART payload");
    free(copy);
  }
}

static void protocol_report_task(void *arg) {
  while (true) {
    protocol_report_msg_t msg = {0};
    if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    nvs_handle_t handle;
    protocol_report_config_t config;
    if (nvs_open("nvs_namespace", NVS_READONLY, &handle) == ESP_OK) {
      load_config_from_nvs(handle, &config);
      nvs_close(handle);
    } else {
      memset(&config, 0, sizeof(config));
    }

    esp_err_t err =
        sx_protocol_tcp_send(SX_PROTOCOL_TCP_AUTO, msg.data, msg.len);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "TCP send failed: %s", esp_err_to_name(err));
    }

    if (config.use_mqtt) {
      err = sx_protocol_mqtt_publish(config.mqtt_topic, msg.data, msg.len, 0,
                                     false);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT publish failed: %s", esp_err_to_name(err));
      }
    }

    if (config.use_http) {
      err = sx_protocol_http_post_configured(msg.data, msg.len,
                                             "application/octet-stream");
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP post failed: %s", esp_err_to_name(err));
      }
    }

    if (config.use_udp) {
      err = sx_protocol_udp_send_to(config.udp_host, config.udp_port, msg.data,
                                    msg.len);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "UDP send failed: %s", esp_err_to_name(err));
      }
    }

    free(msg.data);
  }
}

esp_err_t mode_protocol_report_example_start(void) {
  if (s_task != NULL) {
    return ESP_OK;
  }

  s_queue =
      xQueueCreate(MODE_PROTOCOL_REPORT_QUEUE_SIZE,
                   sizeof(protocol_report_msg_t));
  if (s_queue == NULL) {
    return ESP_FAIL;
  }

  BaseType_t ret = xTaskCreate(protocol_report_task, "protocol_report",
                               MODE_PROTOCOL_REPORT_TASK_STACK, NULL, 5,
                               &s_task);
  if (ret != pdPASS) {
    vQueueDelete(s_queue);
    s_queue = NULL;
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t mode_protocol_report_example_stop(void) {
  if (s_task != NULL) {
    vTaskDelete(s_task);
    s_task = NULL;
  }

  if (s_queue != NULL) {
    protocol_report_msg_t msg = {0};
    while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
      free(msg.data);
    }
    vQueueDelete(s_queue);
    s_queue = NULL;
  }

  return ESP_OK;
}
