/*
 * @Author: AI Assistant
 * @Date: 2024-07-18
 * @Description: 使用ESP32定时器替代FreeRTOS任务，减少内存占用
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#pragma once

#include "esp_timer.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 定时器任务初始化和停止函数
esp_err_t init_timer_tasks(void);
esp_err_t stop_timer_tasks(void);
void init_load_time_from_nvs(void);


esp_err_t init_key_timer(void);


esp_err_t init_led_timer(void);
// LED闪烁定时器回调函数声明
void time_sync_led_callback(void *arg);
// 内存监控定时器回调函数声明
void free_timer_callback(void *arg);

extern esp_timer_handle_t led_timer;
extern esp_timer_handle_t key_timer;
extern esp_timer_handle_t free_timer;
extern esp_timer_handle_t time_sync_led_timer;
extern char strftime_buf[64];


#ifdef __cplusplus
}
#endif
