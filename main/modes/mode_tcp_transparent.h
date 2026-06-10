/*
 * 工作模式：纯 TCP 透传测试模式
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "nvs.h"
#include <stddef.h>
#include <stdint.h>

typedef struct cJSON cJSON;

esp_err_t mode_tcp_transparent_start(void);
esp_err_t mode_tcp_transparent_stop(void);
void mode_tcp_transparent_on_uart_data(const uint8_t *data, size_t len);
esp_err_t mode_tcp_transparent_load_config(nvs_handle_t handle,
                                           cJSON *response);
esp_err_t mode_tcp_transparent_save_config(nvs_handle_t handle,
                                           const cJSON *request);

#ifdef __cplusplus
}
#endif
