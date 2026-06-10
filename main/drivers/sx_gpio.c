/*
 * @Author: Orion
 * @Date: 2024-02-03 11:29:35
 * @LastEditors: Orion
 * @LastEditTime: 2025-05-21 14:18:52
 * @FilePath: \SERIIAL_SERVER_P\main\sx_gpio.c
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#include "sx_gpio.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint16_t KeyState = 0, LED_LAN_STATE = 0, LED_WIFI_STATE = 0, LED_DAT_STATE = 0;

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

void LED_LAN_ON() {
  gpio_set_level(LED_LAN, 0);
  vTaskDelay(10 / portTICK_PERIOD_MS);
  // gpio_set_level(KEY, 1);
  LED_LAN_STATE = 1;
}
void LED_LAN_OFF() {
  gpio_set_level(LED_LAN, 1);
  vTaskDelay(10 / portTICK_PERIOD_MS);
  // gpio_set_level(KEY, 0);
  LED_LAN_STATE = 0;
}

void LED_WIFI_ON() {
  gpio_set_level(LED_WIFI, 0);
  vTaskDelay(10 / portTICK_PERIOD_MS);
  // gpio_set_level(KEY, 1);
  LED_WIFI_STATE = 1;
}
void LED_WIFI_OFF() {
  gpio_set_level(LED_WIFI, 1);
  vTaskDelay(10 / portTICK_PERIOD_MS);
  // gpio_set_level(KEY, 0);
  LED_WIFI_STATE = 0;
}

void LED_DAT_ON() {
  gpio_set_level(LED_DAT, 0);
  vTaskDelay(10 / portTICK_PERIOD_MS);
  // gpio_set_level(KEY, 1);
  LED_DAT_STATE = 1;
}

void LED_DAT_OFF() {
  gpio_set_level(LED_DAT, 1);
  vTaskDelay(10 / portTICK_PERIOD_MS);
  // gpio_set_level(KEY, 0);
  LED_DAT_STATE = 0;
}

void init_gpio() {
  gpio_config_t key_conf = {
      .pin_bit_mask = (1ULL << KEY),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&key_conf)); // 输入专用 GPIO 仅保持浮空
  gpio_config_t led_lan_conf = {
      .pin_bit_mask = (1ULL << LED_LAN),
      .mode = GPIO_MODE_OUTPUT,
  };
  gpio_config(&led_lan_conf);

  gpio_config_t led_wifi_conf = {
      .pin_bit_mask = (1ULL << LED_WIFI),
      .mode = GPIO_MODE_OUTPUT,
  };
  gpio_config(&led_wifi_conf);
  gpio_config_t led_data_conf = {
      .pin_bit_mask = (1ULL << LED_DAT),
      .mode = GPIO_MODE_OUTPUT,
  };
  gpio_config(&led_data_conf);

  esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
  esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
  esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
  esp_log_level_set("wifi", ESP_LOG_ERROR);

  // 设置 GPIO 为高电平（LED 保持常亮）
  gpio_set_level(LED_LAN, 1); // LED0灭
  gpio_set_level(LED_WIFI, 1);
  gpio_set_level(LED_DAT, 1);

  gpio_reset_pin(GPIO_NUM_4);
  gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_4, 0);
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
