/*
 * @Author: Orion
 * @Date: 2024-01-11 11:20:16
 * @LastEditors: Orion
 * @LastEditTime: 2025-06-11 12:14:27
 * @FilePath: \SERIIAL_SERVER\main\include\sx_init_wifi.h
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#pragma once

#include "esp_netif.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void wifi_init_sta(const char *w_ssid, const char *w_passwd);
esp_netif_t *wifi_ensure_sta_netif(void);
void start_wifi_task(const char *ssid, const char *password);
// 暂停WiFi重连，避免在AP模式下Web服务器卡顿
void pause_wifi_reconnect(void);
// 恢复WiFi重连
void resume_wifi_reconnect(void);
// 检测WiFi扫描是否被允许
bool is_wifi_scan_allowed(void);
extern int is_netif_initialized;
#ifdef __cplusplus
}
#endif
