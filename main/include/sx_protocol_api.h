#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  SX_PROTOCOL_TCP_AUTO = 0,
  SX_PROTOCOL_TCP_SERVER,
  SX_PROTOCOL_TCP_CLIENT,
} sx_protocol_tcp_target_t;

esp_err_t sx_protocol_tcp_send(sx_protocol_tcp_target_t target,
                               const uint8_t *data, size_t len);
esp_err_t sx_protocol_tcp_server_send(const uint8_t *data, size_t len);
esp_err_t sx_protocol_tcp_client_send(const uint8_t *data, size_t len);
esp_err_t sx_protocol_mqtt_publish(const char *topic, const uint8_t *data,
                                   size_t len, int qos, bool retain);
esp_err_t sx_protocol_http_post_configured(const uint8_t *data, size_t len,
                                           const char *content_type);
esp_err_t sx_protocol_udp_send_to(const char *host, uint16_t port,
                                  const uint8_t *data, size_t len);

/* Backward-compatible aliases. New work modes should use the APIs above. */
esp_err_t sx_protocol_send_tcp_server(const uint8_t *data, size_t len);
esp_err_t sx_protocol_send_tcp_client(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
