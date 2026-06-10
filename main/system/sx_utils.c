/* Common utility functions. */

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "math.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_gpio.h"
#include "sx_http_client.h"
#include "sx_mqtt_client.h"
#include "sx_network_manager.h"
#include "sx_tcp_client.h"
#include "sx_tcp_server.h"

#include "esp_mac.h"
#include "sx_async_uart.h"
#include "sx_udp_server.h"

#include "sx_timer_tasks.h"
#include "sx_web_server.h"
#include "sx_work_mode.h"
#include "sx_work_mode_ids.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/sys.h"
#include "ping/ping_sock.h"
#include <sys/time.h>
#include <unistd.h>
// 添加包含自己的头文件
#include "sx_utils.h"

// 函数声明
esp_err_t nvs_safe_reset(void);

// 添加ping网关函数声明
bool ping_gateway(const char* gateway_ip);
TaskHandle_t xHandleTask1, xHandleTask2, xHandleTask3 = NULL;
char messageid[48] = {0};
// 添加全局变量，用于判断网络连接状态
bool is_network_connected = false;

// 声明外部变量和函数，用于LED定时器
extern esp_timer_handle_t time_sync_led_timer;
extern void time_sync_led_callback(void *arg);



// 添加内网模式标志
bool is_intranet_mode = false;

// 添加网络类型标志 - 用于区分WiFi和以太网连接
bool is_wifi_active = false;  // true: WiFi连接中, false: 以太网连接中

static const char *RESET_TAG = "FACTORY_RESET";

bool is_valid_work_mode(const char *work_mode) {
  return sx_work_mode_is_valid(work_mode);
}

esp_err_t nvs_get_work_mode(nvs_handle_t handle, char *work_mode, size_t size) {
  if (work_mode == NULL || size == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  work_mode[0] = '\0';
  size_t required_size = size;
  esp_err_t err = nvs_get_str(handle, WORK_MODE_NVS_KEY, work_mode, &required_size);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    snprintf(work_mode, size, "%s", WORK_MODE_DEFAULT);
    return ESP_ERR_NVS_NOT_FOUND;
  }
  if (err == ESP_ERR_NVS_INVALID_LENGTH) {
    snprintf(work_mode, size, "%s", WORK_MODE_DEFAULT);
    return ESP_ERR_INVALID_STATE;
  }
  if (err != ESP_OK) {
    return err;
  }
  if (!is_valid_work_mode(work_mode)) {
    snprintf(work_mode, size, "%s", WORK_MODE_DEFAULT);
    return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

esp_err_t nvs_set_work_mode(nvs_handle_t handle, const char *work_mode) {
  if (!is_valid_work_mode(work_mode)) {
    return ESP_ERR_INVALID_ARG;
  }
  return nvs_set_str(handle, WORK_MODE_NVS_KEY, work_mode);
}

static void set_all_leds(bool on) {
  if (on) {
    LED_DAT_ON();
    LED_WIFI_ON();
    LED_LAN_ON();
  } else {
    LED_DAT_OFF();
    LED_WIFI_OFF();
    LED_LAN_OFF();
  }
}

void perform_factory_reset(bool preserve_work_mode) {
  char current_work_mode[32] = WORK_MODE_DEFAULT;
  char device_sn[32] = {0};
  bool has_device_sn = false;
  nvs_handle_t storage_handle;
  esp_err_t err;

  if (preserve_work_mode) {
    err = nvs_open("nvs_namespace", NVS_READWRITE, &storage_handle);
    if (err == ESP_OK) {
      if (nvs_get_work_mode(storage_handle, current_work_mode,
                            sizeof(current_work_mode)) != ESP_OK) {
        strcpy(current_work_mode, WORK_MODE_DEFAULT);
      }
      ESP_LOGI(RESET_TAG, "保存当前工作模式: %s", current_work_mode);
      nvs_close(storage_handle);
    } else {
      ESP_LOGW(RESET_TAG, "读取工作模式失败: %s", esp_err_to_name(err));
    }
  }

  // 保存设备SN，避免重置后被清空
  nvs_handle_t nvs_handle;
  err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
  if (err == ESP_OK) {
    size_t sn_len = sizeof(device_sn);
    err = nvs_get_str(nvs_handle, "device_sn", device_sn, &sn_len);
    if (err == ESP_OK && device_sn[0] != '\0') {
      has_device_sn = true;
      ESP_LOGI(RESET_TAG, "保存设备SN: %s", device_sn);
    } else {
      ESP_LOGW(RESET_TAG, "未读取到device_sn");
    }
    nvs_close(nvs_handle);
  } else {
    ESP_LOGW(RESET_TAG, "读取device_sn失败: %s", esp_err_to_name(err));
  }

  if (time_sync_led_timer != NULL) {
    esp_err_t timer_err = esp_timer_stop(time_sync_led_timer);
    if (timer_err != ESP_OK && timer_err != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(RESET_TAG, "停止LED定时器失败: %s", esp_err_to_name(timer_err));
    }
  }

  ESP_LOGI(RESET_TAG, "开始擦除NVS...");
  ESP_ERROR_CHECK(nvs_flash_erase());
  vTaskDelay(pdMS_TO_TICKS(100));
  ESP_ERROR_CHECK(nvs_flash_init());

  if (has_device_sn) {
    err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
      err = nvs_set_str(nvs_handle, "device_sn", device_sn);
      if (err != ESP_OK) {
        ESP_LOGW(RESET_TAG, "恢复device_sn失败: %s", esp_err_to_name(err));
      } else {
        nvs_commit(nvs_handle);
        ESP_LOGI(RESET_TAG, "恢复设备SN: %s", device_sn);
      }
      nvs_close(nvs_handle);
    } else {
      ESP_LOGW(RESET_TAG, "写回device_sn失败: %s", esp_err_to_name(err));
    }
  }

  if (preserve_work_mode) {
    err = nvs_open("nvs_namespace", NVS_READWRITE, &storage_handle);
    if (err == ESP_OK) {
      nvs_set_work_mode(storage_handle, current_work_mode);
      nvs_commit(storage_handle);
      nvs_close(storage_handle);
      ESP_LOGI(RESET_TAG, "恢复工作模式: %s", current_work_mode);
    } else {
      ESP_LOGW(RESET_TAG, "恢复工作模式失败: %s", esp_err_to_name(err));
    }
  }

  ESP_LOGI(RESET_TAG, "所有LED闪烁三次表示重置成功");
  for (int i = 0; i < 3; i++) {
    set_all_leds(true);
    vTaskDelay(pdMS_TO_TICKS(500));
    set_all_leds(false);
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  ESP_LOGI(RESET_TAG, "设备即将重启...");
  esp_restart();
}

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

// 函数用于将十六进制字符转换为对应的十进制数
int hexCharToInt(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return 0;
}

// URL 解码函数
void urlDecode(char *src, char *dest) {
  char *pSrc = src;
  char *pDest = dest;

  while (*pSrc) {
    if (*pSrc == '%') {
      if (pSrc[1] && pSrc[2]) {
        *pDest++ = hexCharToInt(pSrc[1]) * 16 + hexCharToInt(pSrc[2]);
        pSrc += 3;
      }
    } else if (*pSrc == '+') { // 处理空格（空格有时被编码为+）
      *pDest++ = ' ';
      pSrc++;
    } else {
      *pDest++ = *pSrc++;
    }
  }

  *pDest = '\0';
}

void save_form_data(const char *query) {

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  // 打开NVS命名空间
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));
  char *params = strdup(query); // 为了不修改原字符串
  char *param;

  // 按 & 分割字符串
  param = strtok(params, "&");
  while (param != NULL) {
    // 找到键和值的分隔符 '='
    char *delimiter = strchr(param, '=');
    if (delimiter != NULL) {
      // 分割键和值
      *delimiter = '\0';
      char *key = param;
      char *value = delimiter + 1;
      // 打印键和值
      printf("Key: %s, Value: %s\n", key, value);
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, key, value));
    }
    // 继续获取下一个参数
    param = strtok(NULL, "&");
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
  }

  nvs_close(nvs_handle);

  free(params); // 释放复制的字符串
}

char *reset_reason_to_string(esp_reset_reason_t reason) {
  switch (reason) {
  case ESP_RST_UNKNOWN:
    return "Unknown";
  case ESP_RST_POWERON:
    return "Power-on reset";
  case ESP_RST_EXT:
    return "External reset";
  case ESP_RST_SW:
    return "Software reset";
  case ESP_RST_PANIC:
    return "Exception/panic reset";
  case ESP_RST_INT_WDT:
    return "Watchdog reset";
  case ESP_RST_TASK_WDT:
    return "Task watchdog reset";
  case ESP_RST_WDT:
    return "Other watchdog reset";
  case ESP_RST_DEEPSLEEP:
    return "Exited deep sleep";
  case ESP_RST_BROWNOUT:
    return "Brownout reset";
  case ESP_RST_SDIO:
    return "SDIO reset";
  default:
    return "Unknown";
  }
}

uint16_t ModbusCRC16(uint8_t *data, uint16_t length) {
  uint16_t crc = 0xFFFF;
  for (int pos = 0; pos < length; pos++) {
    crc ^= (uint16_t)data[pos];    // XOR byte into least sig. byte of crc
    for (int i = 8; i != 0; i--) { // Loop over each bit
      if ((crc & 0x0001) != 0) {   // If the LSB is set
        crc >>= 1;                 // Shift right and XOR 0xA001
        crc ^= 0xA001;
      } else       // Else LSB is not set
        crc >>= 1; // Just shift right
    }
  }
  return crc;
}

// 初始化 NVS
esp_err_t nvs_init() {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  return ret;
}
// 添加安全重置 NVS 的函数实现
esp_err_t nvs_safe_reset(void) {
  const char *TAG = "nvs_safe_reset";
  esp_err_t err;

  // 检查NVS是否已初始化
  nvs_stats_t nvs_stats;
  err = nvs_get_stats(NULL, &nvs_stats);
  if (err == ESP_ERR_NVS_NOT_INITIALIZED) {
    ESP_LOGI(TAG, "NVS not initialized, initializing first");
    err = nvs_flash_init();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
      if (err != ESP_ERR_NVS_NO_FREE_PAGES &&
          err != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return err;
      }

      // 如果是特定错误，尝试擦除后重新初始化
      ESP_LOGI(TAG, "Erasing NVS flash...");
      err = nvs_flash_erase();
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(err));
        return err;
      }

      err = nvs_flash_init();
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS after erase: %s",
                 esp_err_to_name(err));
        return err;
      }
    }
  } else if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get NVS stats: %s", esp_err_to_name(err));
    return err;
  }

  // 打开 NVS 存储
  nvs_handle_t nvs_handle;
  err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
    return err;
  }

  // 读取需要保留的值
  char device_sn[32] = {0};
  char device_type[32] = {0};
  size_t sn_len = sizeof(device_sn);
  size_t type_len = sizeof(device_type);

  err = nvs_get_str(nvs_handle, "device_sn", device_sn, &sn_len);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "No device_sn found in NVS");
  }

  err = nvs_get_str(nvs_handle, "device_type", device_type, &type_len);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "No device_type found in NVS");
  }

  // 关闭 NVS 句柄
  nvs_close(nvs_handle);

  // 擦除 NVS
  err = nvs_flash_erase();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error erasing NVS: %s", esp_err_to_name(err));
    return err;
  }

  // 重新初始化 NVS
  err = nvs_flash_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error initializing NVS: %s", esp_err_to_name(err));
    return err;
  }

  // 如果之前有保存的值，则恢复它们
  if (device_sn[0] != '\0' || device_type[0] != '\0') {
    err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Error opening NVS handle after reset: %s",
               esp_err_to_name(err));
      return err;
    }

    if (device_sn[0] != '\0') {
      err = nvs_set_str(nvs_handle, "device_sn", device_sn);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error restoring device_sn: %s", esp_err_to_name(err));
      }
    }

    if (device_type[0] != '\0') {
      err = nvs_set_str(nvs_handle, "device_type", device_type);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error restoring device_type: %s", esp_err_to_name(err));
      }
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Error committing NVS changes: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
  }

  ESP_LOGI(TAG, "NVS reset completed successfully");
  return ESP_OK;
}

// 新增：获取NVS使用率的函数
float get_nvs_usage_percentage(void) {
  nvs_stats_t nvs_stats;
  esp_err_t err = nvs_get_stats(NULL, &nvs_stats);
  if (err == ESP_OK && nvs_stats.total_entries > 0) {
    return (float)nvs_stats.used_entries / nvs_stats.total_entries * 100.0f;
  }
  return -1.0f; // 表示获取失败
}

void save_to_nvs(const char *json_string) {
  nvs_handle_t nvs_handle;
  esp_err_t err;
  // 打开 NVS
  err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    return;
  } else {
    printf("Opened NVS handle\n");
  }

  bool updated = false;

  if (json_string != NULL) {
    const char *trim = json_string;
    while (trim && *trim && isspace((unsigned char)*trim)) {
      trim++;
    }

    bool parsed_json = false;

    if (trim && *trim == '{') {
      cJSON *json = cJSON_Parse(trim);
      if (json != NULL) {
        parsed_json = true;
        cJSON *current_element = NULL;
        cJSON_ArrayForEach(current_element, json) {
          if (current_element->string == NULL) {
            continue;
          }

          const char *value_ptr = NULL;
          char number_buffer[32];

          if (cJSON_IsString(current_element) &&
              current_element->valuestring != NULL) {
            value_ptr = current_element->valuestring;
          } else if (cJSON_IsNumber(current_element)) {
            snprintf(number_buffer, sizeof(number_buffer), "%.15g",
                     current_element->valuedouble);
            value_ptr = number_buffer;
          } else if (cJSON_IsBool(current_element)) {
            value_ptr = cJSON_IsTrue(current_element) ? "1" : "0";
          } else {
            continue;
          }

          ESP_ERROR_CHECK(
              nvs_set_str(nvs_handle, current_element->string, value_ptr));
          updated = true;
        }
        cJSON_Delete(json);
      } else {
        ESP_LOGW("save_to_nvs", "Failed to parse JSON payload");
      }
    }

    if (!parsed_json) {
      char *params = strdup(json_string);
      if (params != NULL) {
        char *param = strtok(params, "&");
        while (param != NULL) {
          char *delimiter = strchr(param, '=');
          if (delimiter != NULL) {
            *delimiter = '\0';
            char *key = param;
            char *value = delimiter + 1;
            if (value == NULL) {
              value = "";
            }
            ESP_ERROR_CHECK(nvs_set_str(nvs_handle, key, value));
            updated = true;
          }
          param = strtok(NULL, "&");
        }
        free(params);
      }
    }
  }

  if (updated) {
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
  }

  nvs_close(nvs_handle);
}

char *send_device_info(uint8_t *u_data, int idx) {
  // 创建根对象
  cJSON *root = cJSON_CreateObject();
  // 添加基础键值对
  cJSON_AddStringToObject(root, "messageid", messageid);
  cJSON_AddNumberToObject(root, "code", 200);
  cJSON_AddStringToObject(root, "type", DEVICE_TYPE);

  uint64_t time_since_boot = esp_timer_get_time();
  uint64_t time_in_s = time_since_boot / 1000000;
  // 添加 "data" 对象
  cJSON *data = cJSON_CreateObject();
  cJSON *net = cJSON_CreateObject();
  cJSON *sys = cJSON_CreateObject();
  cJSON *protocol = cJSON_CreateObject();
  cJSON *serial = cJSON_CreateObject();

  // char *response = tx_task(u_data, idx);
  // char *response = "";
  tx_task(u_data, idx);
  // cJSON_AddStringToObject(root, "resopnse", device_response);

  cJSON_AddItemToObject(root, "data", data);
  // free(response);

  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));
  size_t use_mqtt, use_tcp, use_http, tcpconn, netconn, wifi_ssid, is_dhcp,
      baud_rate, data_bit, check_bit, stop_bit = 0;
  ret = nvs_get_str(nvs_handle, "netconn", NULL, &netconn);
  nvs_get_str(nvs_handle, "is_dhcp", NULL, &is_dhcp);
  nvs_get_str(nvs_handle, "use_mqtt", NULL, &use_mqtt);
  nvs_get_str(nvs_handle, "use_tcp", NULL, &use_tcp);

  nvs_get_str(nvs_handle, "use_http", NULL, &use_http);
  nvs_get_str(nvs_handle, "tcpconn", NULL, &tcpconn);

  nvs_get_str(nvs_handle, "wifi_ssid", NULL, &wifi_ssid);
  nvs_get_str(nvs_handle, "baud_rate", NULL, &baud_rate);
  nvs_get_str(nvs_handle, "data_bit", NULL, &data_bit);
  nvs_get_str(nvs_handle, "check_bit", NULL, &check_bit);
  nvs_get_str(nvs_handle, "stop_bit", NULL, &stop_bit);
  switch (ret) {
  case ESP_OK:
    printf("protocol set success");
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_mqtt", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_tcp", ""));

    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_http", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcpconn", ""));

    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "netconn", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_ssid", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "is_dhcp", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "baud_rate", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "data_bit", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "check_bit", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "stop_bit", ""));
    break;
  default:
    break;
  }
  if (ret != ESP_OK) {
    printf("Error getting size of 'my_key': %s\n", esp_err_to_name(ret));
  } else {
    char *nvs_use_mqtt = malloc(use_mqtt);
    char *nvs_use_tcp = malloc(use_tcp);

    char *nvs_use_http = malloc(use_http);
    char *nvs_tcpconn = malloc(tcpconn);

    char *nvs_netconn = malloc(netconn);
    char *nvs_wifi_ssid = malloc(wifi_ssid);
    char *nvs_is_dhcp = malloc(is_dhcp);
    char *nvs_baud_rate = malloc(baud_rate);
    char *nvs_data_bit = malloc(data_bit);
    char *nvs_check_bit = malloc(check_bit);
    char *nvs_stop_bit = malloc(stop_bit);

    if (nvs_netconn == NULL) {
      printf("Memory allocation failed\n");
    } else {
      ret = nvs_get_str(nvs_handle, "netconn", nvs_netconn, &netconn);
      nvs_get_str(nvs_handle, "use_mqtt", nvs_use_mqtt, &use_mqtt);
      nvs_get_str(nvs_handle, "use_tcp", nvs_use_tcp, &use_tcp);

      nvs_get_str(nvs_handle, "use_http", nvs_use_http, &use_http);
      nvs_get_str(nvs_handle, "tcpconn", nvs_tcpconn, &tcpconn);

      nvs_get_str(nvs_handle, "wifi_ssid", nvs_wifi_ssid, &wifi_ssid);
      nvs_get_str(nvs_handle, "is_dhcp", nvs_is_dhcp, &is_dhcp);
      nvs_get_str(nvs_handle, "baud_rate", nvs_baud_rate, &baud_rate);
      nvs_get_str(nvs_handle, "data_bit", nvs_data_bit, &data_bit);
      nvs_get_str(nvs_handle, "check_bit", nvs_check_bit, &check_bit);
      nvs_get_str(nvs_handle, "stop_bit", nvs_stop_bit, &stop_bit);

      if (ret == ESP_OK) {
        // 添加 "net" 对象
        cJSON_AddNumberToObject(serial, "baud_rate", atoi(nvs_baud_rate));
        cJSON_AddNumberToObject(serial, "data_bit", atoi(nvs_data_bit));

        cJSON_AddNumberToObject(serial, "stop_bit", atof(nvs_stop_bit));
        cJSON_AddStringToObject(serial, "check_bit", nvs_check_bit);
        cJSON_AddItemToObject(root, "serial", serial);

        if (atoi(nvs_netconn) == 2) {
          cJSON_AddStringToObject(net, "connmethed", "wifi");
          cJSON_AddStringToObject(net, "ssid", nvs_wifi_ssid);
          if (atoi(nvs_is_dhcp) == 2) {
            cJSON_AddNumberToObject(net, "dhcp", 0);
          } else {
            cJSON_AddNumberToObject(net, "dhcp", 1);
          }
        } else {
          cJSON_AddStringToObject(net, "connmethed", "eth");
          cJSON_AddStringToObject(net, "ssid", "---");
          cJSON_AddNumberToObject(net, "dhcp", 1);
        }
        cJSON_AddStringToObject(net, "ip", got_ip_addrs);
        cJSON_AddStringToObject(net, "netmask", got_ip_netmask);
        cJSON_AddStringToObject(net, "mask", got_ip_netmask);
        cJSON_AddStringToObject(net, "gateway", got_ip_gw);
        cJSON_AddItemToObject(root, "net", net);

        // 添加 "sys" 对象

        cJSON_AddStringToObject(sys, "version", VERSION);

        cJSON_AddNumberToObject(sys, "runtime", time_in_s);
        cJSON_AddStringToObject(sys, "eth_mac", device_eth_mac);
        cJSON_AddStringToObject(sys, "sta_mac", device_sta_mac);
        cJSON_AddItemToObject(root, "sys", sys);

        // 添加 "protocol" 对象

        cJSON_AddNumberToObject(protocol, "mqtt", atoi(nvs_use_mqtt));
        cJSON_AddNumberToObject(protocol, "http", atoi(nvs_use_http));
        int tcp_mode = (nvs_tcpconn && nvs_tcpconn[0] != '\0')
                           ? atoi(nvs_tcpconn)
                           : TCP_MODE_CLIENT;
        if (atoi(nvs_use_tcp) == 1) {
          if (TCP_MODE_IS_SERVER(tcp_mode)) {
            cJSON_AddNumberToObject(protocol, "tcpserver", 1);
            cJSON_AddNumberToObject(protocol, "tcpclient", 0);
          } else if (TCP_MODE_IS_CLIENT(tcp_mode)) {
            cJSON_AddNumberToObject(protocol, "tcpserver", 0);
            cJSON_AddNumberToObject(protocol, "tcpclient", 1);
          } else {
            cJSON_AddNumberToObject(protocol, "tcpserver", 0);
            cJSON_AddNumberToObject(protocol, "tcpclient", 0);
          }
        } else {
          cJSON_AddNumberToObject(protocol, "tcpserver", 0);
          cJSON_AddNumberToObject(protocol, "tcpclient", 0);
        }

        cJSON_AddItemToObject(root, "protocol", protocol);
      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }

      free(nvs_use_mqtt);
      free(nvs_use_tcp);

      free(nvs_use_http);
      free(nvs_tcpconn);

      free(nvs_netconn);
      free(nvs_wifi_ssid);
      free(nvs_is_dhcp);
      free(nvs_baud_rate);
      free(nvs_data_bit);
      free(nvs_check_bit);
      free(nvs_stop_bit);
    }
  }
  nvs_close(nvs_handle);
  char *jsonString = cJSON_Print(root);
  char *result = strdup(jsonString);
  free(jsonString);
  cJSON_Delete(root);
  return result;
}

char *build_mqtt_config_json(void) {
  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    printf("Failed to create JSON root object\n");
    return NULL;
  }

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));

  size_t mqtt_clientid = 0, mqtt_server = 0, mqtt_port = 0, mqtt_username = 0,
         mqtt_password = 0, mqtt_pub_topic = 0, mqtt_sub_topic = 0,
         device_type = 0, host_names, device_sn = 0;

  nvs_get_str(nvs_handle, "device_type", NULL, &device_type);
  nvs_get_str(nvs_handle, "host_names", NULL, &host_names);
  nvs_get_str(nvs_handle, "device_sn", NULL, &device_sn);
  nvs_get_str(nvs_handle, "mqtt_clientid", NULL, &mqtt_clientid);
  nvs_get_str(nvs_handle, "mqtt_server", NULL, &mqtt_server);
  nvs_get_str(nvs_handle, "mqtt_port", NULL, &mqtt_port);
  nvs_get_str(nvs_handle, "mqtt_username", NULL, &mqtt_username);
  nvs_get_str(nvs_handle, "mqtt_password", NULL, &mqtt_password);
  nvs_get_str(nvs_handle, "mqtt_pub_topic", NULL, &mqtt_pub_topic);
  nvs_get_str(nvs_handle, "mqtt_sub_topic", NULL, &mqtt_sub_topic);

  if (ret != ESP_OK) {
    printf("Error getting size of NVS strings: %s\n", esp_err_to_name(ret));
    cJSON_Delete(root);
    return NULL;
  }

  // 分配内存
  char *nvs_device_type = malloc(device_type);
  char *nvs_mqtt_clientid = malloc(mqtt_clientid);
  char *nvs_mqtt_server = malloc(mqtt_server);
  char *nvs_mqtt_port = malloc(mqtt_port);
  char *nvs_mqtt_username = malloc(mqtt_username);
  char *nvs_mqtt_password = malloc(mqtt_password);
  char *nvs_mqtt_pub_topic = malloc(mqtt_pub_topic);
  char *nvs_mqtt_sub_topic = malloc(mqtt_sub_topic);
  char *nvs_host_names = malloc(host_names);
  char *nvs_device_sn = malloc(device_sn);

  if (nvs_device_type == NULL || nvs_mqtt_clientid == NULL ||
      nvs_mqtt_server == NULL || nvs_mqtt_port == NULL ||
      nvs_mqtt_username == NULL || nvs_mqtt_password == NULL ||
      nvs_mqtt_pub_topic == NULL || nvs_mqtt_sub_topic == NULL ||
      nvs_host_names == NULL || nvs_device_sn == NULL) {
    // 清理已分配的内存
    if (nvs_device_type)
      free(nvs_device_type);
    if (nvs_mqtt_clientid)
      free(nvs_mqtt_clientid);
    if (nvs_mqtt_server)
      free(nvs_mqtt_server);
    if (nvs_mqtt_port)
      free(nvs_mqtt_port);
    if (nvs_mqtt_username)
      free(nvs_mqtt_username);
    if (nvs_mqtt_password)
      free(nvs_mqtt_password);
    if (nvs_mqtt_pub_topic)
      free(nvs_mqtt_pub_topic);
    if (nvs_mqtt_sub_topic)
      free(nvs_mqtt_sub_topic);
    if (nvs_host_names)
      free(nvs_host_names);
    if (nvs_device_sn)
      free(nvs_device_sn);
    cJSON_Delete(root);
    nvs_close(nvs_handle);
    return NULL;
  }

  uint8_t sta_mac[6] = {0};
  uint8_t eth_mac[6] = {0};

  esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);
  esp_read_mac(eth_mac, ESP_MAC_ETH);

  char d_sta_mac[24];
  char d_eth_mac[24];
  sprintf(d_sta_mac, "%02X:%02X:%02X:%02X:%02X:%02X", sta_mac[0], sta_mac[1],
          sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
  sprintf(d_eth_mac, "%02X:%02X:%02X:%02X:%02X:%02X", eth_mac[0], eth_mac[1],
          eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);

  nvs_get_str(nvs_handle, "mqtt_clientid", nvs_mqtt_clientid, &mqtt_clientid);
  nvs_get_str(nvs_handle, "mqtt_server", nvs_mqtt_server, &mqtt_server);
  nvs_get_str(nvs_handle, "mqtt_port", nvs_mqtt_port, &mqtt_port);
  nvs_get_str(nvs_handle, "mqtt_username", nvs_mqtt_username, &mqtt_username);
  nvs_get_str(nvs_handle, "mqtt_password", nvs_mqtt_password, &mqtt_password);
  nvs_get_str(nvs_handle, "mqtt_pub_topic", nvs_mqtt_pub_topic,
              &mqtt_pub_topic);
  nvs_get_str(nvs_handle, "mqtt_sub_topic", nvs_mqtt_sub_topic,
              &mqtt_sub_topic);
  nvs_get_str(nvs_handle, "device_type", nvs_device_type, &device_type);
  nvs_get_str(nvs_handle, "host_names", nvs_host_names, &host_names);
  nvs_get_str(nvs_handle, "device_sn", nvs_device_sn, &device_sn);
  if (ret == ESP_OK) {
    // 添加设备基本信息
    cJSON_AddStringToObject(root, "device_type", nvs_device_type);
    cJSON_AddStringToObject(root, "device_name", nvs_host_names);
    cJSON_AddStringToObject(root, "sta_mac", d_sta_mac);
    cJSON_AddStringToObject(root, "eth_mac", d_eth_mac);
    cJSON_AddStringToObject(root, "version", VERSION);
    cJSON_AddStringToObject(root, "device_sn", nvs_device_sn);
    // cJSON_AddStringToObject(root, "thsensor", thsensor);
    // cJSON_AddStringToObject(root, "psensor", psensor);

    // 创建MQTT子对象
    cJSON *mqtt = cJSON_CreateObject();
    if (mqtt != NULL) {
      // 添加MQTT配置信息到mqtt子对象
      cJSON_AddStringToObject(mqtt, "client_id", nvs_mqtt_clientid);
      cJSON_AddStringToObject(mqtt, "server", nvs_mqtt_server);
      cJSON_AddStringToObject(mqtt, "port", nvs_mqtt_port);
      cJSON_AddStringToObject(mqtt, "username", nvs_mqtt_username);
      cJSON_AddStringToObject(mqtt, "password", nvs_mqtt_password);
      cJSON_AddStringToObject(mqtt, "pub_topic", nvs_mqtt_pub_topic);
      cJSON_AddStringToObject(mqtt, "sub_topic", nvs_mqtt_sub_topic);

      // 将mqtt子对象添加到root对象
      cJSON_AddItemToObject(root, "mqtt", mqtt);
    }
  }

  // 释放分配的内存
  free(nvs_device_type);
  free(nvs_mqtt_clientid);
  free(nvs_mqtt_server);
  free(nvs_mqtt_port);
  free(nvs_mqtt_username);
  free(nvs_mqtt_password);
  free(nvs_mqtt_pub_topic);
  free(nvs_mqtt_sub_topic);
  free(nvs_host_names);
  free(nvs_device_sn);
  nvs_close(nvs_handle);

  // 序列化JSON并返回
  char *jsonString = cJSON_Print(root);
  cJSON_Delete(root);

  if (jsonString == NULL) {
    printf("Failed to serialize JSON\n");
    return NULL;
  }

  // 打印组装好的JSON
  printf("组装好的MQTT配置JSON: %s\n", jsonString);

  char *result = strdup(jsonString);
  if (result == NULL) {
    printf("Failed to duplicate JSON string\n");
    free(jsonString);
    return NULL;
  }

  free(jsonString);
  return result;
}

char *heartbeat_config_json(void) {
  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    printf("Failed to create JSON root object\n");
    return NULL;
  }

  uint8_t sta_mac[6] = {0};
  uint8_t eth_mac[6] = {0};

  esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);
  esp_read_mac(eth_mac, ESP_MAC_ETH);

  char d_sta_mac[24];
  char d_eth_mac[24];
  sprintf(d_sta_mac, "%02X:%02X:%02X:%02X:%02X:%02X", sta_mac[0], sta_mac[1],
          sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
  sprintf(d_eth_mac, "%02X:%02X:%02X:%02X:%02X:%02X", eth_mac[0], eth_mac[1],
          eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);

  cJSON_AddStringToObject(root, "mac_ap", d_sta_mac);
  cJSON_AddStringToObject(root, "mac_eth", d_eth_mac);
  char *jsonString = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (jsonString == NULL) {
    printf("Failed to serialize JSON\n");
    return NULL;
  }

  return jsonString;
}

char *send_device_ip(void) {
  char *jsonString = NULL;
  char *nvs_host_names = NULL;
  char *nvs_device_type = NULL;
  char *nvs_device_sn = NULL;
  char *nvs_is_dhcp = NULL;
  char *nvs_dns1 = NULL;
  char *nvs_dns2 = NULL;
  char *nvs_thsensor = NULL;
  char *nvs_psensor = NULL;
  char *nvs_adapter_id = NULL;
  char *nvs_interface_mode = NULL;
  char *nvs_power_mode = NULL;

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_safe_reset());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));

  // 初始化大小变量
  size_t host_names = 0, device_type = 0, device_sn = 0, is_dhcp = 0, dns1 = 0,
         dns2 = 0, thsensor = 0, psensor = 0, adapter_id = 0, interface_mode = 0,
         power_mode = 0;

  // 获取设备类型
  ret = nvs_get_str(nvs_handle, "device_type", NULL, &device_type);
  switch (ret) {
  case ESP_OK:
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "device_type", "SP501"));
    device_type = 8; // "SP501" length + null terminator
    break;
  default:
    break;
  }

  // 获取主机名和设备SN大小
  ret = nvs_get_str(nvs_handle, "host_names", NULL, &host_names);
  ret = nvs_get_str(nvs_handle, "device_sn", NULL, &device_sn);

  // 获取DHCP状态和DNS设置大小
  ret = nvs_get_str(nvs_handle, "is_dhcp", NULL, &is_dhcp);
  ret = nvs_get_str(nvs_handle, "dns1", NULL, &dns1);
  ret = nvs_get_str(nvs_handle, "dns2", NULL, &dns2);
  ret = nvs_get_str(nvs_handle, "thsensor", NULL, &thsensor);
  ret = nvs_get_str(nvs_handle, "psensor", NULL, &psensor);
  ret = nvs_get_str(nvs_handle, "adapter_id", NULL, &adapter_id);
  ret = nvs_get_str(nvs_handle, "interface", NULL, &interface_mode);
  ret = nvs_get_str(nvs_handle, "power", NULL, &power_mode);

  if (host_names == 0) {
    host_names = 10; // 预设一个默认大小
  }

  if (device_sn == 0) {
    device_sn = 1; // 至少需要存储空字符
  }

  if (device_type == 0) {
    device_type = 8; // "SP501" length + null terminator
  }

  if (is_dhcp == 0) {
    is_dhcp = 2; // 默认值长度
  }

  if (dns1 == 0) {
    dns1 = 8; // 8.8.8.8 长度
  }

  if (dns2 == 0) {
    dns2 = 16; // 114.114.114.114 长度 + 安全裕度
  }
  if (thsensor == 0) {
    thsensor = 16;
  }
  if (psensor == 0) {
    psensor = 16;
  }
  if (adapter_id == 0) {
    adapter_id = 16;
  }
  if (interface_mode == 0) {
    interface_mode = 8;
  }
  if (power_mode == 0) {
    power_mode = 8;
  }

  // 分配内存
  nvs_host_names = malloc(host_names);
  nvs_device_type = malloc(device_type);
  nvs_device_sn = malloc(device_sn);
  nvs_is_dhcp = malloc(is_dhcp);
  nvs_dns1 = malloc(dns1);
  nvs_dns2 = malloc(dns2);
  nvs_thsensor = malloc(thsensor);
  nvs_psensor = malloc(psensor);
  nvs_adapter_id = malloc(adapter_id);
  nvs_interface_mode = malloc(interface_mode);
  nvs_power_mode = malloc(power_mode);

  // 检查所有分配是否成功
  if (nvs_host_names == NULL || nvs_device_type == NULL ||
      nvs_device_sn == NULL || nvs_is_dhcp == NULL ||
      nvs_dns1 == NULL || nvs_dns2 == NULL || nvs_thsensor == NULL ||
      nvs_psensor == NULL || nvs_adapter_id == NULL ||
      nvs_interface_mode == NULL || nvs_power_mode == NULL) {
    printf("Memory allocation failed\n");
    // 释放所有已分配的内存
    if (nvs_host_names)
      free(nvs_host_names);
    if (nvs_device_type)
      free(nvs_device_type);
    if (nvs_device_sn)
      free(nvs_device_sn);
    if (nvs_is_dhcp)
      free(nvs_is_dhcp);
    if (nvs_dns1)
      free(nvs_dns1);
    if (nvs_dns2)
      free(nvs_dns2);
    if (nvs_thsensor)
      free(nvs_thsensor);
    if (nvs_psensor)
      free(nvs_psensor);
    if (nvs_adapter_id)
      free(nvs_adapter_id);
    if (nvs_interface_mode)
      free(nvs_interface_mode);
    if (nvs_power_mode)
      free(nvs_power_mode);
    nvs_close(nvs_handle);
    return NULL;
  }

  // 设置默认值，确保即使NVS读取失败也有有效值
  strcpy(nvs_host_names, "Unknown");
  strcpy(nvs_device_type, "SP501");
  nvs_device_sn[0] = '\0';
  strcpy(nvs_is_dhcp, "2");        // 默认静态IP
  strcpy(nvs_dns1, "8.8.8.8");     // 默认DNS1
  strcpy(nvs_dns2, "114.114.114.114"); // 默认DNS2
  strcpy(nvs_thsensor, "Unknown");
  strcpy(nvs_psensor, "None");
  strcpy(nvs_adapter_id, "series");
  strcpy(nvs_interface_mode, "0");
  strcpy(nvs_power_mode, "0");

  // 获取主机名
  ret = nvs_get_str(nvs_handle, "host_names", nvs_host_names, &host_names);

  // 获取设备类型
  ret = nvs_get_str(nvs_handle, "device_type", nvs_device_type, &device_type);

  // 获取设备SN
  ret = nvs_get_str(nvs_handle, "device_sn", nvs_device_sn, &device_sn);

  // 获取DHCP状态
  ret = nvs_get_str(nvs_handle, "is_dhcp", nvs_is_dhcp, &is_dhcp);

  // 获取DNS1
  ret = nvs_get_str(nvs_handle, "dns1", nvs_dns1, &dns1);

  // 获取DNS2
  ret = nvs_get_str(nvs_handle, "dns2", nvs_dns2, &dns2);

  // 获取传感器及适配器信息
  ret = nvs_get_str(nvs_handle, "thsensor", nvs_thsensor, &thsensor);
  ret = nvs_get_str(nvs_handle, "psensor", nvs_psensor, &psensor);
  ret = nvs_get_str(nvs_handle, "adapter_id", nvs_adapter_id, &adapter_id);
  ret = nvs_get_str(nvs_handle, "interface", nvs_interface_mode, &interface_mode);
  ret = nvs_get_str(nvs_handle, "power", nvs_power_mode, &power_mode);

  // 创建JSON对象
  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    printf("Failed to create JSON root object\n");
    free(nvs_host_names);
    free(nvs_device_type);
    free(nvs_device_sn);
    free(nvs_is_dhcp);
    free(nvs_dns1);
    free(nvs_dns2);
    free(nvs_thsensor);
    free(nvs_psensor);
    free(nvs_adapter_id);
    free(nvs_interface_mode);
    free(nvs_power_mode);
    nvs_close(nvs_handle);
    return NULL;
  }

  // 添加值到JSON
  cJSON_AddStringToObject(root, "type", nvs_device_type);
  cJSON_AddStringToObject(root, "name", nvs_host_names);

  // 检查字符串长度，不检查地址
  const char *eth_mac_value = (device_eth_mac[0] != '\0') ? device_eth_mac : "00:00:00:00:00:00";
  const char *sta_mac_value = (device_sta_mac[0] != '\0') ? device_sta_mac : "00:00:00:00:00:00";

  cJSON_AddStringToObject(root, "mac", eth_mac_value);

  // 检查字符串是否为空
  const char *ip_value = (got_ip_addrs[0] != '\0') ? got_ip_addrs : "0.0.0.0";
  const char *netmask_value = (got_ip_netmask[0] != '\0') ? got_ip_netmask : "255.255.255.0";
  const char *gateway_value = (got_ip_gw[0] != '\0') ? got_ip_gw : "0.0.0.0";

  cJSON_AddStringToObject(root, "ip", ip_value);
  cJSON_AddStringToObject(root, "netmask", netmask_value);
  cJSON_AddStringToObject(root, "gateway", gateway_value);
  cJSON_AddStringToObject(root, "eth_mac", eth_mac_value);
  cJSON_AddStringToObject(root, "sta_mac", sta_mac_value);
  cJSON_AddStringToObject(root, "thsensor", nvs_thsensor);
  cJSON_AddStringToObject(root, "psensor", nvs_psensor);
  cJSON_AddStringToObject(root, "adapter_id", nvs_adapter_id);
  cJSON_AddStringToObject(root, "interface", nvs_interface_mode);
  cJSON_AddStringToObject(root, "power", nvs_power_mode);

  // 检查版本号是否为空
  const char *version_value = (VERSION[0] != '\0') ? VERSION : "1.0.0";
  cJSON_AddStringToObject(root, "version", version_value);


  cJSON_AddStringToObject(root, "device_sn", nvs_device_sn);
  cJSON_AddStringToObject(root, "is_dhcp", nvs_is_dhcp);
  cJSON_AddStringToObject(root, "dns1", nvs_dns1);
  cJSON_AddStringToObject(root, "dns2", nvs_dns2);

  // 生成JSON字符串
  jsonString = cJSON_Print(root);

  // 释放资源
  cJSON_Delete(root);
  free(nvs_host_names);
  free(nvs_device_type);
  free(nvs_device_sn);
  free(nvs_is_dhcp);
  free(nvs_dns1);
  free(nvs_dns2);
  free(nvs_thsensor);
  free(nvs_psensor);
  free(nvs_adapter_id);
  free(nvs_interface_mode);
  free(nvs_power_mode);
  nvs_close(nvs_handle);

  return jsonString;
}

char *send_device_info_err(void) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "messageid", messageid);
  cJSON_AddNumberToObject(root, "code", 204);
  cJSON_AddStringToObject(root, "msg", "type not found");
  char *jsonString = cJSON_Print(root);
  cJSON_Delete(root);
  return jsonString;
}

// 函数用于重置并清除所有NVS存储
esp_err_t nvs_reset_clear() {
  const char *TAG = "nvs_reset_clear";
  esp_err_t err;
  err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGI(TAG, "Erasing NVS flash");
    err = nvs_flash_erase();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "NVS flash erase failed: %s", esp_err_to_name(err));
      return err;
    }
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "NVS flash reset and cleared");
  return ESP_OK;
}

// 处理多寄存器数据的转换函数
char *convert_multi_register_data(uint16_t *registers, int reg_count,
                                  const char *format) {
  static char result[32];
  union {
    uint32_t long_val;
    float float_val;
    double double_val;
    uint16_t uint16_array[4]; // 最多支持4个寄存器(64位)
  } data_converter;
  memset(&data_converter, 0, sizeof(data_converter));

  bool is_inverse = (strstr(format, "Inverse") != NULL);

  if (strstr(format, "Long") != NULL) {
    // Long类型需要2个寄存器
    if (reg_count >= 2) {
      if (is_inverse) {
        data_converter.uint16_array[0] = registers[1];
        data_converter.uint16_array[1] = registers[0];
      } else {
        data_converter.uint16_array[0] = registers[0];
        data_converter.uint16_array[1] = registers[1];
      }
      snprintf(result, sizeof(result), "%ld", (int32_t)data_converter.long_val);
    }
  } else if (strstr(format, "Float") != NULL) {
    // Float类型需要2个寄存器
    if (reg_count >= 2) {
      if (is_inverse) {
        data_converter.uint16_array[0] = registers[1];
        data_converter.uint16_array[1] = registers[0];
      } else {
        data_converter.uint16_array[0] = registers[0];
        data_converter.uint16_array[1] = registers[1];
      }
      snprintf(result, sizeof(result), "%.6f", data_converter.float_val);
    }
  } else if (strstr(format, "Double") != NULL) {
    // Double类型需要4个寄存器
    if (reg_count >= 4) {
      if (is_inverse) {
        for (int i = 0; i < 4; i++) {
          data_converter.uint16_array[i] = registers[3 - i];
        }
      } else {
        for (int i = 0; i < 4; i++) {
          data_converter.uint16_array[i] = registers[i];
        }
      }
      snprintf(result, sizeof(result), "%.12lf", data_converter.double_val);
    }
  } else if (strcmp(format, "Signed") == 0) {
    snprintf(result, sizeof(result), "%d", (int16_t)registers[0]);
  } else if (strcmp(format, "Unsigned") == 0) {
    snprintf(result, sizeof(result), "%u", registers[0]);
  } else if (strcmp(format, "Binary") == 0) {
    char binary[17] = {0};
    for (int i = 15; i >= 0; i--) {
      binary[15 - i] = ((registers[0] >> i) & 1) ? '1' : '0';
    }
    snprintf(result, sizeof(result), "%s", binary);
  } else { // HEX 或其他格式
    snprintf(result, sizeof(result), "0x%04X", registers[0]);
  }

  return result;
}

void protocol_select() {
  sx_net_event_t event = {
      .type = SX_NET_EVT_PERIODIC_CHECK,
      .source_if = SX_NET_IF_NONE,
  };
  sx_network_manager_post_event(&event);
  ESP_LOGI("PROTOCOL_SELECT",
           "旧协议选择入口已收口，只请求网络管理器刷新状态");
}

void eth_disconnect() {

  printf("eth disconnect\n");
  //   gpio_set_level(LED, 0);
  if (xHandleTask3 != NULL) {
    vTaskDelete(xHandleTask3);
    xHandleTask3 = NULL;
  }
}

void simple_task() {

  init_timer_tasks();

  // xTaskCreate(free_task, (char *)"free_task", 2048, NULL, 1, NULL);
  // xTaskCreate(&key_task, "key_task", 2048, NULL, 0, NULL);
  // xTaskCreate(rx_task, "uart_tx_task", 1024 * 2, NULL, configMAX_PRIORITIES -
  // 2,
  //             NULL);
  // rx_wait();
}

// 验证用户名和密码
bool verify_credentials(const char *username, const char *password) {
  const char *TAG = "verify_credentials";
  nvs_handle_t nvs_handle;
  esp_err_t err;
  bool result = false;

  // 打开NVS
  err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
    return false;
  }

  // 获取存储的用户名和密码
  size_t lgname_size = 0;
  size_t lgpwd_size = 0;

  // 获取存储的用户名大小
  err = nvs_get_str(nvs_handle, "lgname", NULL, &lgname_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error getting username size: %s", esp_err_to_name(err));
    // 如果没有找到lgname，尝试使用默认值
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGI(TAG, "lgname not found, using default credentials");
      if (strcmp(username, "public") == 0 &&
          strcmp(password, "Aa123456") == 0) {
        ESP_LOGI(TAG, "Default credentials verified successfully");
        nvs_close(nvs_handle);
        return true;
      }
    }
    nvs_close(nvs_handle);
    return false;
  }

  // 获取存储的密码大小
  err = nvs_get_str(nvs_handle, "lgpwd", NULL, &lgpwd_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error getting password size: %s", esp_err_to_name(err));
    nvs_close(nvs_handle);
    return false;
  }

  // 分配内存
  char *stored_username = malloc(lgname_size);
  char *stored_password = malloc(lgpwd_size);

  if (!stored_username || !stored_password) {
    ESP_LOGE(TAG, "Memory allocation failed");
    if (stored_username)
      free(stored_username);
    if (stored_password)
      free(stored_password);
    nvs_close(nvs_handle);
    return false;
  }

  // 获取存储的用户名
  err = nvs_get_str(nvs_handle, "lgname", stored_username, &lgname_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error getting username: %s", esp_err_to_name(err));
    free(stored_username);
    free(stored_password);
    nvs_close(nvs_handle);
    return false;
  }

  // 获取存储的密码
  err = nvs_get_str(nvs_handle, "lgpwd", stored_password, &lgpwd_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error getting password: %s", esp_err_to_name(err));
    free(stored_username);
    free(stored_password);
    nvs_close(nvs_handle);
    return false;
  }

  // 验证用户名和密码
  if (strcmp(username, stored_username) == 0 &&
      strcmp(password, stored_password) == 0) {
    ESP_LOGI(TAG, "Credentials verified successfully");
    result = true;
  } else {
    ESP_LOGE(TAG, "Invalid credentials");
    result = false;
  }

  // 释放资源
  free(stored_username);
  free(stored_password);
  nvs_close(nvs_handle);

  return result;
}

// 更新设备设置
bool update_device_settings(cJSON *parm) {
    const char *TAG = "update_device_settings";
    nvs_handle_t nvs_handle;
    esp_err_t err;
    bool result = true;

    // 声明外部函数
    extern void reset_device_name_cache(void);

    // 打印完整的JSON参数，用于调试
    char *json_str = cJSON_Print(parm);
    if (json_str) {
        ESP_LOGI(TAG, "Incoming parameters: %s", json_str);

        // 为了调试，检查mask和netmask字段是否存在
        cJSON *mask_check = cJSON_GetObjectItem(parm, "mask");
        cJSON *netmask_check = cJSON_GetObjectItem(parm, "netmask");

        ESP_LOGI(TAG, "Field check - mask exists: %d, netmask exists: %d",
                 (mask_check != NULL), (netmask_check != NULL));

        if (mask_check) {
            ESP_LOGI(TAG, "mask value: %s",
                     cJSON_IsString(mask_check) ? mask_check->valuestring
                                                : "NOT_STRING");
        }

        if (netmask_check) {
            ESP_LOGI(TAG, "netmask value: %s",
                     cJSON_IsString(netmask_check) ? netmask_check->valuestring
                                                   : "NOT_STRING");
        }

        free(json_str);
    } else {
        ESP_LOGI(TAG, "Failed to print incoming parameters");
    }

    // 打开NVS
    err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        return false;
    }

    // 检查并设置网络模式（DHCP或静态IP）
    cJSON *dhcp_mode_obj = cJSON_GetObjectItem(parm, "is_dhcp");
    if (cJSON_IsString(dhcp_mode_obj) && dhcp_mode_obj->valuestring != NULL) {
        const char *dhcp_mode = dhcp_mode_obj->valuestring;
        ESP_LOGI(TAG, "Setting network mode (is_dhcp = %s) - %s", dhcp_mode,
                 strcmp(dhcp_mode, "1") == 0 ? "DHCP Mode" : "Static IP Mode");

        err = nvs_set_str(nvs_handle, "is_dhcp", dhcp_mode);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error setting is_dhcp: %s", esp_err_to_name(err));
            result = false;
        }
    } else {
        // 默认设置为静态IP模式
        ESP_LOGI(TAG, "No is_dhcp parameter found, defaulting to static IP mode "
                      "(is_dhcp = 2)");
        err = nvs_set_str(nvs_handle, "is_dhcp", "2");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error setting default is_dhcp: %s", esp_err_to_name(err));
            result = false;
        }
    }

    // 检查并更新设备名称 - 使用host_names字段
    cJSON *name = cJSON_GetObjectItem(parm, "name");
    if (cJSON_IsString(name) && name->valuestring != NULL) {
        ESP_LOGI(TAG, "Updating device name to: %s", name->valuestring);
        err = nvs_set_str(nvs_handle, "host_names", name->valuestring);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error setting host_names: %s", esp_err_to_name(err));
            result = false;
        } else {
            // 重置设备名称缓存
            reset_device_name_cache();
        }
    }

    // 检查并更新IP地址 - 使用static_ip字段
    cJSON *ip = cJSON_GetObjectItem(parm, "ip");
    if (cJSON_IsString(ip) && ip->valuestring != NULL) {
        ESP_LOGI(TAG, "Updating IP address to: %s", ip->valuestring);
        err = nvs_set_str(nvs_handle, "static_ip", ip->valuestring);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error setting static_ip: %s", esp_err_to_name(err));
            result = false;
        }
    }

    // 检查并更新子网掩码 - 使用static_netmask字段
    cJSON *mask = cJSON_GetObjectItem(parm, "mask");
    if (cJSON_IsString(mask) && mask->valuestring != NULL) {
        ESP_LOGI(TAG, "Updating subnet mask to: %s", mask->valuestring);
        err = nvs_set_str(nvs_handle, "static_netmask", mask->valuestring);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error setting static_netmask: %s", esp_err_to_name(err));
            result = false;
        }
    } else {
        // 兼容检查客户端可能使用的"netmask"字段
        cJSON *netmask = cJSON_GetObjectItem(parm, "netmask");
        if (cJSON_IsString(netmask) && netmask->valuestring != NULL) {
            ESP_LOGI(TAG, "Updating subnet mask to: %s (from netmask param)",
                     netmask->valuestring);
            err = nvs_set_str(nvs_handle, "static_netmask", netmask->valuestring);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error setting static_netmask: %s", esp_err_to_name(err));
                result = false;
            }
        }
    }

    // 检查并更新网关 - 使用static_gateway字段
    cJSON *gateway = cJSON_GetObjectItem(parm, "gateway");
    if (cJSON_IsString(gateway) && gateway->valuestring != NULL) {
        ESP_LOGI(TAG, "Updating gateway to: %s", gateway->valuestring);
        err = nvs_set_str(nvs_handle, "static_gateway", gateway->valuestring);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error setting static_gateway: %s", esp_err_to_name(err));
            result = false;
        }
    }

    // 检查并更新DNS1 - 使用static_dns1字段
    cJSON *dns1 = cJSON_GetObjectItem(parm, "static_dns1");
    if (cJSON_IsString(dns1) && dns1->valuestring != NULL) {
        ESP_LOGI(TAG, "Updating DNS1 to: %s", dns1->valuestring);
        err = nvs_set_str(nvs_handle, "static_dns1", dns1->valuestring);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error setting static_dns1: %s", esp_err_to_name(err));
            result = false;
        }
    }

    // 检查并更新DNS2 - 使用static_dns2字段
    cJSON *dns2 = cJSON_GetObjectItem(parm, "static_dns2");
    if (cJSON_IsString(dns2) && dns2->valuestring != NULL) {
        ESP_LOGI(TAG, "Updating DNS2 to: %s", dns2->valuestring);
        err = nvs_set_str(nvs_handle, "static_dns2", dns2->valuestring);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error setting static_dns2: %s", esp_err_to_name(err));
            result = false;
        }
    }

    // 打印出所有最终的网络设置
    char ip_str[32] = {0};
    char netmask_str[32] = {0};
    char gateway_str[32] = {0};
    char dns1_str[32] = {0};
    char dns2_str[32] = {0};
    char hostname[64] = {0};
    char nvs_is_dhcp[8] = {0}; // 重命名以避免冲突
    size_t len;

    len = sizeof(nvs_is_dhcp);
    nvs_get_str(nvs_handle, "is_dhcp", nvs_is_dhcp, &len);

    len = sizeof(ip_str);
    nvs_get_str(nvs_handle, "static_ip", ip_str, &len);

    len = sizeof(netmask_str);
    nvs_get_str(nvs_handle, "static_netmask", netmask_str, &len);

    len = sizeof(gateway_str);
    nvs_get_str(nvs_handle, "static_gateway", gateway_str, &len);

    len = sizeof(dns1_str);
    nvs_get_str(nvs_handle, "static_dns1", dns1_str, &len);

    len = sizeof(dns2_str);
    nvs_get_str(nvs_handle, "static_dns2", dns2_str, &len);

    len = sizeof(hostname);
    nvs_get_str(nvs_handle, "host_names", hostname, &len);

    // 更新日志信息，区分DHCP和静态IP模式
    if (strcmp(nvs_is_dhcp, "1") == 0) {
        ESP_LOGI(TAG,
                 "Final network settings - DHCP Mode: Enabled (is_dhcp=%s), "
                 "Hostname: %s",
                 nvs_is_dhcp, hostname);
    } else {
        ESP_LOGI(TAG,
                 "Final network settings - Static IP Mode (is_dhcp=%s), Hostname: "
                 "%s, IP: %s, Netmask: %s, Gateway: %s, DNS1: %s, DNS2: %s",
                 nvs_is_dhcp, hostname, ip_str, netmask_str, gateway_str, dns1_str,
                 dns2_str);
    }

    // 提交更改
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes: %s", esp_err_to_name(err));
        result = false;
    }

    // 关闭NVS
    nvs_close(nvs_handle);

    return result;
}

// DNS解析检查函数
bool check_dns_resolution(const char* hostname) {
    const char *TAG = "DNS_CHECK";
    struct addrinfo hints;
    struct addrinfo *res;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    ESP_LOGI(TAG, "正在解析域名: %s", hostname);

    int err = getaddrinfo(hostname, "80", &hints, &res);
    if (err != 0) {
        ESP_LOGE(TAG, "DNS解析失败 %s (错误码: %d)", hostname, err);
        return false;
    }

    // 获取解析到的IP地址
    struct sockaddr_in *addr_in = (struct sockaddr_in *)res->ai_addr;
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(addr_in->sin_addr), ip_str, INET_ADDRSTRLEN);

    ESP_LOGI(TAG, "DNS解析成功 %s -> %s", hostname, ip_str);

    freeaddrinfo(res);
    return true;
}

// 网络诊断函数
void network_diagnostics(void) {
    const char *TAG = "NETWORK_DIAG";

    ESP_LOGI(TAG, "开始网络诊断...");

    // 显示当前网络配置
    ESP_LOGI(TAG, "当前网络配置:");
    ESP_LOGI(TAG, "IP地址: %s", got_ip_addrs);
    ESP_LOGI(TAG, "子网掩码: %s", got_ip_netmask);
    ESP_LOGI(TAG, "网关: %s", got_ip_gw);

    // 获取DNS配置
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("nvs_namespace", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        char dns1[32] = {0};
        char dns2[32] = {0};
        size_t len = sizeof(dns1);

        nvs_get_str(nvs_handle, "static_dns1", dns1, &len);
        len = sizeof(dns2);
        nvs_get_str(nvs_handle, "static_dns2", dns2, &len);

        ESP_LOGI(TAG, "主DNS: %s", strlen(dns1) > 0 ? dns1 : "8.8.8.8");
        ESP_LOGI(TAG, "备用DNS: %s", strlen(dns2) > 0 ? dns2 : "114.114.114.114");

        nvs_close(nvs_handle);
    }

    sx_network_status_t status;
    sx_network_manager_get_status(&status);
    bool network_ok = status.net_state != SX_NET_STATE_DOWN;

    // 检查网络连接状态
    ESP_LOGI(TAG, "网络连接状态: %s", network_ok ? "已连接" : "未连接");
    ESP_LOGI(TAG, "网络模式: %s", sx_network_state_name(status.net_state));

    // 如果网络已连接，根据模式进行相应测试
    if (network_ok) {
        ESP_LOGI(TAG, "测试网络连接...");

        if (is_intranet_mode) {
            // 内网模式：测试本地网络连接
            ESP_LOGI(TAG, "内网模式 - 测试本地网络连接");

            // 测试网关连接
            if (strlen(got_ip_gw) > 0 && strcmp(got_ip_gw, "0.0.0.0") != 0) {
                ESP_LOGI(TAG, "网关地址: %s (可达)", got_ip_gw);
            }

            // 测试本地DNS解析（如果有本地DNS服务器）
            const char* local_test_domains[] = {
                "localhost",
                got_ip_gw  // 尝试解析网关地址
            };

            for (int i = 0; i < sizeof(local_test_domains)/sizeof(local_test_domains[0]); i++) {
                if (local_test_domains[i] && strlen(local_test_domains[i]) > 0) {
                    check_dns_resolution(local_test_domains[i]);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
        } else {
            // 外网模式：测试外网域名
            ESP_LOGI(TAG, "外网模式 - 测试DNS解析");

            const char* test_domains[] = {
                "www.baidu.com"
            };

            for (int i = 0; i < sizeof(test_domains)/sizeof(test_domains[0]); i++) {
                check_dns_resolution(test_domains[i]);
                vTaskDelay(pdMS_TO_TICKS(100)); // 短暂延迟避免过快
            }
        }
    } else {
        ESP_LOGW(TAG, "网络未连接，跳过连接测试");
    }

    ESP_LOGI(TAG, "网络诊断完成");
}

// 检查本地网络连接（内网）
bool check_local_network_connectivity(void) {
    sx_network_status_t status;
    sx_network_manager_get_status(&status);
    return status.net_state == SX_NET_STATE_LOCAL ||
           status.net_state == SX_NET_STATE_EXTERNAL;
}

// 检查外网连接
bool check_internet_connectivity(void) {
    sx_network_status_t status;
    sx_network_manager_get_status(&status);
    return status.net_state == SX_NET_STATE_EXTERNAL;
}

// 智能网络连接检查 - 修改逻辑：先ping网关，再检查平台心跳
bool check_network_connectivity(void) {
    sx_network_status_t status;
    sx_network_manager_get_status(&status);
    return status.net_state != SX_NET_STATE_DOWN;
}

// 添加ping网关的函数 - 简化版本用于网关可达性检查
bool ping_gateway(const char* gateway_ip) {
    const char *TAG = "PING_GATEWAY";

    if (!gateway_ip || strlen(gateway_ip) == 0 || strcmp(gateway_ip, "0.0.0.0") == 0 || strcmp(gateway_ip, "---") == 0) {
        ESP_LOGD(TAG, "网关地址无效: %s", gateway_ip ? gateway_ip : "NULL");
        return false;
    }

    ESP_LOGD(TAG, "检查网关可达性: %s", gateway_ip);

    // 验证IP地址格式
    struct in_addr addr;
    if (inet_aton(gateway_ip, &addr) == 0) {
        ESP_LOGW(TAG, "网关IP地址格式无效: %s", gateway_ip);
        return false;
    }

    // 检查是否为有效的网络地址
    uint32_t ip = ntohl(addr.s_addr);
    if (ip == 0 || ip == 0xFFFFFFFF) {
        ESP_LOGW(TAG, "网关IP地址无效: %s", gateway_ip);
        return false;
    }

    // 简化的连通性检查：创建socket尝试连接
    int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGD(TAG, "无法创建socket");
        return false;
    }

    // 设置连接超时为2秒
    int timeout_ms = 2000;  // 2秒超时
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
    lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout_ms, sizeof(timeout_ms));

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = addr.s_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(80);  // 尝试连接80端口

    int result = lwip_connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

    bool is_reachable = false;
    if (result == 0) {
        // 连接成功
        is_reachable = true;
        ESP_LOGI(TAG, "网关连接测试成功: %s", gateway_ip);
    } else {
        ESP_LOGD(TAG, "网关端口80连接失败: %s", gateway_ip);

        // 如果端口80连接失败，检查是否在同一网段（简化检查）
        extern char got_ip_addrs[16];
        extern char got_ip_netmask[16];

        if (strlen(got_ip_addrs) > 0 && strlen(got_ip_netmask) > 0 &&
            strcmp(got_ip_addrs, "0.0.0.0") != 0 && strcmp(got_ip_addrs, "---") != 0 &&
            strcmp(got_ip_netmask, "0.0.0.0") != 0 && strcmp(got_ip_netmask, "---") != 0) {

            struct in_addr local_addr, netmask_addr;
            if (inet_aton(got_ip_addrs, &local_addr) && inet_aton(got_ip_netmask, &netmask_addr)) {
                uint32_t local_ip = ntohl(local_addr.s_addr);
                uint32_t gateway_ip_num = ntohl(addr.s_addr);
                uint32_t netmask = ntohl(netmask_addr.s_addr);

                // 检查是否在同一网段
                if ((local_ip & netmask) == (gateway_ip_num & netmask)) {
                    is_reachable = true;
                    ESP_LOGI(TAG, "网关在同一网段，认为可达: %s", gateway_ip);
                }
            }
        }
    }

    lwip_close(sock);
    return is_reachable;
}

// 强制更新网络状态 - 用于确保心跳正常工作
void force_network_status_update(void) {
    sx_net_event_t event = {
        .type = SX_NET_EVT_PERIODIC_CHECK,
        .source_if = SX_NET_IF_NONE,
    };
    sx_network_manager_post_event(&event);
    ESP_LOGI("NETWORK_STATUS_UPDATE",
             "旧强制刷新入口已收口，只请求网络管理器刷新状态");
}



/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
