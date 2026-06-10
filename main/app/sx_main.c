/* Firmware application entry. */

#include "driver/i2c.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "ethernet_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sx_async_uart.h"
#include "sx_gpio.h"
#include "sx_http_client.h"
#include "sx_init_eth.h"
#include "sx_init_wifi.h"
#include "sx_ap_manager.h"
#include "sx_network_manager.h"
#include "sx_protocol_manager.h"
#include "sx_timer_tasks.h"
#include "sx_utils.h"
#include "sx_work_mode_ids.h"
#include "sx_web_server.h"
#include "sx_work_mode.h"
#include <inttypes.h> // 添加inttypes.h头文件用于PRIu32宏
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MAIN";

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data) {
  if (event_id == IP_EVENT_ETH_LOST_IP) {
    ESP_LOGI(TAG, "Ethernet IP_EVENT_ETH_LOST_IP");
    sx_network_manager_notify_eth_link_down();
    return;
  }

  if (event_id == IP_EVENT_ETH_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_infos = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_infos->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_infos->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_infos->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    sx_network_manager_notify_eth_got_ip(ip_infos);
  }
}

bool check_nvs_key(nvs_handle_t nvs_handle, const char *key) {
  size_t required_size;
  return nvs_get_str(nvs_handle, key, NULL, &required_size) ==
         ESP_ERR_NVS_NOT_FOUND;
}

static void migrate_storage_namespace(void) {
  nvs_handle_t storage_handle;
  if (nvs_open("storage", NVS_READONLY, &storage_handle) != ESP_OK) {
    return;
  }

  nvs_handle_t nvs_handle;
  if (nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle) != ESP_OK) {
    nvs_close(storage_handle);
    return;
  }

  bool updated = false;

  const char *string_keys[] = {WORK_MODE_NVS_KEY, "poll_time"};
  for (size_t i = 0; i < sizeof(string_keys) / sizeof(string_keys[0]); i++) {
    size_t dst_size = 0;
    if (nvs_get_str(nvs_handle, string_keys[i], NULL, &dst_size) ==
        ESP_ERR_NVS_NOT_FOUND) {
      size_t src_size = 0;
      if (nvs_get_str(storage_handle, string_keys[i], NULL, &src_size) ==
              ESP_OK &&
          src_size > 0) {
        char *buf = malloc(src_size);
        if (buf &&
            nvs_get_str(storage_handle, string_keys[i], buf, &src_size) ==
                ESP_OK) {
          if (nvs_set_str(nvs_handle, string_keys[i], buf) == ESP_OK) {
            updated = true;
          }
        }
        if (buf) {
          free(buf);
        }
      }
    }
  }

  int32_t count_dst = 0;
  if (nvs_get_i32(nvs_handle, "m_count", &count_dst) != ESP_OK) {
    int32_t count_src = 0;
    if (nvs_get_i32(storage_handle, "m_count", &count_src) == ESP_OK &&
        count_src > 0) {
      if (nvs_set_i32(nvs_handle, "m_count", count_src) == ESP_OK) {
        updated = true;
      }
      const char *suffixes[] = {"s_addr",    "f_code",   "r_addr",  "r_num",
                                "timeout",   "d_fmt",    "i_time",  "r_fmt",
                                "baud_rate", "data_bit", "stop_bit", "check_bit"};
      for (int i = 0; i < count_src; i++) {
        char key[32];
        uint8_t enabled = 0;
        snprintf(key, sizeof(key), "m%d_en", i);
        if (nvs_get_u8(storage_handle, key, &enabled) == ESP_OK) {
          if (nvs_set_u8(nvs_handle, key, enabled) == ESP_OK) {
            updated = true;
          }
        }

        for (size_t j = 0; j < sizeof(suffixes) / sizeof(suffixes[0]); j++) {
          size_t src_size = 0;
          snprintf(key, sizeof(key), "m%d%s", i, suffixes[j]);
          if (nvs_get_str(storage_handle, key, NULL, &src_size) == ESP_OK &&
              src_size > 0) {
            char *buf = malloc(src_size);
            if (buf && nvs_get_str(storage_handle, key, buf, &src_size) ==
                           ESP_OK) {
              if (nvs_set_str(nvs_handle, key, buf) == ESP_OK) {
                updated = true;
              }
            }
            if (buf) {
              free(buf);
            }
          }
        }
      }
    }
  }

  uint8_t log_state = 0;
  if (nvs_get_u8(nvs_handle, "log_enabled", &log_state) != ESP_OK) {
    if (nvs_get_u8(storage_handle, "log_enabled", &log_state) == ESP_OK) {
      if (nvs_set_u8(nvs_handle, "log_enabled", log_state) == ESP_OK) {
        updated = true;
      }
    }
  }

  if (updated) {
    nvs_commit(nvs_handle);
  }

  nvs_close(nvs_handle);
  nvs_close(storage_handle);
}

void print_sockets(void) {
  int socket_count = 0;
  struct tcp_pcb *pcb;
  pcb = tcp_active_pcbs;

  while (pcb != NULL) {
    // ESP_LOGI(TAG, "Active socket found: local port %d, remote port %d",
    //          pcb->local_port, pcb->remote_port);
    socket_count++;
    pcb = pcb->next;
  }

  ESP_LOGI(TAG, "Total active sockets: %d", socket_count);
}

// 内存监控定时器回调函数
static void memory_monitor_timer_callback(void *arg) {
  size_t free_heap = esp_get_free_heap_size();
  size_t min_free = esp_get_minimum_free_heap_size();

  // 打印堆内存信息
  ESP_LOGI(TAG, "Free heap: %zu bytes, Minimum: %zu bytes", free_heap, min_free);

  // 🔴 紧急：内存严重不足（<10KB）
  if (free_heap < 10000) {
    ESP_LOGE(TAG, "🔴 CRITICAL: 内存严重不足！剩余: %zu bytes", free_heap);

    // 紧急清理UART队列
    extern QueueHandle_t uartDataQueue;
    if (uartDataQueue) {
      UBaseType_t queue_count = uxQueueMessagesWaiting(uartDataQueue);
      if (queue_count > 0) {
        ESP_LOGW(TAG, "紧急清理UART队列: %d条消息", queue_count);

        typedef struct {
          uint8_t *data;
          size_t len;
        } uart_queue_msg_t;

        uart_queue_msg_t msg;
        int cleared = 0;
        while (xQueueReceive(uartDataQueue, &msg, 0) == pdTRUE && cleared < queue_count - 2) {
          if (msg.data) free(msg.data);
          cleared++;
        }
        ESP_LOGW(TAG, "已清理 %d 条UART消息，释放内存", cleared);
      }
    }

    // 如果还是不够，10秒后重启
    if (esp_get_free_heap_size() < 5000) {
      ESP_LOGE(TAG, "🔴 内存耗尽！10秒后自动重启保护系统");
      vTaskDelay(pdMS_TO_TICKS(10000));
      esp_restart();
    }
  }
  // 🟠 警告：内存偏低（<20KB）
  else if (free_heap < 20000) {
    ESP_LOGW(TAG, "⚠️ 内存偏低：%zu bytes，执行清理", free_heap);

    // 清理部分UART队列
    extern QueueHandle_t uartDataQueue;
    if (uartDataQueue) {
      UBaseType_t queue_count = uxQueueMessagesWaiting(uartDataQueue);
      if (queue_count > 10) {
        ESP_LOGW(TAG, "UART队列积压: %d条，清理部分旧消息", queue_count);

        typedef struct {
          uint8_t *data;
          size_t len;
        } uart_queue_msg_t;

        uart_queue_msg_t msg;
        int cleared = 0;
        while (xQueueReceive(uartDataQueue, &msg, 0) == pdTRUE && cleared < 5) {
          if (msg.data) free(msg.data);
          cleared++;
        }
        ESP_LOGI(TAG, "清理了 %d 条旧UART消息", cleared);
      }
    }
  }

  // 打印TCP/IP连接状态
  print_sockets();
}

void app_main(void) {
  init_gpio();
  LED_DAT_ON();
  LED_WIFI_ON();
  LED_LAN_ON();
  vTaskDelay(pdMS_TO_TICKS(500));
  LED_DAT_OFF();
  LED_WIFI_OFF();
  LED_LAN_OFF();

  // 首先初始化NVS，确保后续需要访问NVS的功能正常工作
  ESP_LOGI(TAG, "初始化NVS存储...");
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_safe_reset());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  migrate_storage_namespace();
  ESP_ERROR_CHECK(sx_work_mode_register_builtin_modes());

  // 立即启动按键定时器，确保按键功能不受网络影响
  ESP_LOGI(TAG, "初始化按键定时器...");
  esp_err_t key_timer_err = init_key_timer();
  if (key_timer_err != ESP_OK) {
    ESP_LOGE(TAG, "按键定时器初始化失败: %s", esp_err_to_name(key_timer_err));
  }

  // 初始化LED闪烁定时器（使用独立定时器任务，防止被心跳检查阻塞）
  ESP_LOGI(TAG, "初始化LED闪烁定时器...");
  esp_err_t led_timer_err = init_led_timer();
  if (led_timer_err != ESP_OK) {
    ESP_LOGE(TAG, "LED闪烁定时器初始化失败: %s",
             esp_err_to_name(led_timer_err));
  }

  // 初始化内存监控定时器（不依赖网络连接）
  extern esp_timer_handle_t free_timer;
  extern void free_timer_callback(void *arg);
  if (free_timer == NULL) {
    esp_timer_create_args_t free_timer_args = {.callback = &free_timer_callback,
                                               .name = "free_timer"};
    esp_err_t err = esp_timer_create(&free_timer_args, &free_timer);
    if (err == ESP_OK) {
      err = esp_timer_start_periodic(free_timer, 5000 * 1000); // 5000ms
      if (err == ESP_OK) {
        ESP_LOGI(TAG, "内存监控定时器已在上电时启动");
      } else {
        ESP_LOGE(TAG, "无法启动内存监控定时器: %s", esp_err_to_name(err));
        esp_timer_delete(free_timer);
        free_timer = NULL;
      }
    } else {
      ESP_LOGE(TAG, "无法创建内存监控定时器: %s", esp_err_to_name(err));
    }
  }

  // ESP_ERROR_CHECK(i2cdev_init());
  // read_sx_bmp280();
  // xTaskCreatePinnedToCore(bmp280_test, "bmp280_test",
  // configMINIMAL_STACK_SIZE * 8, NULL, 5, NULL, APP_CPU_NUM);

  vTaskDelay(pdMS_TO_TICKS(200));

  // 打印启动信息和固件版本
  ESP_LOGI(TAG, "======================================");
  ESP_LOGI(TAG, "SHUOXIN-IOT Serial Server v%s", VERSION);
  ESP_LOGI(TAG, "Build time: %s %s", __DATE__, __TIME__);
  ESP_LOGI(TAG, "======================================");

  if (gpio_get_level(KEY) == 0) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    if ((gpio_get_level(KEY) == 0) && (KeyState == 0)) {
      KeyState = 1;

      for (int i = 0; i < 200; i++) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
        if (gpio_get_level(KEY) == 1) {
          break;
        }
      }
      if (gpio_get_level(KEY) == 0) {
        printf("RESET ALL!\n");
        // 重置配置
        ESP_ERROR_CHECK(nvs_flash_erase());
        vTaskDelay(100 / portTICK_PERIOD_MS);

        // 所有灯全亮一秒
        LED_DAT_ON();
        LED_WIFI_ON();
        LED_LAN_ON();
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        // 所有灯全灭
        LED_DAT_OFF();
        LED_WIFI_OFF();
        LED_LAN_OFF();
        vTaskDelay(100 / portTICK_PERIOD_MS);

        // 重启设备
        esp_restart();
      }
    }
  } else {
    KeyState = 0;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    uint8_t sta_mac[6] = {0};
    uint8_t eth_mac[6] = {0};
    char d_sta_mac[24];
    char p_mac[48];
    char s_mac[48];
    char c_mac[48];
    esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);
    esp_read_mac(eth_mac, ESP_MAC_ETH);

    sprintf(d_sta_mac, "%02x%02x%02x%02x%02x%02x", sta_mac[0], sta_mac[1],
            sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
    sprintf(p_mac, "/public/%02x%02x%02x%02x%02x%02x/publish", sta_mac[0],
            sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
    sprintf(s_mac, "/public/%02x%02x%02x%02x%02x%02x/subscribe", sta_mac[0],
            sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
    sprintf(c_mac, "%s_%02x%02x%02x%02x%02x%02x", CLIENT_HEAD, sta_mac[0],
            sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 格式化MAC地址
    sprintf(device_eth_mac, "%02X:%02X:%02X:%02X:%02X:%02X", eth_mac[0],
            eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);
    sprintf(device_sta_mac, "%02X:%02X:%02X:%02X:%02X:%02X", sta_mac[0],
            sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);

    ESP_LOGI(TAG, "Device ETH MAC: %s", device_eth_mac);
    ESP_LOGI(TAG, "Device STA MAC: %s", device_sta_mac);

    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));
    // 从NVS加载时间
    init_load_time_from_nvs();
    // 初始化所有size_t变量为0，避免使用栈上的垃圾值
    size_t netconn = 0, is_dhcp = 0, static_ip = 0, static_netmask = 0,
           static_gateway = 0, static_dns1 = 0, static_dns2 = 0, wifi_ssid = 0,
           wifi_password = 0, ap_name = 0, ap_password = 0, ap_wait_time = 0,
           use_mqtt = 0, mqtt_type = 0, mqtt_server = 0, mqtt_port = 0,
           mqtt_username = 0, mqtt_password = 0, mqtt_clientid = 0,
           mqtt_sub_topic = 0, mqtt_pub_topic = 0, qos = 0, retain = 0,
           mqtt_send = 0, mqtt_time = 0, use_http = 0, http_port = 0,
           httpconn = 0, http_url = 0, http_header = 0, http_time = 0,
           host_names = 0, ntp_server = 0, lgname = 0, lgpwd = 0, use_tcp = 0,
           tcpconn = 0;
    size_t tcp_server = 0, tcp_port = 0, tcp_send = 0, tcp_time = 0,
           device_sn = 0;

    bool nvs_not_found = false;
    if (nvs_get_str(nvs_handle, "netconn", NULL, &netconn) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "is_dhcp", NULL, &is_dhcp) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "static_ip", NULL, &static_ip) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "static_netmask", NULL, &static_netmask) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "static_gateway", NULL, &static_gateway) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "static_dns1", NULL, &static_dns1) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "static_dns2", NULL, &static_dns2) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "wifi_ssid", NULL, &wifi_ssid) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "wifi_password", NULL, &wifi_password) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "ap_name", NULL, &ap_name) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "ap_password", NULL, &ap_password) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "ap_wait_time", NULL, &ap_wait_time) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "use_mqtt", NULL, &use_mqtt) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "mqtt_type", NULL, &mqtt_type) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "mqtt_server", NULL, &mqtt_server) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "mqtt_port", NULL, &mqtt_port) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "mqtt_username", NULL, &mqtt_username) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "mqtt_password", NULL, &mqtt_password) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "mqtt_clientid", NULL, &mqtt_clientid) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "mqtt_sub_topic", NULL, &mqtt_sub_topic) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "mqtt_pub_topic", NULL, &mqtt_pub_topic) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "qos", NULL, &qos) == ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "retain", NULL, &retain) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "mqtt_send", NULL, &mqtt_send) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "mqtt_time", NULL, &mqtt_time) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "use_http", NULL, &use_http) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "http_port", NULL, &http_port) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "httpconn", NULL, &httpconn) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "http_url", NULL, &http_url) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "http_header", NULL, &http_header) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "http_time", NULL, &http_time) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "host_names", NULL, &host_names) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "ntp_server", NULL, &ntp_server) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "lgname", NULL, &lgname) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "lgpwd", NULL, &lgpwd) == ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;

    if (nvs_get_str(nvs_handle, "use_tcp", NULL, &use_tcp) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "tcpconn", NULL, &tcpconn) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "tcp_server", NULL, &tcp_server) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "tcp_port", NULL, &tcp_port) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "tcp_send", NULL, &tcp_send) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "tcp_time", NULL, &tcp_time) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_get_str(nvs_handle, "device_sn", NULL, &device_sn) ==
        ESP_ERR_NVS_NOT_FOUND)
      nvs_not_found = true;
    if (nvs_not_found) {
      // 在设置默认值之前，先检查并保存已存在的device_sn和device_type
      char existing_device_sn[32] = {0};
      char existing_device_type[32] = {0};
      size_t existing_sn_len = sizeof(existing_device_sn);
      size_t existing_type_len = sizeof(existing_device_type);

      bool has_device_sn = false;
      bool has_device_type = false;

      // 检查device_sn是否已存在
      if (nvs_get_str(nvs_handle, "device_sn", existing_device_sn,
                      &existing_sn_len) == ESP_OK) {
        has_device_sn = true;
        ESP_LOGI(TAG, "保留现有的device_sn: %s", existing_device_sn);
      }

      // 检查device_type是否已存在
      if (nvs_get_str(nvs_handle, "device_type", existing_device_type,
                      &existing_type_len) == ESP_OK) {
        has_device_type = true;
        ESP_LOGI(TAG, "保留现有的device_type: %s", existing_device_type);
      }
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "netconn", ""));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "is_dhcp", "2"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_ip", "192.168.1.84"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_netmask", "255.255.255.0"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_gateway", "192.168.1.1"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_dns1", "8.8.8.8"));
      ESP_ERROR_CHECK(
          nvs_set_str(nvs_handle, "static_dns2", "114.114.114.114"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_ssid", ""));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_password", ""));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_name", ""));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_password", ""));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_wait_time", "0"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_mqtt", "0"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_type", "0"));
      ESP_ERROR_CHECK(
          nvs_set_str(nvs_handle, "mqtt_server", "mqtt.shuoxin-iot.com"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_port", "1883"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_username", ""));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_password", ""));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_clientid", c_mac));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_sub_topic", s_mac));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_pub_topic", p_mac));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "qos", "0"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "retain", "0"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_send", "0"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "mqtt_time", "5"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_http", "0"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "http_port", "5000"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "httpconn", "1"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "http_url", ""));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "http_header", ""));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "http_time", "5"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "host_names", "串口服务器"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ntp_server", ""));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "lgname", "admin"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "lgpwd", "12345678"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_tcp", "1"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcpconn", "0"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcp_server", "192.168.1.100"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcp_port", "8888"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcp_send", "0"));
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcp_time", "5"));
      // 只有在没有现有值时才设置默认的device_type和device_sn
      if (!has_device_type) {
        ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "device_type", "SP501"));
        ESP_LOGI(TAG, "设置默认device_type: SP501");
      } else {
        ESP_ERROR_CHECK(
            nvs_set_str(nvs_handle, "device_type", existing_device_type));
        ESP_LOGI(TAG, "恢复现有device_type: %s", existing_device_type);
      }

      if (!has_device_sn) {
        ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "device_sn", "0000000000000"));
        ESP_LOGI(TAG, "设置默认device_sn: 0000000000000");
      } else {
        ESP_ERROR_CHECK(
            nvs_set_str(nvs_handle, "device_sn", existing_device_sn));
        ESP_LOGI(TAG, "恢复现有device_sn: %s", existing_device_sn);
      }
    }
    // 设置工作模式默认值（统一使用 nvs_namespace）
    nvs_handle_t mode_handle;
    esp_err_t mode_err = nvs_open("nvs_namespace", NVS_READWRITE, &mode_handle);
    if (mode_err == ESP_OK) {
      // 检查w_mode是否存在
      size_t required_size;
      mode_err = nvs_get_str(mode_handle, WORK_MODE_NVS_KEY, NULL, &required_size);
      if (mode_err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_ERROR_CHECK(nvs_set_work_mode(mode_handle, WORK_MODE_DEFAULT));
        printf("设置默认工作模式为TCP透传\n");
      } else {
        // 如果存在，读取当前值并记录日志
        char current_mode[32] = {0};
        nvs_get_work_mode(mode_handle, current_mode, sizeof(current_mode));
        printf("当前工作模式为: %s\n", current_mode);
      }

      // 检查poll_time是否存在
      mode_err = nvs_get_str(mode_handle, "poll_time", NULL, &required_size);
      if (mode_err == ESP_ERR_NVS_NOT_FOUND) {
        // 如果不存在，设置默认值为30000ms（30秒）
        ESP_ERROR_CHECK(nvs_set_str(mode_handle, "poll_time", "30000"));
        printf("设置默认轮询时间为30000ms（30秒）\n");
      }

      // 提交更改
      ESP_ERROR_CHECK(nvs_commit(mode_handle));
      nvs_close(mode_handle);
    }

    ESP_ERROR_CHECK(nvs_commit(nvs_handle));

#define REFRESH_NVS_STR_SIZE(key, size_var)                                      \
    do {                                                                         \
      ret = nvs_get_str(nvs_handle, key, NULL, &(size_var));                     \
      if (ret != ESP_OK) {                                                       \
        ESP_LOGE(TAG, "Failed to get NVS size for %s: %s", key,                  \
                 esp_err_to_name(ret));                                          \
        ESP_ERROR_CHECK(ret);                                                    \
      }                                                                          \
    } while (0)

    REFRESH_NVS_STR_SIZE("netconn", netconn);
    REFRESH_NVS_STR_SIZE("is_dhcp", is_dhcp);
    REFRESH_NVS_STR_SIZE("static_ip", static_ip);
    REFRESH_NVS_STR_SIZE("static_netmask", static_netmask);
    REFRESH_NVS_STR_SIZE("static_gateway", static_gateway);
    REFRESH_NVS_STR_SIZE("wifi_ssid", wifi_ssid);
    REFRESH_NVS_STR_SIZE("wifi_password", wifi_password);
    REFRESH_NVS_STR_SIZE("static_dns1", static_dns1);
    REFRESH_NVS_STR_SIZE("static_dns2", static_dns2);
#undef REFRESH_NVS_STR_SIZE

    sx_protocol_manager_init();
    ESP_ERROR_CHECK(sx_network_manager_init());

    if (wifi_ensure_sta_netif() == NULL) {
      ESP_LOGE(TAG, "WiFi STA netif初始化失败，跳过AP启动前预绑定");
    }

    ESP_ERROR_CHECK(sx_ap_manager_start());
    http_server_init();

    // 分配缓冲区 - 确保至少分配1字节，避免因为size_t为0导致的问题
    // 另外检查size_t值是否合理（小于1MB），防止分配过大内存
#define MAX_NVS_STRING_SIZE 1024
#define SAFE_SIZE(s) ((s) > 0 && (s) < MAX_NVS_STRING_SIZE ? (s) : 64)

    char *nvs_netconn = malloc(SAFE_SIZE(netconn));
    char *nvs_is_dhcp = malloc(SAFE_SIZE(is_dhcp));
    char *nvs_static_ip = malloc(SAFE_SIZE(static_ip));
    char *nvs_static_netmask = malloc(SAFE_SIZE(static_netmask));
    char *nvs_static_gateway = malloc(SAFE_SIZE(static_gateway));
    char *nvs_wifi_ssid = malloc(SAFE_SIZE(wifi_ssid));
    char *nvs_wifi_password = malloc(SAFE_SIZE(wifi_password));
    char *nvs_static_dns1 = malloc(SAFE_SIZE(static_dns1));
    char *nvs_static_dns2 = malloc(SAFE_SIZE(static_dns2));

    // 检查所有内存分配是否成功
    if (nvs_netconn == NULL || nvs_is_dhcp == NULL || nvs_static_ip == NULL ||
        nvs_static_netmask == NULL || nvs_static_gateway == NULL ||
        nvs_wifi_ssid == NULL || nvs_wifi_password == NULL ||
        nvs_static_dns1 == NULL || nvs_static_dns2 == NULL) {

      // 释放已分配的内存
      if (nvs_netconn)
        free(nvs_netconn);
      if (nvs_is_dhcp)
        free(nvs_is_dhcp);
      if (nvs_static_ip)
        free(nvs_static_ip);
      if (nvs_static_netmask)
        free(nvs_static_netmask);
      if (nvs_static_gateway)
        free(nvs_static_gateway);
      if (nvs_wifi_ssid)
        free(nvs_wifi_ssid);
      if (nvs_wifi_password)
        free(nvs_wifi_password);
      if (nvs_static_dns1)
        free(nvs_static_dns1);
      if (nvs_static_dns2)
        free(nvs_static_dns2);
      esp_restart();
    } else {
      // 初始化所有字符串为空字符串，现在所有指针都已确认不为NULL
      memset(nvs_netconn, 0, SAFE_SIZE(netconn));
      memset(nvs_is_dhcp, 0, SAFE_SIZE(is_dhcp));
      memset(nvs_static_ip, 0, SAFE_SIZE(static_ip));
      memset(nvs_static_netmask, 0, SAFE_SIZE(static_netmask));
      memset(nvs_static_gateway, 0, SAFE_SIZE(static_gateway));
      memset(nvs_wifi_ssid, 0, SAFE_SIZE(wifi_ssid));
      memset(nvs_wifi_password, 0, SAFE_SIZE(wifi_password));
      memset(nvs_static_dns1, 0, SAFE_SIZE(static_dns1));
      memset(nvs_static_dns2, 0, SAFE_SIZE(static_dns2));

      ret = nvs_get_str(nvs_handle, "netconn", nvs_netconn, &netconn);
      nvs_get_str(nvs_handle, "is_dhcp", nvs_is_dhcp, &is_dhcp);
      nvs_get_str(nvs_handle, "static_ip", nvs_static_ip, &static_ip);
      nvs_get_str(nvs_handle, "static_netmask", nvs_static_netmask,
                  &static_netmask);
      nvs_get_str(nvs_handle, "static_gateway", nvs_static_gateway,
                  &static_gateway);

      nvs_get_str(nvs_handle, "wifi_ssid", nvs_wifi_ssid, &wifi_ssid);
      nvs_get_str(nvs_handle, "wifi_password", nvs_wifi_password,
                  &wifi_password);
      nvs_get_str(nvs_handle, "static_dns1", nvs_static_dns1, &static_dns1);
      nvs_get_str(nvs_handle, "static_dns2", nvs_static_dns2, &static_dns2);

      // 添加安全检查，确保字符串不为NULL
      if (nvs_is_dhcp == NULL)
        nvs_is_dhcp = "";
      if (nvs_static_ip == NULL)
        nvs_static_ip = "";
      if (nvs_static_netmask == NULL)
        nvs_static_netmask = "";
      if (nvs_static_gateway == NULL)
        nvs_static_gateway = "";
      if (nvs_wifi_ssid == NULL)
        nvs_wifi_ssid = "";
      if (nvs_wifi_password == NULL)
        nvs_wifi_password = "";
      if (nvs_static_dns1 == NULL)
        nvs_static_dns1 = "";
      if (nvs_static_dns2 == NULL)
        nvs_static_dns2 = "";

      if (ret == ESP_OK) {
        if (atoi(nvs_netconn) != 2) {
          uint8_t eth_port_cnt = 0;
          esp_eth_handle_t *eth_handles;
          // ESP_ERROR_CHECK(example_eth_init(&eth_handles, &eth_port_cnt));
          esp_err_t ret = example_eth_init(&eth_handles, &eth_port_cnt);
          if (ret == ESP_OK) {
            bool eth_static_ip_applied = false;
            esp_netif_ip_info_t eth_static_info;
            memset(&eth_static_info, 0, sizeof(eth_static_info));

            if (eth_port_cnt == 1) {
              esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
              esp_netif_t *eth_netif = esp_netif_new(&cfg);

              if (strcmp(nvs_is_dhcp, "1") == 0 ||
                  strcmp(nvs_static_ip, "") == 0 ||
                  strcmp(nvs_static_netmask, "") == 0 ||
                  strcmp(nvs_static_gateway, "") == 0) {
                ESP_LOGI(TAG, "Using DHCP mode");
                ESP_ERROR_CHECK(esp_netif_attach(
                    eth_netif, esp_eth_new_netif_glue(eth_handles[0])));
              } else {
                ESP_ERROR_CHECK(esp_netif_dhcpc_stop(eth_netif));

                // 设置静态IP
                esp_netif_ip_info_t info_t;
                memset(&info_t, 0, sizeof(esp_netif_ip_info_t));
                info_t.ip.addr = esp_ip4addr_aton((const char *)nvs_static_ip);
                info_t.netmask.addr =
                    esp_ip4addr_aton((const char *)nvs_static_netmask);
                info_t.gw.addr =
                    esp_ip4addr_aton((const char *)nvs_static_gateway);

                if (info_t.ip.addr == 0 || info_t.netmask.addr == 0 ||
                    info_t.gw.addr == 0) {
                  ESP_LOGE(TAG, "Invalid static IP configuration");
                  ESP_ERROR_CHECK(esp_netif_attach(
                      eth_netif, esp_eth_new_netif_glue(eth_handles[0])));
                } else {
                  esp_err_t err = esp_netif_set_ip_info(eth_netif, &info_t);
                  if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to set static IP: %s",
                             esp_err_to_name(err));
                    ESP_ERROR_CHECK(esp_netif_attach(
                        eth_netif, esp_eth_new_netif_glue(eth_handles[0])));
                  } else {
                    ESP_LOGI(
                        TAG,
                        "Static IP set successfully - IP:%s, Mask:%s, GW:%s",
                        nvs_static_ip, nvs_static_netmask, nvs_static_gateway);
                    memcpy(&eth_static_info, &info_t, sizeof(eth_static_info));
                    eth_static_ip_applied = true;

                    // 设置DNS
                    if (nvs_static_dns1 != NULL &&
                        strlen(nvs_static_dns1) > 0) {
                      esp_netif_dns_info_t dns_info = {0};
                      dns_info.ip.u_addr.ip4.addr =
                          esp_ip4addr_aton((const char *)nvs_static_dns1);
                      dns_info.ip.type = IPADDR_TYPE_V4;
                      ESP_ERROR_CHECK(esp_netif_set_dns_info(
                          eth_netif, ESP_NETIF_DNS_MAIN, &dns_info));
                      ESP_LOGI(TAG, "Primary DNS set to: %s", nvs_static_dns1);
                    }

                    if (nvs_static_dns2 != NULL &&
                        strlen(nvs_static_dns2) > 0) {
                      esp_netif_dns_info_t dns_info = {0};
                      dns_info.ip.u_addr.ip4.addr =
                          esp_ip4addr_aton((const char *)nvs_static_dns2);
                      dns_info.ip.type = IPADDR_TYPE_V4;
                      ESP_ERROR_CHECK(esp_netif_set_dns_info(
                          eth_netif, ESP_NETIF_DNS_BACKUP, &dns_info));
                      ESP_LOGI(TAG, "Secondary DNS set to: %s",
                               nvs_static_dns2);
                    }

                    ESP_ERROR_CHECK(esp_netif_attach(
                        eth_netif, esp_eth_new_netif_glue(eth_handles[0])));
                  }
                }
              }
            } else {
              esp_netif_inherent_config_t esp_netif_config =
                  ESP_NETIF_INHERENT_DEFAULT_ETH();
              esp_netif_config_t cfg_spi = {.base = &esp_netif_config,
                                            .stack =
                                                ESP_NETIF_NETSTACK_DEFAULT_ETH};
              char if_key_str[10];
              char if_desc_str[10];
              char num_str[3];
              for (int i = 0; i < eth_port_cnt; i++) {
                itoa(i, num_str, 10);
                strcat(strcpy(if_key_str, "ETH_"), num_str);
                strcat(strcpy(if_desc_str, "eth"), num_str);
                esp_netif_config.if_key = if_key_str;
                esp_netif_config.if_desc = if_desc_str;
                esp_netif_config.route_prio -= i * 5;
                esp_netif_t *eth_netif = esp_netif_new(&cfg_spi);
                ESP_ERROR_CHECK(esp_netif_attach(
                    eth_netif, esp_eth_new_netif_glue(eth_handles[i])));
              }
            }
            ESP_ERROR_CHECK(esp_event_handler_register(
                ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
            ESP_ERROR_CHECK(esp_event_handler_register(
                IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));
            ESP_ERROR_CHECK(esp_event_handler_register(
                IP_EVENT, IP_EVENT_ETH_LOST_IP, &got_ip_event_handler, NULL));
            for (int i = 0; i < eth_port_cnt; i++) {
              ESP_ERROR_CHECK(esp_eth_start(eth_handles[i]));
            }
            if (eth_static_ip_applied) {
              sx_network_manager_notify_eth_static_ip(&eth_static_info);
            }
          } else {
            printf("eth init failed\n");
          }

        } else {
          // WiFi模式
          printf("启动WiFi模式，禁用以太网连接\n");
          ESP_LOGI(TAG, "设备设置为WiFi模式，跳过以太网初始化");

          // 确保禁用以太网相关服务，为保险起见，可以在这里添加任何需要的以太网清理代码

          // 初始化WiFi连接
          // wifi_init_sta(nvs_wifi_ssid, nvs_wifi_password);
          start_wifi_task(nvs_wifi_ssid, nvs_wifi_password);
        }

      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
      free(nvs_netconn); // 释放缓冲区
      free(nvs_is_dhcp);
      free(nvs_static_ip);
      free(nvs_static_netmask);
      free(nvs_static_gateway);
      free(nvs_wifi_ssid);
      free(nvs_wifi_password);
    }

    nvs_close(nvs_handle);

    uart_init();
    simple_task();

    // ========== 根据工作模式启动对应任务 ==========
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "开始加载工作模式");
    ESP_LOGI(TAG, "======================================");

    nvs_handle_t work_mode_handle;
    if (nvs_open("nvs_namespace", NVS_READONLY, &work_mode_handle) == ESP_OK) {
        char work_mode[32] = {0};

        if (nvs_get_work_mode(work_mode_handle, work_mode, sizeof(work_mode)) == ESP_OK ||
            work_mode[0] != '\0') {
            ESP_LOGI(TAG, "当前配置的工作模式: %s", work_mode);

            esp_err_t start_err = sx_work_mode_start_by_name(work_mode);
            if (start_err != ESP_OK) {
                ESP_LOGW(TAG, "启动工作模式失败: %s (%s)", work_mode,
                         esp_err_to_name(start_err));
            }
        } else {
            ESP_LOGW(TAG, "未读取到工作模式配置，默认启动透传模式");
            sx_work_mode_start_by_name(WORK_MODE_DEFAULT);
        }

        nvs_close(work_mode_handle);
    } else {
        ESP_LOGE(TAG, "无法打开NVS读取工作模式");
    }

    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "工作模式加载完成");
    ESP_LOGI(TAG, "======================================");

    // 初始化并启动内存监控定时器
    esp_timer_handle_t memory_monitor_timer;
    const esp_timer_create_args_t memory_monitor_timer_args = {
        .callback = &memory_monitor_timer_callback, .name = "memory_monitor"};
    ESP_ERROR_CHECK(
        esp_timer_create(&memory_monitor_timer_args, &memory_monitor_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(memory_monitor_timer,
                                             60000000)); // 每60秒运行一次

    while (1) {
      printf("+++++++++++++++++++++++++++++++++++++++++++++++++++ \n");
      print_sockets();
      printf("+++++++++++++++++++++++++++++++++++++++++++++++++++ \n");
      vTaskDelay(pdMS_TO_TICKS(20000)); // 每20秒统计一次
    }
  }
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
