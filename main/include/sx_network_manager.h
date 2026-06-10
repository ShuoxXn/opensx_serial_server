#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  SX_NET_IF_NONE = 0,
  SX_NET_IF_WIFI,
  SX_NET_IF_ETH,
} sx_net_if_t;

typedef enum {
  SX_NET_STATE_DOWN = 0,
  SX_NET_STATE_LOCAL,
  SX_NET_STATE_EXTERNAL,
} sx_net_state_t;

typedef enum {
  SX_AP_STATE_OFF = 0,
  SX_AP_STATE_ON,
} sx_ap_state_t;

typedef struct {
  sx_net_if_t active_if;
  sx_net_state_t net_state;
  sx_ap_state_t ap_state;

  bool wifi_connected;
  bool wifi_got_ip;
  bool eth_got_ip;
  bool eth_static_ip_valid;

  bool gateway_ping_ok;
  bool internet_ping_ok;

  char ip[16];
  char netmask[16];
  char gateway[16];
  char dns1[16];
  char dns2[16];

  uint32_t ap_wait_time_sec;
  uint32_t ap_remaining_sec;
} sx_network_status_t;

typedef enum {
  SX_NET_EVT_BOOT = 0,
  SX_NET_EVT_WIFI_CONNECTED,
  SX_NET_EVT_WIFI_DISCONNECTED,
  SX_NET_EVT_WIFI_GOT_IP,
  SX_NET_EVT_ETH_LINK_UP,
  SX_NET_EVT_ETH_LINK_DOWN,
  SX_NET_EVT_ETH_GOT_IP,
  SX_NET_EVT_ETH_STATIC_IP_READY,
  SX_NET_EVT_AP_STARTED,
  SX_NET_EVT_AP_STOPPED,
  SX_NET_EVT_PERIODIC_CHECK,
} sx_net_event_type_t;

typedef struct {
  sx_net_event_type_t type;
  sx_net_if_t source_if;
  char ip[16];
  char netmask[16];
  char gateway[16];
  char dns1[16];
  char dns2[16];
} sx_net_event_t;

esp_err_t sx_network_manager_init(void);
esp_err_t sx_network_manager_post_event(const sx_net_event_t *event);
void sx_network_manager_get_status(sx_network_status_t *out);
sx_net_if_t sx_network_manager_get_configured_interface(void);

void sx_network_manager_notify_wifi_connected(void);
void sx_network_manager_notify_wifi_disconnected(void);
void sx_network_manager_notify_wifi_got_ip(const esp_netif_ip_info_t *ip_info);
void sx_network_manager_notify_eth_link_up(void);
void sx_network_manager_notify_eth_link_down(void);
void sx_network_manager_notify_eth_got_ip(const esp_netif_ip_info_t *ip_info);
void sx_network_manager_notify_eth_static_ip(const esp_netif_ip_info_t *ip_info);
void sx_network_manager_notify_ap_started(void);
void sx_network_manager_notify_ap_stopped(void);

const char *sx_network_if_name(sx_net_if_t netif);
const char *sx_network_state_name(sx_net_state_t state);

#ifdef __cplusplus
}
#endif
