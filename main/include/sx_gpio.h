/*
 * @Author: Orion
 * @Date: 2024-02-03 11:26:54
 * @LastEditors: Orion
 * @LastEditTime: 2024-10-22 10:46:48
 * @FilePath: \SERIIAL_SERVER\main\include\sx_gpio.h
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
// #define LED GPIO_NUM_4
// #define LED0 GPIO_NUM_13
#define KEY GPIO_NUM_38
#define LED_LAN GPIO_NUM_14
#define LED_WIFI GPIO_NUM_13
#define LED_DAT GPIO_NUM_15

extern uint16_t KeyState, LED_LAN_STATE, LED_WIFI_STATE, LED_DAT_STATE;
void LED_LAN_ON();
void LED_LAN_OFF();
void LED_WIFI_ON();
void LED_WIFI_OFF();
void LED_DAT_ON();
void LED_DAT_OFF();
void init_gpio();

#ifdef __cplusplus
}
#endif
