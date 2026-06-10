/*
 * 工作模式：纯 TCP 透传测试模式
 *
 * 该模式只负责把 UART 数据转发到当前 TCP 通道。
 * TCP 收到的数据由 TCP Server/Client 协议层直接写回 UART。
 */

#include "mode_tcp_transparent.h"

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sx_protocol_api.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MODE_TCP_TRANSPARENT";

#define TCP_TRANSPARENT_QUEUE_SIZE 10

typedef struct {
  uint8_t *data;
  size_t len;
} tcp_transparent_msg_t;

static TaskHandle_t s_task_handle = NULL;
static QueueHandle_t s_queue = NULL;

esp_err_t mode_tcp_transparent_load_config(nvs_handle_t handle,
                                           cJSON *response) {
  (void)handle;
  if (response == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  cJSON *mode_config = cJSON_CreateObject();
  if (mode_config == NULL) {
    return ESP_ERR_NO_MEM;
  }

  cJSON_AddStringToObject(mode_config, "transport", "tcp");
  cJSON_AddBoolToObject(mode_config, "has_private_config", false);
  cJSON_AddItemToObject(response, "mode_config", mode_config);
  return ESP_OK;
}

esp_err_t mode_tcp_transparent_save_config(nvs_handle_t handle,
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

void mode_tcp_transparent_on_uart_data(const uint8_t *data, size_t len) {
  if (s_queue == NULL || data == NULL || len == 0) {
    return;
  }

  uint8_t *data_copy = malloc(len);
  if (data_copy == NULL) {
    ESP_LOGE(TAG, "failed to allocate UART payload");
    return;
  }

  memcpy(data_copy, data, len);

  tcp_transparent_msg_t msg = {
      .data = data_copy,
      .len = len,
  };

  if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
    ESP_LOGW(TAG, "queue full, dropping UART payload");
    free(data_copy);
  }
}

static void tcp_transparent_task(void *arg) {
  ESP_LOGI(TAG, "TCP transparent mode started");

  while (true) {
    tcp_transparent_msg_t msg = {0};
    if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    esp_err_t err =
        sx_protocol_tcp_send(SX_PROTOCOL_TCP_AUTO, msg.data, msg.len);

    if (err != ESP_OK) {
      ESP_LOGW(TAG, "failed to forward UART payload to TCP: %s",
               esp_err_to_name(err));
    }

    free(msg.data);
  }
}

esp_err_t mode_tcp_transparent_start(void) {
  if (s_task_handle != NULL) {
    ESP_LOGW(TAG, "TCP transparent mode already running");
    return ESP_OK;
  }

  s_queue = xQueueCreate(TCP_TRANSPARENT_QUEUE_SIZE,
                         sizeof(tcp_transparent_msg_t));
  if (s_queue == NULL) {
    ESP_LOGE(TAG, "failed to create queue");
    return ESP_FAIL;
  }

  BaseType_t ret = xTaskCreate(tcp_transparent_task, "mode_tcp_transparent",
                               4096, NULL, 5, &s_task_handle);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "failed to create task");
    vQueueDelete(s_queue);
    s_queue = NULL;
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t mode_tcp_transparent_stop(void) {
  if (s_task_handle != NULL) {
    vTaskDelete(s_task_handle);
    s_task_handle = NULL;
  }

  if (s_queue != NULL) {
    tcp_transparent_msg_t msg = {0};
    while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
      free(msg.data);
    }
    vQueueDelete(s_queue);
    s_queue = NULL;
  }

  return ESP_OK;
}
