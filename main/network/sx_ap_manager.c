#include "sx_ap_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "sx_dns_server.h"
#include "sx_network_manager.h"
#include "sx_utils.h"
#include "sx_web_server.h"
#include <stdlib.h>
#include <string.h>

#define ESP_WIFI_SSID DEVICE_MODEL
#define ESP_WIFI_PASS "12345678"
#define MAX_STA_CONN 10
#define AP_WAIT_TIME_DEFAULT_SEC 0
#define AP_WAIT_TIME_FALLBACK_SEC 3600
#define AP_CLOSE_RECHECK_INTERVAL_SEC 10

static const char *TAG = "AP_MANAGER";

static esp_timer_handle_t s_ap_close_timer = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_ap_event_handler_registered = false;
static bool s_ap_ip_event_handler_registered = false;
static bool s_ap_on = false;
static bool s_close_pending = false;
static uint32_t s_wait_time_sec = 0;
static int64_t s_close_at_us = 0;

static void ap_close_timer_callback(void *arg);

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_id == WIFI_EVENT_AP_START) {
    ESP_LOGI(TAG, "softAP started");
  } else if (event_id == WIFI_EVENT_AP_STOP) {
    ESP_LOGI(TAG, "softAP stopped");
  } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event =
        (wifi_event_ap_staconnected_t *)event_data;
    ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac),
             event->aid);
  } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event =
        (wifi_event_ap_stadisconnected_t *)event_data;
    ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac),
             event->aid);
  }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data) {
  if (event_id != IP_EVENT_AP_STAIPASSIGNED || event_data == NULL) {
    return;
  }

  ip_event_ap_staipassigned_t *event =
      (ip_event_ap_staipassigned_t *)event_data;
  char ip_addr[16] = {0};
  inet_ntoa_r(event->ip.addr, ip_addr, sizeof(ip_addr));
  ESP_LOGI(TAG, "station " MACSTR " assigned IP %s", MAC2STR(event->mac),
           ip_addr);
}

static uint32_t load_ap_wait_time_sec(void) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "打开NVS读取AP开放时长失败: %s", esp_err_to_name(err));
    return 0;
  }

  char value[16] = {0};
  size_t len = sizeof(value);
  err = nvs_get_str(handle, "ap_wait_time", value, &len);
  if (err == ESP_ERR_NVS_NOT_FOUND || value[0] == '\0') {
    snprintf(value, sizeof(value), "%u", AP_WAIT_TIME_DEFAULT_SEC);
    nvs_set_str(handle, "ap_wait_time", value);
    nvs_commit(handle);
  }

  int seconds = atoi(value);
  bool valid = seconds == 0 || seconds == 600 || seconds == 1200 ||
               seconds == 1800 || seconds == 3600;
  if (!valid) {
    uint32_t corrected = AP_WAIT_TIME_FALLBACK_SEC;
    ESP_LOGW(TAG, "AP开放时长配置无效: %s，纠正为%u秒", value,
             (unsigned)corrected);
    snprintf(value, sizeof(value), "%lu", (unsigned long)corrected);
    nvs_set_str(handle, "ap_wait_time", value);
    nvs_commit(handle);
    seconds = corrected;
  }

  nvs_close(handle);
  return seconds > 0 ? (uint32_t)seconds : 0;
}

static esp_err_t schedule_ap_close_timer(uint32_t delay_sec) {
  if (s_ap_close_timer == NULL) {
    esp_timer_create_args_t args = {
        .callback = ap_close_timer_callback,
        .name = "ap_close_timer",
    };
    esp_err_t err = esp_timer_create(&args, &s_ap_close_timer);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "创建AP关闭定时器失败: %s", esp_err_to_name(err));
      return err;
    }
  } else {
    esp_timer_stop(s_ap_close_timer);
  }

  return esp_timer_start_once(s_ap_close_timer,
                              (uint64_t)delay_sec * 1000000ULL);
}

static bool ap_has_connected_clients(bool *has_clients) {
  if (has_clients == NULL) {
    return false;
  }

  wifi_sta_list_t sta_list;
  esp_err_t err = esp_wifi_ap_get_sta_list(&sta_list);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "读取AP客户端列表失败: %s", esp_err_to_name(err));
    return false;
  }

  *has_clients = sta_list.num > 0;
  ESP_LOGI(TAG, "AP当前客户端数量: %d", sta_list.num);
  return true;
}

static void ap_close_timer_callback(void *arg) {
  (void)arg;
  if (!s_ap_on) {
    return;
  }

  bool has_clients = false;
  if (!ap_has_connected_clients(&has_clients) || has_clients) {
    s_close_pending = true;
    ESP_LOGI(TAG, "AP开放时长已到，但仍有客户端或无法确认客户端为空，%u秒后重查",
             (unsigned)AP_CLOSE_RECHECK_INTERVAL_SEC);
    esp_err_t err = schedule_ap_close_timer(AP_CLOSE_RECHECK_INTERVAL_SEC);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "重新启动AP关闭检查定时器失败: %s", esp_err_to_name(err));
    }
    return;
  }

  ESP_LOGI(TAG, "AP开放时长已到且无客户端，关闭AP");
  sx_ap_manager_stop_ap();
}

static esp_err_t start_softap(void) {
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t wifi_ret = esp_wifi_init(&cfg);
  if (wifi_ret != ESP_OK && wifi_ret != ESP_ERR_WIFI_INIT_STATE) {
    return wifi_ret;
  }

  if (s_ap_netif == NULL) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
      return ESP_FAIL;
    }
  }

  if (!s_ap_event_handler_registered) {
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    s_ap_event_handler_registered = true;
  }
  if (!s_ap_ip_event_handler_registered) {
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                               IP_EVENT_AP_STAIPASSIGNED,
                                               &ip_event_handler, NULL));
    s_ap_ip_event_handler_registered = true;
  }

  uint8_t sta_mac[6] = {0};
  uint8_t eth_mac[6] = {0};
  esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);
  esp_read_mac(eth_mac, ESP_MAC_ETH);

  ESP_LOGI(TAG, "ESP_MAC_WIFI_STA MAC: %02x:%02x:%02x:%02x:%02x:%02x",
           sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4],
           sta_mac[5]);

  sprintf(device_mac, "%02X%02X%02X%02X%02X%02X", sta_mac[0], sta_mac[1],
          sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
  sprintf(device_mac_end, "%02X%02X", sta_mac[4], sta_mac[5]);
  sprintf(device_sta_mac, "%02X:%02X:%02X:%02X:%02X:%02X", sta_mac[0],
          sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
  sprintf(device_eth_mac, "%02X:%02X:%02X:%02X:%02X:%02X", eth_mac[0],
          eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);

  char default_ap_ssid[80] = {0};
  strcpy(default_ap_ssid, ESP_WIFI_SSID);
  strcat(default_ap_ssid, "_");
  strcat(default_ap_ssid, device_mac_end);

  esp_err_t ret = nvs_init();
  if (ret != ESP_OK) {
    return ret;
  }

  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));

  char ap_name[80] = {0};
  char ap_password[80] = {0};
  size_t len = sizeof(ap_name);
  ret = nvs_get_str(nvs_handle, "ap_name", ap_name, &len);
  if (ret == ESP_ERR_NVS_NOT_FOUND) {
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_name", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_password", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_wait_time", "0"));
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    ap_name[0] = '\0';
    ap_password[0] = '\0';
  } else {
    len = sizeof(ap_password);
    nvs_get_str(nvs_handle, "ap_password", ap_password, &len);
  }
  nvs_close(nvs_handle);

  const char *ssid = ap_name[0] ? ap_name : default_ap_ssid;
  const char *password = ap_name[0] ? ap_password : ESP_WIFI_PASS;

  wifi_config_t wifi_config = {
      .ap = {.ssid = ESP_WIFI_SSID,
             .ssid_len = 0,
             .password = ESP_WIFI_PASS,
             .max_connection = MAX_STA_CONN,
             .authmode = WIFI_AUTH_WPA_WPA2_PSK},
  };

  strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
  strncpy((char *)wifi_config.ap.password, password,
          sizeof(wifi_config.ap.password) - 1);
  wifi_config.ap.ssid_len = strlen((char *)wifi_config.ap.ssid);
  if (strlen((char *)wifi_config.ap.password) == 0) {
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
  esp_err_t start_ret = esp_wifi_start();
  if (start_ret != ESP_OK && start_ret != ESP_ERR_WIFI_CONN) {
    return start_ret;
  }

  esp_netif_ip_info_t ip_info;
  esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"),
                        &ip_info);

  char ip_addr[16];
  inet_ntoa_r(ip_info.ip.addr, ip_addr, sizeof(ip_addr));
  ESP_LOGI(TAG, "Set up softAP with IP: %s", ip_addr);
  ESP_LOGI(TAG, "wifi softAP finished. SSID:'%s'", ssid);
  return ESP_OK;
}

esp_err_t sx_ap_manager_start(void) {
  esp_err_t err = start_softap();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "启动AP失败: %s", esp_err_to_name(err));
    return err;
  }

  start_dns_server();
  s_ap_on = true;
  sx_network_manager_notify_ap_started();

  s_wait_time_sec = load_ap_wait_time_sec();
  if (s_wait_time_sec == 0) {
    ESP_LOGI(TAG, "AP配置为永远开启");
    return ESP_OK;
  }

  s_close_at_us = esp_timer_get_time() + ((int64_t)s_wait_time_sec * 1000000LL);
  s_close_pending = false;
  err = schedule_ap_close_timer(s_wait_time_sec);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "启动AP关闭定时器失败: %s", esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(TAG, "AP将在%u秒后关闭", (unsigned)s_wait_time_sec);
  return ESP_OK;
}

void sx_ap_manager_stop_ap(void) {
  if (!s_ap_on) {
    return;
  }

  if (s_ap_close_timer != NULL) {
    esp_timer_stop(s_ap_close_timer);
  }
  s_close_pending = false;

  stop_dns_server();

  wifi_mode_t target_mode = WIFI_MODE_STA;

  esp_err_t err = esp_wifi_set_mode(target_mode);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "关闭AP设置WiFi模式失败: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "AP已关闭，WiFi模式切换为STA，保留WiFi扫描能力");
  }

  s_ap_on = false;
  s_close_at_us = 0;
  sx_network_manager_notify_ap_stopped();
}

bool sx_ap_manager_is_on(void) { return s_ap_on; }

uint32_t sx_ap_manager_get_wait_time_sec(void) { return s_wait_time_sec; }

uint32_t sx_ap_manager_get_remaining_sec(void) {
  if (!s_ap_on || s_wait_time_sec == 0 || s_close_at_us == 0) {
    return 0;
  }
  int64_t remain_us = s_close_at_us - esp_timer_get_time();
  if (remain_us <= 0) {
    return 0;
  }
  return (uint32_t)((remain_us + 999999LL) / 1000000LL);
}
