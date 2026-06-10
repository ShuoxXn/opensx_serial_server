#pragma once

#include "esp_err.h"
#include "nvs.h"
#include <stddef.h>
#include <stdint.h>

typedef struct cJSON cJSON;

esp_err_t mode_protocol_test_start(void);
esp_err_t mode_protocol_test_stop(void);
esp_err_t mode_protocol_test_load_config(nvs_handle_t handle, cJSON *response);
esp_err_t mode_protocol_test_save_config(nvs_handle_t handle,
                                         const cJSON *request);
esp_err_t mode_protocol_test_on_tcp_data(const uint8_t *data, size_t len);
esp_err_t mode_protocol_test_on_mqtt_data(const char *topic,
                                          const uint8_t *data, size_t len);
