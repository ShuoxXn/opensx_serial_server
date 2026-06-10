/*
 * @Author: Orion
 * @Date: 2024-01-11 11:20:29
 * @LastEditors: Orion
 * @LastEditTime: 2025-02-11 13:53:45
 * @FilePath: \SERIIAL_SERVER\main\include\sx_mqtt_client.h
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */
#pragma once

/*
 * Internal MQTT Client module.
 *
 * Work modes should prefer sx_protocol_api.h instead of calling this module
 * directly. This header is kept for framework internals and backward
 * compatibility.
 */

#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
#include <stdint.h>
bool mqtt_app_start(void);
void mqtt_app_stop(void);
int mqtt_publish_topic(const char *topic, const char *data, int len, int qos,
                       int retain);
#ifdef __cplusplus
}
#endif
