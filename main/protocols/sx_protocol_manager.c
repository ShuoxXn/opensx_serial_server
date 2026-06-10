#include "sx_protocol_manager.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_mqtt_client.h"
#include "sx_tcp_client.h"
#include "sx_tcp_server.h"
#include "sx_udp_server.h"
#include "sx_utils.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "PROTOCOL_MANAGER";

static bool s_local_started = false;
static bool s_external_started = false;
static bool s_tcp_server_started = false;
static bool s_tcp_client_started = false;
static bool s_mqtt_started = false;

static bool nvs_read_string(nvs_handle_t handle, const char *key, char *buf,
                            size_t buf_len) {
  size_t len = buf_len;
  if (buf_len == 0) {
    return false;
  }
  buf[0] = '\0';
  esp_err_t err = nvs_get_str(handle, key, buf, &len);
  if (err != ESP_OK) {
    return false;
  }
  buf[buf_len - 1] = '\0';
  return true;
}

static void start_local_protocols(void) {
  if (s_local_started) {
    ESP_LOGD(TAG, "本地协议已启动，跳过重复启动");
    return;
  }

  ESP_LOGI(TAG, "启动本地协议");
  start_udp_server();

  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "打开NVS读取TCP配置失败: %s", esp_err_to_name(err));
    s_local_started = true;
    return;
  }

  char use_tcp[8] = {0};
  char tcpconn[8] = {0};
  if (nvs_read_string(nvs_handle, "use_tcp", use_tcp, sizeof(use_tcp)) &&
      atoi(use_tcp) == 1) {
    int tcp_mode = TCP_MODE_CLIENT;
    if (nvs_read_string(nvs_handle, "tcpconn", tcpconn, sizeof(tcpconn)) &&
        tcpconn[0] != '\0') {
      tcp_mode = atoi(tcpconn);
    }

    if (TCP_MODE_IS_SERVER(tcp_mode) && !s_tcp_server_started) {
      ESP_LOGI(TAG, "启动TCP Server协议");
      start_tcp_server();
      s_tcp_server_started = true;
    }

    if (TCP_MODE_IS_CLIENT(tcp_mode) && !s_tcp_client_started) {
      ESP_LOGI(TAG, "启动TCP Client协议");
      start_tcp_client();
      s_tcp_client_started = true;
    }
  }

  nvs_close(nvs_handle);
  s_local_started = true;
}

static void start_external_protocols(void) {
  if (s_external_started) {
    ESP_LOGD(TAG, "外网协议已启动，跳过重复启动");
    return;
  }

  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "打开NVS读取MQTT配置失败: %s", esp_err_to_name(err));
    return;
  }

  char use_mqtt[8] = {0};
  char mqtt_type[8] = {0};
  if (nvs_read_string(nvs_handle, "use_mqtt", use_mqtt, sizeof(use_mqtt)) &&
      atoi(use_mqtt) == 1 &&
      nvs_read_string(nvs_handle, "mqtt_type", mqtt_type, sizeof(mqtt_type)) &&
      atoi(mqtt_type) == 0 && !s_mqtt_started) {
    ESP_LOGI(TAG, "启动MQTT协议");
    if (mqtt_app_start()) {
      s_mqtt_started = true;
    } else {
      ESP_LOGW(TAG, "MQTT协议启动失败，等待下次外网检测重试");
    }
  }

  nvs_close(nvs_handle);
  s_external_started = s_mqtt_started;
}

static void stop_local_protocols(void) {
  if (!s_local_started && !s_tcp_server_started && !s_tcp_client_started) {
    return;
  }

  ESP_LOGI(TAG, "停止本地协议");
  kill_udp_server();
  stop_tcp_client();
  stop_tcp_server();

  s_local_started = false;
  s_tcp_server_started = false;
  s_tcp_client_started = false;
}

static void stop_external_protocols(void) {
  if (!s_external_started && !s_mqtt_started) {
    return;
  }

  ESP_LOGI(TAG, "外网协议离线");
  mqtt_app_stop();
  s_external_started = false;
  s_mqtt_started = false;
}

void sx_protocol_manager_init(void) {
  s_local_started = false;
  s_external_started = false;
  s_tcp_server_started = false;
  s_tcp_client_started = false;
  s_mqtt_started = false;
}

void sx_protocol_manager_on_network_state(sx_net_state_t old_state,
                                          sx_net_state_t new_state) {
  if (old_state == new_state) {
    if (new_state == SX_NET_STATE_LOCAL) {
      start_local_protocols();
    } else if (new_state == SX_NET_STATE_EXTERNAL) {
      start_local_protocols();
      start_external_protocols();
    }
    return;
  }

  ESP_LOGI(TAG, "网络状态变化，协议调度: %s -> %s",
           sx_network_state_name(old_state), sx_network_state_name(new_state));

  if (new_state == SX_NET_STATE_DOWN) {
    stop_external_protocols();
    stop_local_protocols();
    return;
  }

  if (new_state == SX_NET_STATE_LOCAL) {
    if (old_state == SX_NET_STATE_DOWN) {
      start_local_protocols();
    }
    if (old_state == SX_NET_STATE_EXTERNAL) {
      stop_external_protocols();
    }
    return;
  }

  if (new_state == SX_NET_STATE_EXTERNAL) {
    if (old_state == SX_NET_STATE_DOWN) {
      start_local_protocols();
    }
    start_external_protocols();
  }
}

void sx_protocol_manager_stop_all(void) {
  stop_external_protocols();
  stop_local_protocols();
}
