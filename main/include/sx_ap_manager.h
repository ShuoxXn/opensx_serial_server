#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t sx_ap_manager_start(void);
void sx_ap_manager_stop_ap(void);
bool sx_ap_manager_is_on(void);
uint32_t sx_ap_manager_get_wait_time_sec(void);
uint32_t sx_ap_manager_get_remaining_sec(void);

#ifdef __cplusplus
}
#endif
