#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "nvs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct cJSON cJSON;

typedef void (*sx_work_mode_uart_handler_t)(const uint8_t *data, size_t len);
typedef esp_err_t (*sx_work_mode_tcp_handler_t)(const uint8_t *data,
                                                size_t len);
typedef esp_err_t (*sx_work_mode_mqtt_handler_t)(const char *topic,
                                                 const uint8_t *data,
                                                 size_t len);
typedef esp_err_t (*sx_work_mode_lifecycle_fn_t)(void);
typedef esp_err_t (*sx_work_mode_config_load_fn_t)(nvs_handle_t handle,
                                                   cJSON *response);
typedef esp_err_t (*sx_work_mode_config_save_fn_t)(nvs_handle_t handle,
                                                   const cJSON *request);

typedef struct {
  sx_work_mode_config_load_fn_t load;
  sx_work_mode_config_save_fn_t save;
} sx_work_mode_config_ops_t;

typedef struct {
  const char *name;
  const char *label;
  sx_work_mode_lifecycle_fn_t start;
  sx_work_mode_lifecycle_fn_t stop;
  sx_work_mode_lifecycle_fn_t after_start;
  sx_work_mode_uart_handler_t on_uart_data;
  sx_work_mode_tcp_handler_t on_tcp_data;
  sx_work_mode_mqtt_handler_t on_mqtt_data;
  const sx_work_mode_config_ops_t *config;
} sx_work_mode_t;

esp_err_t sx_work_mode_register(const sx_work_mode_t *mode);
esp_err_t sx_work_mode_register_builtin_modes(void);
esp_err_t sx_work_mode_start_by_name(const char *name);
esp_err_t sx_work_mode_stop_current(void);
sx_work_mode_uart_handler_t sx_work_mode_get_uart_handler(void);
sx_work_mode_tcp_handler_t sx_work_mode_get_tcp_handler(void);
sx_work_mode_mqtt_handler_t sx_work_mode_get_mqtt_handler(void);
bool sx_work_mode_is_valid(const char *name);
const sx_work_mode_t *sx_work_mode_get_current(void);
const sx_work_mode_t *sx_work_mode_find_by_name(const char *name);
esp_err_t sx_work_mode_load_config(const char *name, nvs_handle_t handle,
                                   cJSON *response);
esp_err_t sx_work_mode_save_config(const char *name, nvs_handle_t handle,
                                   const cJSON *request);
esp_err_t sx_work_mode_append_available_modes(cJSON *array);

#ifdef __cplusplus
}
#endif
