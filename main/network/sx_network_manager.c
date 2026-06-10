#include "sx_network_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "ping/ping_sock.h"
#include "sx_ap_manager.h"
#include "sx_protocol_manager.h"
#include "sx_web_server.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "NETWORK_MANAGER";
static const char *INTERNET_CHECK_HOST = "www.baidu.com";

#define NET_QUEUE_LEN 12
#define NET_CHECK_INTERVAL_US (30LL * 1000LL * 1000LL)

static QueueHandle_t s_net_queue = NULL;
static SemaphoreHandle_t s_status_mutex = NULL;
static TaskHandle_t s_net_task = NULL;
static esp_timer_handle_t s_periodic_timer = NULL;
static sx_network_status_t s_status = {0};

extern bool is_network_connected;
extern bool is_intranet_mode;
extern bool is_wifi_active;

typedef struct {
  SemaphoreHandle_t done;
  bool success;
} ping_wait_ctx_t;

static void copy_ip_info_to_event(sx_net_event_t *event,
                                  const esp_netif_ip_info_t *ip_info) {
  if (!event || !ip_info) {
    return;
  }
  inet_ntoa_r(ip_info->ip, event->ip, sizeof(event->ip));
  inet_ntoa_r(ip_info->netmask, event->netmask, sizeof(event->netmask));
  inet_ntoa_r(ip_info->gw, event->gateway, sizeof(event->gateway));
}

static void status_copy_ip(const sx_net_event_t *event) {
  strncpy(s_status.ip, event->ip, sizeof(s_status.ip) - 1);
  strncpy(s_status.netmask, event->netmask, sizeof(s_status.netmask) - 1);
  strncpy(s_status.gateway, event->gateway, sizeof(s_status.gateway) - 1);
  s_status.ip[sizeof(s_status.ip) - 1] = '\0';
  s_status.netmask[sizeof(s_status.netmask) - 1] = '\0';
  s_status.gateway[sizeof(s_status.gateway) - 1] = '\0';

  strncpy(got_ip_addrs, s_status.ip[0] ? s_status.ip : "---", 16);
  strncpy(got_ip_netmask, s_status.netmask[0] ? s_status.netmask : "---", 16);
  strncpy(got_ip_gw, s_status.gateway[0] ? s_status.gateway : "---", 16);
  got_ip_addrs[15] = '\0';
  got_ip_netmask[15] = '\0';
  got_ip_gw[15] = '\0';
}

static void status_clear_ip(void) {
  strcpy(s_status.ip, "---");
  strcpy(s_status.netmask, "---");
  strcpy(s_status.gateway, "---");
  strcpy(got_ip_addrs, "---");
  strcpy(got_ip_netmask, "---");
  strcpy(got_ip_gw, "---");
}

static void sync_legacy_flags(void) {
  is_network_connected = (s_status.net_state != SX_NET_STATE_DOWN);
  is_intranet_mode = (s_status.net_state == SX_NET_STATE_LOCAL);
  is_wifi_active = (s_status.active_if == SX_NET_IF_WIFI);
  s_status.ap_state = sx_ap_manager_is_on() ? SX_AP_STATE_ON : SX_AP_STATE_OFF;
  s_status.ap_wait_time_sec = sx_ap_manager_get_wait_time_sec();
  s_status.ap_remaining_sec = sx_ap_manager_get_remaining_sec();
}

static bool set_network_state(sx_net_state_t new_state,
                              sx_net_state_t *old_state_out) {
  sx_net_state_t old_state = s_status.net_state;
  if (old_state_out) {
    *old_state_out = old_state;
  }
  if (old_state == new_state) {
    sync_legacy_flags();
    return false;
  }

  s_status.net_state = new_state;
  sync_legacy_flags();
  ESP_LOGI(TAG, "网络状态: %s -> %s (%s)",
           sx_network_state_name(old_state), sx_network_state_name(new_state),
           sx_network_if_name(s_status.active_if));
  return true;
}

static sx_net_if_t load_configured_interface(void) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READONLY, &handle);
  if (err != ESP_OK) {
    return SX_NET_IF_ETH;
  }

  char netconn[8] = {0};
  size_t len = sizeof(netconn);
  err = nvs_get_str(handle, "netconn", netconn, &len);
  nvs_close(handle);

  if (err == ESP_OK && atoi(netconn) == 2) {
    return SX_NET_IF_WIFI;
  }
  return SX_NET_IF_ETH;
}

static void ping_on_success(esp_ping_handle_t hdl, void *args) {
  ping_wait_ctx_t *ctx = (ping_wait_ctx_t *)args;
  ctx->success = true;
}

static void ping_on_end(esp_ping_handle_t hdl, void *args) {
  ping_wait_ctx_t *ctx = (ping_wait_ctx_t *)args;
  xSemaphoreGive(ctx->done);
}

static bool ping_ip_string(const char *ip_string, uint32_t timeout_ms) {
  if (!ip_string || ip_string[0] == '\0' || strcmp(ip_string, "---") == 0 ||
      strcmp(ip_string, "0.0.0.0") == 0) {
    return false;
  }

  ip_addr_t target_addr;
  memset(&target_addr, 0, sizeof(target_addr));
  struct in_addr addr4;
  if (!inet_aton(ip_string, &addr4)) {
    ESP_LOGW(TAG, "ping目标IP无效: %s", ip_string);
    return false;
  }
  target_addr.type = IPADDR_TYPE_V4;
  target_addr.u_addr.ip4.addr = addr4.s_addr;

  ping_wait_ctx_t ctx = {
      .done = xSemaphoreCreateBinary(),
      .success = false,
  };
  if (ctx.done == NULL) {
    return false;
  }

  esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
  config.target_addr = target_addr;
  config.count = 1;
  config.timeout_ms = timeout_ms;
  config.interval_ms = timeout_ms;

  esp_ping_callbacks_t callbacks = {
      .on_ping_success = ping_on_success,
      .on_ping_end = ping_on_end,
      .cb_args = &ctx,
  };

  esp_ping_handle_t ping = NULL;
  esp_err_t err = esp_ping_new_session(&config, &callbacks, &ping);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "创建ping会话失败: %s", esp_err_to_name(err));
    vSemaphoreDelete(ctx.done);
    return false;
  }

  err = esp_ping_start(ping);
  if (err == ESP_OK) {
    xSemaphoreTake(ctx.done, pdMS_TO_TICKS(timeout_ms + 500));
  } else {
    ESP_LOGW(TAG, "启动ping失败: %s", esp_err_to_name(err));
  }

  esp_ping_delete_session(ping);
  vSemaphoreDelete(ctx.done);
  return ctx.success;
}

static bool resolve_hostname_ip(const char *host, char *out, size_t out_len) {
  struct addrinfo hints;
  struct addrinfo *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  int err = getaddrinfo(host, NULL, &hints, &res);
  if (err != 0 || res == NULL) {
    ESP_LOGW(TAG, "解析外网检测域名失败: %s, err=%d", host, err);
    return false;
  }

  struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
  inet_ntoa_r(addr->sin_addr, out, out_len);
  freeaddrinfo(res);
  return true;
}

static bool check_internet_external(bool *internet_ping_ok) {
  char internet_ip[16] = {0};
  if (internet_ping_ok) {
    *internet_ping_ok = false;
  }

  if (!resolve_hostname_ip(INTERNET_CHECK_HOST, internet_ip,
                           sizeof(internet_ip))) {
    return false;
  }

  bool ping_ok = ping_ip_string(internet_ip, 1500);
  if (internet_ping_ok) {
    *internet_ping_ok = ping_ok;
  }
  if (!ping_ok) {
    ESP_LOGW(TAG, "外网检测ping失败: %s(%s)", INTERNET_CHECK_HOST,
             internet_ip);
    return false;
  }

  ESP_LOGI(TAG, "外网检测ping成功: %s(%s)", INTERNET_CHECK_HOST, internet_ip);
  return true;
}

static sx_net_state_t evaluate_wifi_state(const sx_network_status_t *snapshot,
                                          bool *gateway_ping_ok,
                                          bool *internet_ping_ok) {
  if (gateway_ping_ok) {
    *gateway_ping_ok = false;
  }

  if (!snapshot->wifi_connected || !snapshot->wifi_got_ip) {
    return SX_NET_STATE_DOWN;
  }

  bool gateway_ok = ping_ip_string(snapshot->gateway, 1500);
  if (gateway_ping_ok) {
    *gateway_ping_ok = gateway_ok;
  }
  if (!gateway_ok) {
    ESP_LOGW(TAG, "WiFi网关ping失败: %s", snapshot->gateway);
    return SX_NET_STATE_DOWN;
  }

  if (check_internet_external(internet_ping_ok)) {
    return SX_NET_STATE_EXTERNAL;
  }
  return SX_NET_STATE_LOCAL;
}

static sx_net_state_t evaluate_eth_state(const sx_network_status_t *snapshot,
                                         bool *gateway_ping_ok,
                                         bool *internet_ping_ok) {
  if (gateway_ping_ok) {
    *gateway_ping_ok = false;
  }

  if (!snapshot->eth_got_ip && !snapshot->eth_static_ip_valid) {
    return SX_NET_STATE_DOWN;
  }

  if (check_internet_external(internet_ping_ok)) {
    return SX_NET_STATE_EXTERNAL;
  }
  return SX_NET_STATE_LOCAL;
}

static bool status_matches_snapshot_locked(const sx_network_status_t *snapshot) {
  return s_status.active_if == snapshot->active_if &&
         s_status.wifi_connected == snapshot->wifi_connected &&
         s_status.wifi_got_ip == snapshot->wifi_got_ip &&
         s_status.eth_got_ip == snapshot->eth_got_ip &&
         s_status.eth_static_ip_valid == snapshot->eth_static_ip_valid &&
         strcmp(s_status.ip, snapshot->ip) == 0 &&
         strcmp(s_status.gateway, snapshot->gateway) == 0;
}

static void evaluate_current_state(void) {
  sx_network_status_t snapshot;
  memset(&snapshot, 0, sizeof(snapshot));

  xSemaphoreTake(s_status_mutex, portMAX_DELAY);
  memcpy(&snapshot, &s_status, sizeof(snapshot));
  xSemaphoreGive(s_status_mutex);

  sx_net_state_t new_state = SX_NET_STATE_DOWN;
  bool gateway_ping_ok = false;
  bool internet_ping_ok = false;

  if (snapshot.active_if == SX_NET_IF_WIFI) {
    new_state = evaluate_wifi_state(&snapshot, &gateway_ping_ok,
                                    &internet_ping_ok);
  } else if (snapshot.active_if == SX_NET_IF_ETH) {
    new_state = evaluate_eth_state(&snapshot, &gateway_ping_ok,
                                   &internet_ping_ok);
  }

  xSemaphoreTake(s_status_mutex, portMAX_DELAY);
  if (!status_matches_snapshot_locked(&snapshot)) {
    ESP_LOGI(TAG, "检测期间网络基础状态已变化，丢弃本次检测结果");
    xSemaphoreGive(s_status_mutex);
    return;
  }

  s_status.gateway_ping_ok = gateway_ping_ok;
  s_status.internet_ping_ok = internet_ping_ok;
  sx_net_state_t old_state = SX_NET_STATE_DOWN;
  bool state_changed = set_network_state(new_state, &old_state);
  xSemaphoreGive(s_status_mutex);

  if (state_changed || new_state != SX_NET_STATE_DOWN) {
    sx_protocol_manager_on_network_state(old_state, new_state);
  }
}

static void handle_event(const sx_net_event_t *event) {
  if (!event) {
    return;
  }

  bool should_evaluate = false;
  bool state_changed = false;
  sx_net_state_t old_state = SX_NET_STATE_DOWN;
  sx_net_state_t new_state = SX_NET_STATE_DOWN;
  xSemaphoreTake(s_status_mutex, portMAX_DELAY);

  switch (event->type) {
  case SX_NET_EVT_BOOT:
    s_status.active_if = load_configured_interface();
    s_status.net_state = SX_NET_STATE_DOWN;
    status_clear_ip();
    ESP_LOGI(TAG, "网络管理启动，配置接口: %s",
             sx_network_if_name(s_status.active_if));
    sync_legacy_flags();
    break;

  case SX_NET_EVT_WIFI_CONNECTED:
    s_status.active_if = SX_NET_IF_WIFI;
    s_status.wifi_connected = true;
    sync_legacy_flags();
    break;

  case SX_NET_EVT_WIFI_DISCONNECTED:
    s_status.active_if = SX_NET_IF_WIFI;
    s_status.wifi_connected = false;
    s_status.wifi_got_ip = false;
    s_status.gateway_ping_ok = false;
    s_status.internet_ping_ok = false;
    status_clear_ip();
    new_state = SX_NET_STATE_DOWN;
    state_changed = set_network_state(new_state, &old_state);
    break;

  case SX_NET_EVT_WIFI_GOT_IP:
    s_status.active_if = SX_NET_IF_WIFI;
    s_status.wifi_connected = true;
    s_status.wifi_got_ip = true;
    status_copy_ip(event);
    should_evaluate = true;
    break;

  case SX_NET_EVT_ETH_LINK_UP:
    s_status.active_if = SX_NET_IF_ETH;
    sync_legacy_flags();
    break;

  case SX_NET_EVT_ETH_LINK_DOWN:
    s_status.active_if = SX_NET_IF_ETH;
    s_status.eth_got_ip = false;
    s_status.internet_ping_ok = false;
    if (s_status.eth_static_ip_valid) {
      should_evaluate = true;
    } else {
      status_clear_ip();
      new_state = SX_NET_STATE_DOWN;
      state_changed = set_network_state(new_state, &old_state);
    }
    break;

  case SX_NET_EVT_ETH_GOT_IP:
    s_status.active_if = SX_NET_IF_ETH;
    s_status.eth_got_ip = true;
    status_copy_ip(event);
    should_evaluate = true;
    break;

  case SX_NET_EVT_ETH_STATIC_IP_READY:
    s_status.active_if = SX_NET_IF_ETH;
    s_status.eth_static_ip_valid = true;
    status_copy_ip(event);
    should_evaluate = true;
    break;

  case SX_NET_EVT_AP_STARTED:
    s_status.ap_state = SX_AP_STATE_ON;
    sync_legacy_flags();
    break;

  case SX_NET_EVT_AP_STOPPED:
    s_status.ap_state = SX_AP_STATE_OFF;
    sync_legacy_flags();
    break;

  case SX_NET_EVT_PERIODIC_CHECK:
    should_evaluate = true;
    break;

  default:
    break;
  }

  xSemaphoreGive(s_status_mutex);

  if (state_changed) {
    sx_protocol_manager_on_network_state(old_state, new_state);
  }

  if (should_evaluate) {
    evaluate_current_state();
  }
}

static void network_task(void *arg) {
  (void)arg;
  sx_net_event_t event;
  while (1) {
    if (xQueueReceive(s_net_queue, &event, portMAX_DELAY) == pdTRUE) {
      handle_event(&event);
    }
  }
}

static void periodic_timer_callback(void *arg) {
  (void)arg;
  sx_net_event_t event = {
      .type = SX_NET_EVT_PERIODIC_CHECK,
      .source_if = SX_NET_IF_NONE,
  };
  sx_network_manager_post_event(&event);
}

esp_err_t sx_network_manager_init(void) {
  if (s_status_mutex == NULL) {
    s_status_mutex = xSemaphoreCreateMutex();
    if (s_status_mutex == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }

  if (s_net_queue == NULL) {
    s_net_queue = xQueueCreate(NET_QUEUE_LEN, sizeof(sx_net_event_t));
    if (s_net_queue == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }

  if (s_net_task == NULL) {
    BaseType_t ok =
        xTaskCreate(network_task, "network_manager", 8192, NULL, 5, &s_net_task);
    if (ok != pdPASS) {
      return ESP_ERR_NO_MEM;
    }
  }

  if (s_periodic_timer == NULL) {
    esp_timer_create_args_t args = {
        .callback = periodic_timer_callback,
        .name = "network_periodic",
    };
    esp_err_t err = esp_timer_create(&args, &s_periodic_timer);
    if (err != ESP_OK) {
      return err;
    }
    err = esp_timer_start_periodic(s_periodic_timer, NET_CHECK_INTERVAL_US);
    if (err != ESP_OK) {
      return err;
    }
  }

  sx_net_event_t event = {
      .type = SX_NET_EVT_BOOT,
      .source_if = SX_NET_IF_NONE,
  };
  return sx_network_manager_post_event(&event);
}

esp_err_t sx_network_manager_post_event(const sx_net_event_t *event) {
  if (s_net_queue == NULL || event == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  if (xQueueSend(s_net_queue, event, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_LOGW(TAG, "网络事件队列满，丢弃事件: %d", event->type);
    return ESP_FAIL;
  }
  return ESP_OK;
}

void sx_network_manager_get_status(sx_network_status_t *out) {
  if (!out) {
    return;
  }
  if (s_status_mutex && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    sync_legacy_flags();
    memcpy(out, &s_status, sizeof(*out));
    xSemaphoreGive(s_status_mutex);
  } else {
    memcpy(out, &s_status, sizeof(*out));
  }
}

sx_net_if_t sx_network_manager_get_configured_interface(void) {
  return load_configured_interface();
}

void sx_network_manager_notify_wifi_connected(void) {
  sx_net_event_t event = {
      .type = SX_NET_EVT_WIFI_CONNECTED,
      .source_if = SX_NET_IF_WIFI,
  };
  sx_network_manager_post_event(&event);
}

void sx_network_manager_notify_wifi_disconnected(void) {
  sx_net_event_t event = {
      .type = SX_NET_EVT_WIFI_DISCONNECTED,
      .source_if = SX_NET_IF_WIFI,
  };
  sx_network_manager_post_event(&event);
}

void sx_network_manager_notify_wifi_got_ip(const esp_netif_ip_info_t *ip_info) {
  sx_net_event_t event = {
      .type = SX_NET_EVT_WIFI_GOT_IP,
      .source_if = SX_NET_IF_WIFI,
  };
  copy_ip_info_to_event(&event, ip_info);
  sx_network_manager_post_event(&event);
}

void sx_network_manager_notify_eth_link_up(void) {
  sx_net_event_t event = {
      .type = SX_NET_EVT_ETH_LINK_UP,
      .source_if = SX_NET_IF_ETH,
  };
  sx_network_manager_post_event(&event);
}

void sx_network_manager_notify_eth_link_down(void) {
  sx_net_event_t event = {
      .type = SX_NET_EVT_ETH_LINK_DOWN,
      .source_if = SX_NET_IF_ETH,
  };
  sx_network_manager_post_event(&event);
}

void sx_network_manager_notify_eth_got_ip(const esp_netif_ip_info_t *ip_info) {
  sx_net_event_t event = {
      .type = SX_NET_EVT_ETH_GOT_IP,
      .source_if = SX_NET_IF_ETH,
  };
  copy_ip_info_to_event(&event, ip_info);
  sx_network_manager_post_event(&event);
}

void sx_network_manager_notify_eth_static_ip(const esp_netif_ip_info_t *ip_info) {
  sx_net_event_t event = {
      .type = SX_NET_EVT_ETH_STATIC_IP_READY,
      .source_if = SX_NET_IF_ETH,
  };
  copy_ip_info_to_event(&event, ip_info);
  sx_network_manager_post_event(&event);
}

void sx_network_manager_notify_ap_started(void) {
  sx_net_event_t event = {
      .type = SX_NET_EVT_AP_STARTED,
      .source_if = SX_NET_IF_NONE,
  };
  sx_network_manager_post_event(&event);
}

void sx_network_manager_notify_ap_stopped(void) {
  sx_net_event_t event = {
      .type = SX_NET_EVT_AP_STOPPED,
      .source_if = SX_NET_IF_NONE,
  };
  sx_network_manager_post_event(&event);
}

const char *sx_network_if_name(sx_net_if_t netif) {
  switch (netif) {
  case SX_NET_IF_WIFI:
    return "WiFi";
  case SX_NET_IF_ETH:
    return "Ethernet";
  default:
    return "None";
  }
}

const char *sx_network_state_name(sx_net_state_t state) {
  switch (state) {
  case SX_NET_STATE_LOCAL:
    return "LOCAL";
  case SX_NET_STATE_EXTERNAL:
    return "EXTERNAL";
  default:
    return "DOWN";
  }
}
