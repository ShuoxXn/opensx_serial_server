/*
 * @Author: Orion
 * @Date: 2024-05-08 16:08:44
 * @LastEditors: Orion
 * @LastEditTime: 2025-05-19 11:19:28
 * @FilePath: \SERIIAL_SERVER_P\main\include\sx_async_uart.h
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Maximum number of Modbus items supported across configs.
#define MODBUS_ITEM_MAX 50

// Modbus配置项结构体
typedef struct {
    bool enabled;           // 添加启用状态字段
    char slave_addr[8];
    char function_code[8];
    char register_addr[8];
    char register_num[8];
    char timeout[8];
    char data_format[16];
    char interval_time[8];
    char report_format[8];
    char baud_rate[8];
    char data_bit[8];
    char stop_bit[8];
    char check_bit[8];
} ModbusItemConfig;

// Modbus任务配置结构体
typedef struct {
    int items_count;
    ModbusItemConfig *items;
} ModbusTaskConfig;

// 函数声明
void stop_modbus_tasks(void);
esp_err_t start_modbus_task(ModbusTaskConfig *config);
uint16_t calculate_crc(uint8_t *data, size_t length);
char* convert_modbus_data(uint8_t *data, size_t length, const char *format);
extern SemaphoreHandle_t modbus_rtu_response_sem;
#ifdef __cplusplus
}
#endif
