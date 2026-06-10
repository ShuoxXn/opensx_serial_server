/*
 * @Author: Orion
 * @Date: 2024-01-11 11:21:24
 * @LastEditors: Orion
 * @LastEditTime: 2025-06-11 11:51:28
 * @FilePath: \SERIIAL_SERVER\main\include\sx_utils.h
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

#define TCP_MODE_SERVER 0
#define TCP_MODE_CLIENT 1
#define TCP_MODE_IS_SERVER(mode) ((mode) == TCP_MODE_SERVER)
#define TCP_MODE_IS_CLIENT(mode) ((mode) == TCP_MODE_CLIENT)

#define MAX_MODBUS_REGISTERS_PER_RESPONSE 128

void urlDecode(char *src, char *dest);
void save_form_data(const char *query);
char *reset_reason_to_string(esp_reset_reason_t reason);
esp_err_t nvs_init();
void save_to_nvs(const char *json_string);
double pressure_to_altitude(double pressure);

extern char messageid[48];
// 添加网络连接状态标志的extern声明
extern bool is_network_connected;

// 添加内网模式配置
extern bool is_intranet_mode;

// 添加网络类型标志
extern bool is_wifi_active;

// 添加安全重置 NVS 的函数声明
esp_err_t nvs_safe_reset(void);
esp_err_t nvs_reset_clear(void);
float get_nvs_usage_percentage(void);
void monitor_nvs_usage(void);

char *send_device_info(uint8_t *u_data, int idx);
char *send_device_info_err(void);
char *send_device_ip(void);

esp_err_t nvs_reset_clear();
void protocol_select();
void eth_disconnect();
void simple_task();
char* convert_multi_register_data(uint16_t *registers, int reg_count, const char* format);

// 新增函数声明
bool verify_credentials(const char *username, const char *password);
bool update_device_settings(cJSON *parm);
char *build_mqtt_config_json(void);
char *heartbeat_config_json(void);

bool verify_credentials(const char *username, const char *password);
bool update_device_settings(cJSON *parm);

// 网络连接检查函数
bool check_network_connectivity(void);
bool check_local_network_connectivity(void);
bool check_internet_connectivity(void);
bool ping_gateway(const char* gateway_ip);

// 网络诊断和DNS检查函数
bool check_dns_resolution(const char* hostname);
void network_diagnostics(void);

// 强制网络状态更新函数
void force_network_status_update(void);

void perform_factory_reset(bool preserve_work_mode);
bool is_valid_work_mode(const char *work_mode);
esp_err_t nvs_get_work_mode(nvs_handle_t handle, char *work_mode, size_t size);
esp_err_t nvs_set_work_mode(nvs_handle_t handle, const char *work_mode);
#ifdef __cplusplus
}
#endif
