/*
 * @Author: Orion
 * @Date: 2024-01-23 15:57:22
 * @LastEditors: Orion
 * @LastEditTime: 2025-06-11 13:53:51
 * @FilePath: \ETH_TH\main\sx_init_eth.c
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "sx_network_manager.h"
#include "sx_utils.h"
#include "sx_web_server.h"
#include "driver/gpio.h"
#include "sx_gpio.h"
#include <string.h>
#include "esp_timer.h"
static const char *TAG = "ETH-TH";

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

/** Event handler for Ethernet events */
void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                       void *event_data) {
  uint8_t mac_addr[6] = {0};
  /* we can get the ethernet driver handle from event data */
  esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

  switch (event_id) {
  case ETHERNET_EVENT_CONNECTED:
    esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
    ESP_LOGI(TAG, "Ethernet Link Up");
    ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4],
             mac_addr[5]);
    sprintf(device_mac, "%02X%02X%02X%02X%02X%02X", mac_addr[0], mac_addr[1],
            mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    sprintf(device_mac_end, "%02X%02X", mac_addr[4], mac_addr[5]);
    sx_network_manager_notify_eth_link_up();
    break;
  case ETHERNET_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "Ethernet Link Down");
    // LED状态由常驻LED定时器根据网络管理器状态统一刷新。
    LED_LAN_OFF();

    sx_network_manager_notify_eth_link_down();
    break;
  case ETHERNET_EVENT_START:
    ESP_LOGI(TAG, "Ethernet Started");
    break;
  case ETHERNET_EVENT_STOP:
    ESP_LOGI(TAG, "Ethernet Stopped");
    break;
  default:
    ESP_LOGI(TAG, "Unknown Ethernet Event");
    break;
  }
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
