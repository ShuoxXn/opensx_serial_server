/*
 * @Author: Orion
 * @Date: 2024-01-11 11:19:54
 * @LastEditors: Orion
 * @LastEditTime: 2025-02-11 11:52:32
 * @FilePath: \SERIIAL_SERVER\main\include\sx_http_client.h
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#pragma once

/*
 * Internal HTTP Client module.
 *
 * Work modes should prefer sx_protocol_api.h instead of calling this module
 * directly. This header is kept for framework internals and backward
 * compatibility.
 */

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void http_test_task(void *pvParameters);
void http_report_data(uint8_t *data, int len, int template_index);
#ifdef __cplusplus
}
#endif
