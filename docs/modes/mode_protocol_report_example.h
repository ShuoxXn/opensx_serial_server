#pragma once

#include "esp_err.h"
#include "nvs.h"
#include <stddef.h>
#include <stdint.h>

typedef struct cJSON cJSON;

esp_err_t mode_protocol_report_example_start(void);
esp_err_t mode_protocol_report_example_stop(void);
void mode_protocol_report_example_on_uart_data(const uint8_t *data, size_t len);
esp_err_t mode_protocol_report_example_load_config(nvs_handle_t handle,
                                                   cJSON *response);
esp_err_t mode_protocol_report_example_save_config(nvs_handle_t handle,
                                                   const cJSON *request);
