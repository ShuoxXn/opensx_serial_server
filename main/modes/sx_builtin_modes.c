#include "sx_work_mode.h"

#include "sx_work_mode_ids.h"
#include "mode_tcp_transparent.h"
#include "mode_protocol_test.h"

esp_err_t sx_work_mode_register_builtin_modes(void) {
  static bool s_registered = false;
  if (s_registered) {
    return ESP_OK;
  }

  static const sx_work_mode_config_ops_t tcp_transparent_config = {
      .load = mode_tcp_transparent_load_config,
      .save = mode_tcp_transparent_save_config,
  };

  static const sx_work_mode_t tcp_transparent_mode = {
      .name = WORK_MODE_TCP_TRANSPARENT,
      .label = "TCP透传",
      .start = mode_tcp_transparent_start,
      .stop = mode_tcp_transparent_stop,
      .after_start = NULL,
      .on_uart_data = mode_tcp_transparent_on_uart_data,
      .config = &tcp_transparent_config,
  };

  static const sx_work_mode_config_ops_t protocol_test_config = {
      .load = mode_protocol_test_load_config,
      .save = mode_protocol_test_save_config,
  };

  static const sx_work_mode_t protocol_test_mode = {
      .name = WORK_MODE_PROTOCOL_TEST,
      .label = "协议测试",
      .start = mode_protocol_test_start,
      .stop = mode_protocol_test_stop,
      .after_start = NULL,
      .on_uart_data = NULL,
      .on_tcp_data = mode_protocol_test_on_tcp_data,
      .on_mqtt_data = mode_protocol_test_on_mqtt_data,
      .config = &protocol_test_config,
  };

  esp_err_t err = sx_work_mode_register(&tcp_transparent_mode);
  if (err != ESP_OK) {
    return err;
  }

  err = sx_work_mode_register(&protocol_test_mode);
  if (err != ESP_OK) {
    return err;
  }

  s_registered = true;
  return ESP_OK;
}
