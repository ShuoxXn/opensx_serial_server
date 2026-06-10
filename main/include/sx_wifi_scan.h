/*
 * @Author: Orion
 * @Date: 2024-01-23 17:05:33
 * @LastEditors: Orion
 * @LastEditTime: 2025-05-12 15:24:01
 * @FilePath: \ETH_TH\main\include\sx_wifi_scan.h
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include "esp_wifi.h"

#define DEFAULT_SCAN_LIST_SIZE 15

typedef struct {
  int ap_count;  // 修改为int类型，以支持负值作为错误代码
  wifi_ap_record_t ap_info[DEFAULT_SCAN_LIST_SIZE];
  int wait_time;  // 添加等待时间，单位为毫秒
  int error_code; // 错误代码：0=正常，1=扫描间隔过短，2=扫描不允许，3=其他错误
} WifiValueList;

// 返回是否当前允许WiFi扫描
bool is_wifi_scan_allowed(void);

// 执行WiFi扫描
WifiValueList wifiSearchValue();

// 暂停WiFi自动重连（扫描前调用）
void pause_wifi_reconnect(void);

// 恢复WiFi自动重连（扫描后调用）
void resume_wifi_reconnect(void);

#ifdef __cplusplus
}
#endif
