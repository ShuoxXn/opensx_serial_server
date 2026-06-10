/*
 * @Author: Orion
 * @Date: 2024-05-08 16:08:44
 * @LastEditors: Orion
 * @LastEditTime: 2025-02-14 15:22:44
 * @FilePath: \SERIIAL_SERVER_P\main\include\sx_async_uart.h
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include <stdbool.h>
#include "hal/uart_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/mpu_wrappers.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define UART_IO_BUF_SIZE 4096
void uart_init(void);
// char *tx_task(uint8_t data[], size_t length);
void tx_task(uint8_t data[], size_t length);
void tx_tasks(uint8_t data[], size_t length);
// void tcp_tx_task(char *data, size_t length);
int sendData(char *data, size_t length);
// void rx_task(void *arg);
extern uint8_t dataArray[];
void uart_reinit(int baudrate, uart_word_length_t data_bits,
                 uart_parity_t parity, uart_stop_bits_t stop_bits,
                 int frame_time, int frame_len, bool persist);
// void rx_task(void *arg);
void rx_wait();
void stop_rx_task();
void uart_cleanup_buffers(void);  // 清理缓冲区（可选，通常不需要）
// extern char device_response[256];
extern uint8_t *uart_response;       // 接收数据缓冲区（动态分配）
extern uint8_t *uart_tx_data;        // 发送数据缓冲区（动态分配）
extern size_t uart_response_size;   // 接收缓冲区大小
extern size_t uart_tx_data_size;    // 发送缓冲区大小

extern size_t response_len;          // 接收数据长度
extern size_t tx_data_len;          // 发送数据长度
extern char *hex_data_str;
extern SemaphoreHandle_t xSemaphore;
// extern uint8_t uart_response[BUF_SIZE];
extern portMUX_TYPE uart_spinlock;
extern SemaphoreHandle_t uart_mutex;  // UART操作互斥锁

extern int current_modbus_template_target;  // 当前正在处理的模板索引


// 添加时间戳结构定义
typedef struct {
    uint64_t tx_timestamp;
    uint64_t rx_timestamp;
} uart_timestamps_t;

// 声明为外部变量
extern uart_timestamps_t uart_timestamps;

#ifdef __cplusplus
}
#endif
