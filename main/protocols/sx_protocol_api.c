#include "sx_protocol_api.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "sx_mqtt_client.h"
#include "sx_tcp_client.h"
#include "sx_tcp_server.h"
#include "sx_utils.h"
#include <limits.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "PROTOCOL_API";

static esp_err_t validate_payload(const uint8_t *data, size_t len) {
  if (data == NULL || len == 0 || len > INT_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

static bool nvs_read_string(nvs_handle_t handle, const char *key, char *buf,
                            size_t buf_len) {
  if (buf == NULL || buf_len == 0) {
    return false;
  }

  buf[0] = '\0';
  size_t len = buf_len;
  esp_err_t err = nvs_get_str(handle, key, buf, &len);
  if (err != ESP_OK) {
    return false;
  }

  buf[buf_len - 1] = '\0';
  return true;
}

static int read_tcp_mode_from_nvs(void) {
  int tcp_mode = TCP_MODE_SERVER;
  nvs_handle_t handle;

  if (nvs_open("nvs_namespace", NVS_READONLY, &handle) != ESP_OK) {
    return tcp_mode;
  }

  char tcpconn[8] = {0};
  if (nvs_read_string(handle, "tcpconn", tcpconn, sizeof(tcpconn)) &&
      tcpconn[0] != '\0') {
    tcp_mode = atoi(tcpconn);
  }

  nvs_close(handle);
  return tcp_mode;
}

esp_err_t sx_protocol_tcp_server_send(const uint8_t *data, size_t len) {
  esp_err_t err = validate_payload(data, len);
  if (err != ESP_OK) {
    return err;
  }

  tcpserver_message((char *)data, (int)len);
  return ESP_OK;
}

esp_err_t sx_protocol_tcp_client_send(const uint8_t *data, size_t len) {
  esp_err_t err = validate_payload(data, len);
  if (err != ESP_OK) {
    return err;
  }

  tcpclient_message((char *)data, (int)len);
  return ESP_OK;
}

esp_err_t sx_protocol_tcp_send(sx_protocol_tcp_target_t target,
                               const uint8_t *data, size_t len) {
  if (target == SX_PROTOCOL_TCP_SERVER) {
    return sx_protocol_tcp_server_send(data, len);
  }

  if (target == SX_PROTOCOL_TCP_CLIENT) {
    return sx_protocol_tcp_client_send(data, len);
  }

  if (target != SX_PROTOCOL_TCP_AUTO) {
    return ESP_ERR_INVALID_ARG;
  }

  int tcp_mode = read_tcp_mode_from_nvs();
  if (TCP_MODE_IS_CLIENT(tcp_mode)) {
    return sx_protocol_tcp_client_send(data, len);
  }
  return sx_protocol_tcp_server_send(data, len);
}

esp_err_t sx_protocol_mqtt_publish(const char *topic, const uint8_t *data,
                                   size_t len, int qos, bool retain) {
  esp_err_t err = validate_payload(data, len);
  if (err != ESP_OK || topic == NULL || topic[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }

  int msg_id =
      mqtt_publish_topic(topic, (const char *)data, (int)len, qos,
                         retain ? 1 : 0);
  return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t sx_protocol_http_post_configured(const uint8_t *data, size_t len,
                                           const char *content_type) {
  esp_err_t err = validate_payload(data, len);
  if (err != ESP_OK) {
    return err;
  }

  nvs_handle_t handle;
  err = nvs_open("nvs_namespace", NVS_READONLY, &handle);
  if (err != ESP_OK) {
    return err;
  }

  char use_http[8] = {0};
  char url[256] = {0};
  bool enabled = nvs_read_string(handle, "use_http", use_http,
                                 sizeof(use_http)) &&
                 atoi(use_http) == 1;
  bool has_url = nvs_read_string(handle, "http_url", url, sizeof(url)) &&
                 url[0] != '\0';
  nvs_close(handle);

  if (!enabled || !has_url) {
    ESP_LOGW(TAG, "HTTP is disabled or http_url is empty");
    return ESP_ERR_INVALID_STATE;
  }

  esp_http_client_config_t config = {
      .url = url,
      .timeout_ms = 5000,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    return ESP_FAIL;
  }

  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(
      client, "Content-Type",
      (content_type != NULL && content_type[0] != '\0') ? content_type
                                                        : "application/octet-stream");
  esp_http_client_set_post_field(client, (const char *)data, (int)len);

  err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "HTTP POST status=%d",
             esp_http_client_get_status_code(client));
  }

  esp_http_client_cleanup(client);
  return err;
}

esp_err_t sx_protocol_udp_send_to(const char *host, uint16_t port,
                                  const uint8_t *data, size_t len) {
  esp_err_t err = validate_payload(data, len);
  if (err != ESP_OK || host == NULL || host[0] == '\0' || port == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

  struct addrinfo hints = {
      .ai_family = AF_INET,
      .ai_socktype = SOCK_DGRAM,
      .ai_protocol = IPPROTO_UDP,
  };
  struct addrinfo *res = NULL;
  int gai_err = getaddrinfo(host, port_str, &hints, &res);
  if (gai_err != 0 || res == NULL) {
    ESP_LOGW(TAG, "UDP target resolve failed: %s:%u err=%d", host,
             (unsigned)port, gai_err);
    return ESP_ERR_NOT_FOUND;
  }

  int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (sock < 0) {
    freeaddrinfo(res);
    return ESP_FAIL;
  }

  ssize_t sent = sendto(sock, data, len, 0, res->ai_addr, res->ai_addrlen);
  close(sock);
  freeaddrinfo(res);

  if (sent < 0 || (size_t)sent != len) {
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t sx_protocol_send_tcp_server(const uint8_t *data, size_t len) {
  return sx_protocol_tcp_server_send(data, len);
}

esp_err_t sx_protocol_send_tcp_client(const uint8_t *data, size_t len) {
  return sx_protocol_tcp_client_send(data, len);
}
