/*
 * @Author: Orion
 * @Date: 2024-01-23 16:10:01
 * @LastEditors: Orion
 * @LastEditTime: 2025-06-11 19:17:50
 * @FilePath: \SERIIAL_SERVER\main\include\sx_web_server.h
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stdbool.h>
#include "esp_http_server.h"
extern char dIp[20];
extern char dNetmask[20];
extern char dGateway[20];
extern uint8_t mac_addr[6];
extern char device_mac[30];
extern char th[20];
extern char device_mac_end[20];
extern char got_ip_addrs[16];
extern char got_ip_netmask[16];
extern char got_ip_gw[16];
extern int httpreason;
extern char device_sta_mac[24];
extern char device_eth_mac[24];
void http_server_init(void);
void send_log_to_websocket(const char* log_msg);
void send_uart_to_websocket(const uint8_t *data, size_t len, bool is_tx);
// 设备标识配置
#ifndef DEVICE_MODEL
#define DEVICE_MODEL "SP501W"
#endif

#ifndef BRAND_TYPE
#define BRAND_TYPE "SHUOXIN"
#endif

#ifndef BRAND_NAME_CN
#define BRAND_NAME_CN "硕芯电子"
#endif

#ifndef BRAND_NAME_EN
#define BRAND_NAME_EN "SHUOXIN"
#endif

#define VERSION "open_sx_1.0.0"
#define DEVICE_TYPE DEVICE_MODEL
#define CLIENT_HEAD "SP501"  // 保持不变，用于平台通信
#ifdef __cplusplus
}
#endif
