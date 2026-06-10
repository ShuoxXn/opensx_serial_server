#include "sx_work_mode.h"

#include "cJSON.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "WORK_MODE";

#define SX_WORK_MODE_MAX 8

static const sx_work_mode_t *s_modes[SX_WORK_MODE_MAX] = {0};
static size_t s_mode_count = 0;
static const sx_work_mode_t *s_current_mode = NULL;

static const sx_work_mode_t *find_mode(const char *name) {
  if (name == NULL || name[0] == '\0') {
    return NULL;
  }

  for (size_t i = 0; i < s_mode_count; i++) {
    if (s_modes[i] != NULL && s_modes[i]->name != NULL &&
        strcmp(s_modes[i]->name, name) == 0) {
      return s_modes[i];
    }
  }

  return NULL;
}

esp_err_t sx_work_mode_register(const sx_work_mode_t *mode) {
  if (mode == NULL || mode->name == NULL || mode->name[0] == '\0' ||
      mode->start == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  const sx_work_mode_t *existing = find_mode(mode->name);
  if (existing != NULL) {
    ESP_LOGD(TAG, "work mode already registered: %s", mode->name);
    return ESP_OK;
  }

  if (s_mode_count >= SX_WORK_MODE_MAX) {
    ESP_LOGE(TAG, "work mode registry full");
    return ESP_ERR_NO_MEM;
  }

  s_modes[s_mode_count++] = mode;
  ESP_LOGI(TAG, "registered work mode: %s", mode->name);
  return ESP_OK;
}

esp_err_t sx_work_mode_start_by_name(const char *name) {
  sx_work_mode_register_builtin_modes();

  const sx_work_mode_t *mode = find_mode(name);
  if (mode == NULL) {
    ESP_LOGW(TAG, "unknown work mode: %s", name ? name : "(null)");
    return ESP_ERR_NOT_FOUND;
  }

  if (s_current_mode != NULL && s_current_mode->name != NULL &&
      strcmp(s_current_mode->name, mode->name) == 0) {
    ESP_LOGW(TAG, "work mode already active: %s", mode->name);
    return ESP_OK;
  }

  if (s_current_mode != NULL) {
    sx_work_mode_stop_current();
  }

  ESP_LOGI(TAG, "starting work mode: %s", mode->name);
  esp_err_t err = mode->start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "failed to start work mode %s: %s", mode->name,
             esp_err_to_name(err));
    return err;
  }

  s_current_mode = mode;

  if (mode->after_start != NULL) {
    err = mode->after_start();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "work mode after_start failed for %s: %s", mode->name,
               esp_err_to_name(err));
    }
  }

  return ESP_OK;
}

esp_err_t sx_work_mode_stop_current(void) {
  if (s_current_mode == NULL) {
    return ESP_OK;
  }

  const sx_work_mode_t *mode = s_current_mode;
  s_current_mode = NULL;

  if (mode->stop == NULL) {
    return ESP_OK;
  }

  ESP_LOGI(TAG, "stopping work mode: %s", mode->name);
  return mode->stop();
}

sx_work_mode_uart_handler_t sx_work_mode_get_uart_handler(void) {
  if (s_current_mode == NULL) {
    return NULL;
  }

  return s_current_mode->on_uart_data;
}

sx_work_mode_tcp_handler_t sx_work_mode_get_tcp_handler(void) {
  if (s_current_mode == NULL) {
    return NULL;
  }

  return s_current_mode->on_tcp_data;
}

sx_work_mode_mqtt_handler_t sx_work_mode_get_mqtt_handler(void) {
  if (s_current_mode == NULL) {
    return NULL;
  }

  return s_current_mode->on_mqtt_data;
}

bool sx_work_mode_is_valid(const char *name) {
  sx_work_mode_register_builtin_modes();
  return find_mode(name) != NULL;
}

const sx_work_mode_t *sx_work_mode_get_current(void) {
  return s_current_mode;
}

const sx_work_mode_t *sx_work_mode_find_by_name(const char *name) {
  sx_work_mode_register_builtin_modes();
  return find_mode(name);
}

esp_err_t sx_work_mode_load_config(const char *name, nvs_handle_t handle,
                                   cJSON *response) {
  if (response == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  const sx_work_mode_t *mode = sx_work_mode_find_by_name(name);
  if (mode == NULL) {
    return ESP_ERR_NOT_FOUND;
  }

  if (mode->config != NULL && mode->config->load != NULL) {
    return mode->config->load(handle, response);
  }

  cJSON *mode_config = cJSON_CreateObject();
  if (mode_config == NULL) {
    return ESP_ERR_NO_MEM;
  }
  cJSON_AddItemToObject(response, "mode_config", mode_config);
  return ESP_OK;
}

esp_err_t sx_work_mode_save_config(const char *name, nvs_handle_t handle,
                                   const cJSON *request) {
  if (request == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  const sx_work_mode_t *mode = sx_work_mode_find_by_name(name);
  if (mode == NULL) {
    return ESP_ERR_NOT_FOUND;
  }

  if (mode->config == NULL || mode->config->save == NULL) {
    return ESP_OK;
  }

  return mode->config->save(handle, request);
}

esp_err_t sx_work_mode_append_available_modes(cJSON *array) {
  if (array == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t err = sx_work_mode_register_builtin_modes();
  if (err != ESP_OK) {
    return err;
  }

  for (size_t i = 0; i < s_mode_count; i++) {
    const sx_work_mode_t *mode = s_modes[i];
    if (mode == NULL || mode->name == NULL) {
      continue;
    }

    cJSON *item = cJSON_CreateObject();
    if (item == NULL) {
      return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(item, "name", mode->name);
    cJSON_AddStringToObject(item, "label",
                            mode->label != NULL ? mode->label : mode->name);
    cJSON_AddItemToArray(array, item);
  }

  return ESP_OK;
}
