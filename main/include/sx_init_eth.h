/*
 * @Author: Orion
 * @Date: 2024-05-07 11:27:30
 * @LastEditors: Orion
 * @LastEditTime: 2024-05-07 11:27:37
 * @FilePath: \SERIIAL_SERVER\main\include\sx_init_eth.h
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif
void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                       void *event_data);

#ifdef __cplusplus
}
#endif
