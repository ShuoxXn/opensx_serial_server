#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/mpu_wrappers.h"
#include "freertos/task.h"
#include "freertos/queue.h" // 引入队列头文件
#include "hal/uart_types.h"
#include "string.h"

#include "cJSON.h"
#include "driver/gpio.h"
#include "freertos/ringbuf.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_gpio.h"
#include "sx_http_client.h"
#include "sx_mqtt_client.h"
#include "sx_tcp_client.h"
#include "sx_tcp_server.h"
#include "sx_utils.h"
#include "sx_web_server.h"
#include "sx_work_mode.h"
#include <stdio.h>
#include <stdlib.h>

#include "sx_async_uart.h"

// ================= 定义与全局变量 =================

uint8_t dataArray[] = {0};
TaskHandle_t uartTaskHandle = NULL;        // 接收任务句柄
TaskHandle_t uartProcessTaskHandle = NULL; // 处理任务句柄
QueueHandle_t uartDataQueue = NULL;        // 数据传输队列

// 定义队列消息结构体
typedef struct
{
  uint8_t *data; // 指向动态分配的数据缓冲区
  size_t len;    // 数据长度
} uart_queue_msg_t;

#define UART_QUEUE_LEN 20 // 队列深度，允许缓存20帧待处理数据

// GPIO 定义
// #define TXD_PIN (GPIO_NUM_15)
// #define RXD_PIN (GPIO_NUM_2)
#define TXD_PIN (GPIO_NUM_17)
#define RXD_PIN (GPIO_NUM_16)
#define RX_TASK_STOP_BIT (1 << 0)
#define TAG "ASYNCRS485"
#define BUF_SIZE (4096)
#define ECHO_TEST_RTS 2
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)
#define ECHO_READ_TOUT (3)
#define PACKET_READ_TICS (100 / portTICK_PERIOD_MS)

uint8_t *uart_response = NULL; // 接收数据缓冲区（全局，为了兼容旧接口）
uint8_t *uart_tx_data = NULL;  // 发送数据缓冲区
size_t uart_response_size = 0;
size_t uart_tx_data_size = 0;
size_t tx_data_len = 0;
uart_timestamps_t uart_timestamps = {0};

// 修复：添加UART驱动安装状态标志，避免重复删除
static bool uart_driver_installed = false;

int current_modbus_template_target = 0;
#define RING_BUFFER_SIZE 10240

typedef struct
{
  int baudrate;
  int data_bits;
  int stop_bits;
  uart_parity_t parity;
  int frame_time_ms;
  int frame_len_bytes;
  int bits_per_char;
} sx_uart_runtime_config_t;

static sx_uart_runtime_config_t uart_runtime_config = {
    .baudrate = 9600,
    .data_bits = 8,
    .stop_bits = 1,
    .parity = UART_PARITY_DISABLE,
    .frame_time_ms = 50,
    .frame_len_bytes = 512,
    .bits_per_char = 10,
};

static SemaphoreHandle_t uart_config_mutex = NULL; // 保护配置参数的互斥锁
portMUX_TYPE uart_spinlock = portMUX_INITIALIZER_UNLOCKED;
SemaphoreHandle_t uart_mutex = NULL; // UART操作互斥锁

// ================= 辅助函数 =================

static uart_word_length_t normalize_data_bits(uart_word_length_t data_bits)
{
  switch ((int)data_bits)
  {
  case UART_DATA_5_BITS:
  case 5:
    return UART_DATA_5_BITS;
  case UART_DATA_6_BITS:
  case 6:
    return UART_DATA_6_BITS;
  case UART_DATA_7_BITS:
  case 7:
    return UART_DATA_7_BITS;
  case UART_DATA_8_BITS:
  case 8:
  default:
    return UART_DATA_8_BITS;
  }
}

static int data_bits_value(uart_word_length_t data_bits)
{
  switch (data_bits)
  {
  case UART_DATA_5_BITS:
    return 5;
  case UART_DATA_6_BITS:
    return 6;
  case UART_DATA_7_BITS:
    return 7;
  case UART_DATA_8_BITS:
  default:
    return 8;
  }
}

static uart_stop_bits_t normalize_stop_bits(uart_stop_bits_t stop_bits)
{
  if (stop_bits == UART_STOP_BITS_1_5)
    return UART_STOP_BITS_1_5;
  if (stop_bits == UART_STOP_BITS_2 || (int)stop_bits == 3 || (int)stop_bits == 2)
    return UART_STOP_BITS_2;
  return UART_STOP_BITS_1;
}

static int stop_bits_value(uart_stop_bits_t stop_bits)
{
  uart_stop_bits_t normalized = normalize_stop_bits(stop_bits);
  switch (normalized)
  {
  case UART_STOP_BITS_2:
  case UART_STOP_BITS_1_5:
    return 2;
  case UART_STOP_BITS_1:
  default:
    return 1;
  }
}

static uart_parity_t normalize_parity(uart_parity_t parity)
{
  switch (parity)
  {
  case UART_PARITY_DISABLE:
  case UART_PARITY_EVEN:
  case UART_PARITY_ODD:
    return parity;
  default:
    break;
  }
  int raw = (int)parity;
  if (raw == 1)
    return UART_PARITY_EVEN;
  if (raw == 2)
    return UART_PARITY_ODD;
  return UART_PARITY_DISABLE;
}

static void uart_update_runtime_config(int baudrate, int data_bits, int stop_bits,
                                       uart_parity_t parity, int frame_time,
                                       int frame_len, int bits_per_char)
{
  if (data_bits < 5 || data_bits > 8)
    data_bits = 8;
  if (stop_bits < 1)
    stop_bits = 1;
  else if (stop_bits > 2)
    stop_bits = 2;
  if (frame_time <= 0)
    frame_time = 10;
  if (frame_len <= 0)
    frame_len = 256;
  if (bits_per_char <= 0)
    bits_per_char = 10;

  sx_uart_runtime_config_t new_cfg = {
      .baudrate = baudrate,
      .data_bits = data_bits,
      .stop_bits = stop_bits,
      .parity = parity,
      .frame_time_ms = frame_time,
      .frame_len_bytes = frame_len,
      .bits_per_char = bits_per_char,
  };

  // 必须获取互斥锁才能修改配置，避免竞态条件
  if (uart_config_mutex == NULL) {
    ESP_LOGE(TAG, "UART config mutex not initialized");
    return;
  }

  if (xSemaphoreTake(uart_config_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to acquire UART config mutex, config not updated");
    return;
  }

  uart_runtime_config = new_cfg;
  xSemaphoreGive(uart_config_mutex);
}

static sx_uart_runtime_config_t uart_get_runtime_config_snapshot(void)
{
  sx_uart_runtime_config_t snapshot;

  if (uart_config_mutex != NULL &&
      xSemaphoreTake(uart_config_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
  {
    snapshot = uart_runtime_config;
    xSemaphoreGive(uart_config_mutex);
  }
  else
  {
    // 获取失败时使用默认值，而不是无保护读取
    ESP_LOGW(TAG, "Failed to acquire config mutex, using safe defaults");
    snapshot.baudrate = 9600;
    snapshot.data_bits = 8;
    snapshot.stop_bits = 1;
    snapshot.parity = UART_PARITY_DISABLE;
    snapshot.frame_time_ms = 50;
    snapshot.frame_len_bytes = 512;
    snapshot.bits_per_char = 10;
  }
  return snapshot;
}

void stop_rx_task();
void rx_wait();

static inline int calculate_wait_time_ms(size_t byte_count)
{
  if (byte_count == 0)
    return 10;
  sx_uart_runtime_config_t cfg = uart_get_runtime_config_snapshot();
  int baudrate = (cfg.baudrate > 0) ? cfg.baudrate : 9600;
  int bits_per_char = (cfg.bits_per_char > 0) ? cfg.bits_per_char : 10;
  float transmission_time_us = (byte_count * bits_per_char * 1000000.0) / baudrate;
  int wait_time_ms = (int)(transmission_time_us / 1000.0 * 1.5);
  if (wait_time_ms < 10)
    wait_time_ms = 10;
  if (wait_time_ms > 2000)
    wait_time_ms = 2000;
  return wait_time_ms;
}

// uint8_t uart_response[BUF_SIZE] = {0};
size_t response_len = 0;

// ================= UART 配置与初始化 =================

void uart_configure(int baudrate, uart_word_length_t data_bits,
                    uart_parity_t parity, uart_stop_bits_t stop_bits,
                    int frame_time, int frame_len)
{
  ESP_LOGI(TAG, "uart_configure: B=%d D=%d P=%d S=%d FT=%d FL=%d",
           baudrate, data_bits, parity, stop_bits, frame_time, frame_len);

  uart_parity_t uart_parity = normalize_parity(parity);
  uart_word_length_t uart_data_bits = normalize_data_bits(data_bits);
  uart_stop_bits_t uart_stop_bits = normalize_stop_bits(stop_bits);
  int runtime_data_bits = data_bits_value(uart_data_bits);
  int runtime_stop_bits = stop_bits_value(uart_stop_bits);

  uart_config_t uart_config = {
      .baud_rate = baudrate,
      .data_bits = uart_data_bits,
      .parity = uart_parity,
      .stop_bits = uart_stop_bits,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 122,
      .source_clk = UART_SCLK_DEFAULT,
  };

  int bits_per_char = 1 + runtime_data_bits + runtime_stop_bits;
  if (uart_parity != UART_PARITY_DISABLE)
  {
    bits_per_char += 1;
  }
  uart_update_runtime_config(baudrate, runtime_data_bits, runtime_stop_bits,
                             uart_parity, frame_time, frame_len, bits_per_char);

  int timeout_ticks = 1;
  int rx_threshold = 10;
  if (rx_threshold > 120)
    rx_threshold = 120;

  int rx_buffer_size = 256;
  if (frame_len > 128)
    rx_buffer_size = 512;
  if (frame_len > 256)
    rx_buffer_size = 1024;
  if (frame_len > 512)
    rx_buffer_size = 2048;
  if (frame_len > 1024)
    rx_buffer_size = 4096;
  if (frame_len > 2048)
    rx_buffer_size = 4096;

  ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, TXD_PIN, RXD_PIN, ECHO_TEST_RTS,
                               UART_PIN_NO_CHANGE));

  esp_err_t err = uart_driver_install(UART_NUM_2, rx_buffer_size, 0, 0, NULL, 0);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "uart_driver_install error: %d, retrying minimal buffer", err);
    rx_buffer_size = 256;
    err = uart_driver_install(UART_NUM_2, rx_buffer_size, 0, 0, NULL, 0);
    if (err != ESP_OK)
      return;
  }
  uart_driver_installed = true;  // 标记驱动已安装

  ESP_ERROR_CHECK(uart_set_mode(UART_NUM_2, UART_MODE_RS485_HALF_DUPLEX));
  ESP_ERROR_CHECK(uart_set_rx_timeout(UART_NUM_2, timeout_ticks));
  ESP_ERROR_CHECK(uart_set_rx_full_threshold(UART_NUM_2, rx_threshold));

  // 接收缓冲区管理 (仅针对uart_response全局变量的预分配，实际队列使用动态malloc)
  size_t calculated_size = (size_t)frame_len + 32;
  size_t new_response_size;
  if (calculated_size < 64)
    new_response_size = 64;
  else if (calculated_size > UART_IO_BUF_SIZE)
    new_response_size = UART_IO_BUF_SIZE;
  else
    new_response_size = calculated_size;

  portENTER_CRITICAL(&uart_spinlock);
  if (uart_response == NULL)
  {
    uart_response = (uint8_t *)malloc(new_response_size);
    if (uart_response)
    {
      uart_response_size = new_response_size;
      memset(uart_response, 0, uart_response_size);
    }
  }
  else if (uart_response_size < new_response_size)
  {
    uint8_t *new_buffer = (uint8_t *)realloc(uart_response, new_response_size);
    if (new_buffer)
    {
      uart_response = new_buffer;
      uart_response_size = new_response_size;
    }
  }
  portEXIT_CRITICAL(&uart_spinlock);

}

void uart_init(void)
{
  if (uart_mutex == NULL)
    uart_mutex = xSemaphoreCreateMutex();
  if (uart_config_mutex == NULL)
    uart_config_mutex = xSemaphoreCreateMutex();

  // 创建数据队列
  if (uartDataQueue == NULL)
  {
    uartDataQueue = xQueueCreate(UART_QUEUE_LEN, sizeof(uart_queue_msg_t));
    if (uartDataQueue == NULL)
    {
      ESP_LOGE(TAG, "Failed to create UART data queue");
    }
  }

  // 分配默认缓冲区
  if (uart_response == NULL)
  {
    uart_response_size = UART_IO_BUF_SIZE;
    uart_response = (uint8_t *)malloc(uart_response_size);
    if (uart_response)
      memset(uart_response, 0, uart_response_size);
  }

  if (uart_tx_data == NULL)
  {
    uart_tx_data_size = 1024;
    uart_tx_data = (uint8_t *)malloc(uart_tx_data_size);
    if (uart_tx_data)
      memset(uart_tx_data, 0, uart_tx_data_size);
  }

  esp_err_t rets = nvs_init();
  nvs_handle_t nvs_handle;
  if (nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle) != ESP_OK)
    return;

  char *nvs_values[6] = {NULL};
  size_t sizes[6] = {0};
  const char *keys[6] = {"baud_rate", "data_bit", "check_bit",
                         "stop_bit", "frame_time", "frame_len"};
  const char *defaults[6] = {"9600", "8", "0", "0", "50", "512"};
  bool defaults_written = false;

  for (int i = 0; i < 6; i++)
  {
    rets = nvs_get_str(nvs_handle, keys[i], NULL, &sizes[i]);
    if (rets == ESP_ERR_NVS_NOT_FOUND)
    {
      if (!defaults_written)
        ESP_LOGW(TAG, "First run, setting default UART values");
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, keys[i], defaults[i]));
      sizes[i] = strlen(defaults[i]) + 1;
      defaults_written = true;
    }
    else if (rets != ESP_OK)
    {
      ESP_LOGE(TAG, "Failed to read UART config size for %s: %s", keys[i],
               esp_err_to_name(rets));
      nvs_close(nvs_handle);
      return;
    }
  }

  if (defaults_written)
  {
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
  }

  bool allocation_failed = false;
  for (int i = 0; i < 6; i++)
  {
    nvs_values[i] = malloc(sizes[i]);
    if (nvs_values[i] == NULL)
    {
      allocation_failed = true;
      break;
    }
    if (nvs_get_str(nvs_handle, keys[i], nvs_values[i], &sizes[i]) != ESP_OK)
    {
      allocation_failed = true;
      break;
    }
  }

  if (!allocation_failed)
  {
    uart_configure(atoi(nvs_values[0]), atoi(nvs_values[1]),
                   atoi(nvs_values[2]), atof(nvs_values[3]),
                   atoi(nvs_values[4]), atoi(nvs_values[5]));
    rx_wait();
  }

  for (int i = 0; i < 6; i++)
    if (nvs_values[i])
      free(nvs_values[i]);
  nvs_close(nvs_handle);
}

void uart_reinit(int baudrate, uart_word_length_t data_bits,
                 uart_parity_t parity, uart_stop_bits_t stop_bits,
                 int frame_time, int frame_len, bool persist)
{
  (void)persist;
  if (uart_mutex != NULL && xSemaphoreTake(uart_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
  {
    ESP_LOGE(TAG, "UART reinit mutex timeout");
    return;
  }

  stop_rx_task();
  vTaskDelay(pdMS_TO_TICKS(10));
  if (uart_driver_installed)
  {
    esp_err_t err = uart_driver_delete(UART_NUM_2);
    if (err != ESP_OK)
    {
      ESP_LOGW(TAG, "uart_driver_delete failed during reinit: %s", esp_err_to_name(err));
    }
    uart_driver_installed = false;
  }
  uart_configure(baudrate, data_bits, parity, stop_bits, frame_time, frame_len);
  rx_wait();
  ESP_LOGI(TAG, "✓ UART reinit done");

  if (uart_mutex != NULL)
    xSemaphoreGive(uart_mutex);
}

int sendData(char *data, size_t length)
{
  if (uart_mutex != NULL && xSemaphoreTake(uart_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    return -1;

  size_t before_flush_size = 0;
  uart_get_buffered_data_len(UART_NUM_2, &before_flush_size);
  for (int i = 0; i < 3; i++)
  {
    uart_flush_input(UART_NUM_2);
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  gpio_set_level(GPIO_NUM_4, 1);
  uart_timestamps.tx_timestamp = esp_timer_get_time();

  portENTER_CRITICAL(&uart_spinlock);
  if (uart_tx_data == NULL || uart_tx_data_size < length)
  {
    size_t new_size = (length > 1024) ? length * 2 : 1024;
    uint8_t *new_buffer = (uint8_t *)realloc(uart_tx_data, new_size);
    if (new_buffer == NULL)
    {
      portEXIT_CRITICAL(&uart_spinlock);
      if (uart_mutex != NULL)
        xSemaphoreGive(uart_mutex);
      return -1;
    }
    uart_tx_data = new_buffer;
    uart_tx_data_size = new_size;
  }
  memset(uart_tx_data, 0, uart_tx_data_size);
  memcpy(uart_tx_data, data, length);
  tx_data_len = length;
  portEXIT_CRITICAL(&uart_spinlock);

  send_uart_to_websocket((uint8_t *)data, length, true);

  int txBytes = uart_write_bytes(UART_NUM_2, data, length);
  ESP_LOG_BUFFER_HEXDUMP("UART_TX", data, length, ESP_LOG_INFO);

  const uint32_t wait_ms_steps[] = {3, 5, 10, 15, 5, 2};
  for (size_t i = 0; i < (sizeof(wait_ms_steps) / sizeof(wait_ms_steps[0])); i++)
  {
    esp_err_t err = uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(wait_ms_steps[i]));
    if (err == ESP_OK)
      break;
  }

  gpio_set_level(GPIO_NUM_4, 0);
  ESP_LOGD("UART_TX", "RS485 switched to RX mode");

  if (uart_mutex != NULL)
    xSemaphoreGive(uart_mutex);
  return txBytes;
}

void tx_task(uint8_t data[], size_t length)
{
  LED_DAT_ON();
  LED_DAT_OFF();
  sendData((char *)data, length);
  LED_DAT_ON();
  LED_DAT_OFF();
}

void tx_tasks(uint8_t data[], size_t length)
{
  vTaskDelay(pdMS_TO_TICKS(5));
  LED_DAT_ON();
  LED_DAT_OFF();
  uint8_t *data_copy = malloc(length);
  if (data_copy)
  {
    memcpy(data_copy, data, length);
    sendData((char *)data_copy, length);
    free(data_copy);
  }
  LED_DAT_ON();
  LED_DAT_OFF();
}

// ================= 核心重构：处理任务 (消费者) =================
// 这个任务负责处理数据、NVS读取、网络发送等慢速操作
void uart_processing_task(void *arg)
{
  static const char *TAG_PROC = "RX_PROC";
  ESP_LOGI(TAG_PROC, "业务处理任务启动");

  bool wdt_registered = false;
  if (esp_task_wdt_add(NULL) == ESP_OK)
    wdt_registered = true;

  // 初始化部分 (放到循环外减少开销，或者在需要时动态获取)
  // ... (保留原有的NVS初始化检查，如果需要)

  while (1)
  {
    // 修复：检查队列是否已初始化
    if (uartDataQueue == NULL) {
      ESP_LOGE(TAG_PROC, "UART队列未初始化，退出处理任务");
      if (wdt_registered) {
        esp_task_wdt_delete(NULL);
      }
      vTaskDelete(NULL);
      return;
    }

    // 检查队列积压情况，优化清理策略
    UBaseType_t queue_count = uxQueueMessagesWaiting(uartDataQueue);
    if (queue_count > 15) {
      ESP_LOGW(TAG_PROC, "🔴 UART队列积压严重: %d条消息，紧急清理", queue_count);

      // 优化：只保留最新5条消息，丢弃其余
      uart_queue_msg_t discard_msg;
      int cleared = 0;
      int to_keep = 5;
      int to_clear = (queue_count > to_keep) ? (queue_count - to_keep) : 0;

      while (cleared < to_clear && xQueueReceive(uartDataQueue, &discard_msg, 0) == pdTRUE) {
        if (discard_msg.data) {
          free(discard_msg.data);
        }
        cleared++;
      }

      ESP_LOGW(TAG_PROC, "已清理 %d 条旧消息，保留最新 %d 条",
               cleared, uxQueueMessagesWaiting(uartDataQueue));
    }

    // 限流：如果队列仍然过满，暂停接收让处理任务追赶
    if (queue_count > 18) {
      ESP_LOGW(TAG_PROC, "队列接近满载，暂停50ms让处理追赶");
      vTaskDelay(pdMS_TO_TICKS(50));
    }

    uart_queue_msg_t msg;
    // 使用有超时的阻塞等待，避免饥饿时无法喂狗
    if (xQueueReceive(uartDataQueue, &msg, pdMS_TO_TICKS(500)) == pdTRUE)
    {
      if (wdt_registered)
        esp_task_wdt_reset();

      if (msg.data == NULL || msg.len == 0)
      {
        if (msg.data)
          free(msg.data);
        continue;
      }

      int rxBytes = msg.len;
      uint8_t *data = msg.data;

      // 检查数据大小，防止过大数据导致内存问题
      if ((size_t)rxBytes > UART_IO_BUF_SIZE) {
        ESP_LOGW(TAG_PROC, "接收数据过大 (%d bytes)，将截断至 %d bytes", rxBytes, UART_IO_BUF_SIZE);
        rxBytes = UART_IO_BUF_SIZE;
      }

      // 1. 更新全局缓冲区 (为了兼容旧接口)
      // 使用自旋锁保护，因为rx_task虽然不写它了，但sendData可能会realloc它
      portENTER_CRITICAL(&uart_spinlock);
      if (uart_response == NULL || uart_response_size < (size_t)rxBytes)
      {
        size_t new_rx_size = (size_t)rxBytes + 32;
        if (new_rx_size > UART_IO_BUF_SIZE)
          new_rx_size = UART_IO_BUF_SIZE;
        uint8_t *new_buffer = (uint8_t *)realloc(uart_response, new_rx_size);
        if (new_buffer)
        {
          uart_response = new_buffer;
          uart_response_size = new_rx_size;
        }
        else
        {
          ESP_LOGE(TAG_PROC, "realloc失败，无法扩展缓冲区至 %zu bytes", new_rx_size);
          portEXIT_CRITICAL(&uart_spinlock);
          free(data);  // 释放消息数据
          continue;  // 跳过此帧数据
        }
      }
      if (uart_response)
      {
        memset(uart_response, 0, uart_response_size);
        size_t copy_len = ((size_t)rxBytes < uart_response_size) ? (size_t)rxBytes : uart_response_size;
        memcpy(uart_response, data, copy_len);
        response_len = rxBytes;
      }
      portEXIT_CRITICAL(&uart_spinlock);

      // 2. WebSocket 发送（调试用）
      ESP_LOGI(TAG_PROC, "处理 RX 数据: %d 字节", rxBytes);
      LED_DAT_ON();
      send_uart_to_websocket(data, rxBytes, false);

      if (wdt_registered)
        esp_task_wdt_reset();

      sx_work_mode_uart_handler_t handler = sx_work_mode_get_uart_handler();
      if (handler != NULL) {
        handler(data, rxBytes);
      } else {
        ESP_LOGW(TAG_PROC, "未注册工作模式UART处理函数，丢弃 %d 字节", rxBytes);
      }

      LED_DAT_OFF();
      
      // *** 关键：释放 RX 任务分配的内存 ***
      free(msg.data);
    }
    else
    {
      // 即便长时间没有数据也要喂狗
      if (wdt_registered)
        esp_task_wdt_reset();
    }
    
    // 检查是否收到停止信号（通过任务通知）
    uint32_t notified = 0;
    if (xTaskNotifyWait(0, RX_TASK_STOP_BIT, &notified, 0) == pdTRUE)
    {
      if ((notified & RX_TASK_STOP_BIT) != 0) break;
    }
  }

  if (wdt_registered)
    esp_task_wdt_delete(NULL);
    
  portENTER_CRITICAL(&uart_spinlock);
  if (uartProcessTaskHandle == xTaskGetCurrentTaskHandle())
    uartProcessTaskHandle = NULL;
  portEXIT_CRITICAL(&uart_spinlock);
  
  vTaskDelete(NULL);
}

// ================= 核心重构：接收任务 (生产者) =================
// 这个任务只负责快速读取数据并放入队列，保证实时性
void rx_task(void *arg)
{
  static const char *RX_TASK_TAG = "RX_TASK";
  ESP_LOGI(RX_TASK_TAG, "高性能接收任务启动");
  
  bool wdt_registered = false;
  if (esp_task_wdt_add(NULL) == ESP_OK)
    wdt_registered = true;

  int max_frame_len = 512;
  int frame_interval_ms = 50;
  
  sx_uart_runtime_config_t runtime_cfg = uart_get_runtime_config_snapshot();
  if (runtime_cfg.frame_len_bytes > 0)
    max_frame_len = runtime_cfg.frame_len_bytes;
  if (runtime_cfg.frame_time_ms > 0)
    frame_interval_ms = runtime_cfg.frame_time_ms;

  // 分配本地接收缓冲区
  uint8_t *frame_buffer = (uint8_t *)malloc(max_frame_len);
  if (frame_buffer == NULL)
  {
    ESP_LOGE(RX_TASK_TAG, "内存分配失败");
    vTaskDelete(NULL);
    return;
  }

  size_t frame_length = 0;
  int64_t last_data_timestamp = 0;
  bool has_data_flag = false;

  while (1)
  {
    // 1. 检查退出信号
    uint32_t notified = 0;
    xTaskNotifyWait(0, RX_TASK_STOP_BIT, &notified, 0);
    if ((notified & RX_TASK_STOP_BIT) != 0)
      break;
    
    if (wdt_registered) esp_task_wdt_reset();

    // 2. 读取 UART FIFO
    size_t buffered_size = 0;
    uart_get_buffered_data_len(UART_NUM_2, &buffered_size);
    
    if (buffered_size > 0)
    {
      size_t available_space = max_frame_len - frame_length;
      size_t read_size = (buffered_size < available_space) ? buffered_size : available_space;
      
      if (read_size > 0)
      {
        int bytes_read = uart_read_bytes(UART_NUM_2, frame_buffer + frame_length, read_size, 0);
        if (bytes_read > 0)
        {
          frame_length += bytes_read;
          last_data_timestamp = esp_timer_get_time();
          has_data_flag = true;
        }
      }
    }

    // 3. 判断是否成帧 (Buffer满 或 超时)
    bool should_process_frame = false;
    size_t frame_to_process = frame_length;

    if (frame_length >= max_frame_len)
    {
      should_process_frame = true;
      frame_to_process = max_frame_len;
    }
    else if (has_data_flag)
    {
       int64_t elapsed_ms = (esp_timer_get_time() - last_data_timestamp) / 1000;
       if (elapsed_ms >= frame_interval_ms)
       {
         should_process_frame = true;
         frame_to_process = frame_length;
       }
    }

    // 4. 发送到处理队列
    if (should_process_frame && frame_to_process > 0)
    {
      // 动态分配内存用于传递数据
      uint8_t *data_copy = (uint8_t *)malloc(frame_to_process);
      if (data_copy)
      {
        memcpy(data_copy, frame_buffer, frame_to_process);
        
        uart_queue_msg_t msg;
        msg.data = data_copy;
        msg.len = frame_to_process;
        
        // 发送给处理任务，如果队列满则丢弃并释放内存
        if (xQueueSend(uartDataQueue, &msg, 0) != pdTRUE)
        {
          ESP_LOGW(RX_TASK_TAG, "处理队列满，丢弃帧数据");
          free(data_copy);
        }
        else
        {
           uart_timestamps.rx_timestamp = esp_timer_get_time();
        }
      }
      else
      {
        ESP_LOGE(RX_TASK_TAG, "无法分配帧拷贝内存");
      }

      // 重置缓冲区
      if (frame_length > frame_to_process)
      {
        size_t remaining = frame_length - frame_to_process;
        memmove(frame_buffer, frame_buffer + frame_to_process, remaining);
        frame_length = remaining;
        last_data_timestamp = esp_timer_get_time();
        has_data_flag = true;
      }
      else
      {
        frame_length = 0;
        has_data_flag = false;
      }
    }

    // 5. 短暂休眠 (有数据时快跑，没数据时慢跑)
    if (has_data_flag)
      vTaskDelay(pdMS_TO_TICKS(2)); 
    else
      vTaskDelay(pdMS_TO_TICKS(10));
  }

  free(frame_buffer);
  if (wdt_registered) esp_task_wdt_delete(NULL);
  
  portENTER_CRITICAL(&uart_spinlock);
  if (uartTaskHandle == xTaskGetCurrentTaskHandle())
    uartTaskHandle = NULL;
  portEXIT_CRITICAL(&uart_spinlock);
  
  vTaskDelete(NULL);
}

void stop_rx_task()
{
  // 1. 停止接收任务
  TaskHandle_t rx_handle = NULL;
  portENTER_CRITICAL(&uart_spinlock);
  rx_handle = uartTaskHandle;
  portEXIT_CRITICAL(&uart_spinlock);

  if (rx_handle)
  {
    xTaskNotify(rx_handle, RX_TASK_STOP_BIT, eSetBits);
  }

  // 2. 停止处理任务
  TaskHandle_t proc_handle = NULL;
  portENTER_CRITICAL(&uart_spinlock);
  proc_handle = uartProcessTaskHandle;
  portEXIT_CRITICAL(&uart_spinlock);
  
  if (proc_handle)
  {
    xTaskNotify(proc_handle, RX_TASK_STOP_BIT, eSetBits);
    // 发送一个空消息唤醒队列等待
    uart_queue_msg_t dummy = {NULL, 0};
    if (uartDataQueue)
      xQueueSend(uartDataQueue, &dummy, 10); 
  }

  if (rx_handle || proc_handle)
  {
    const int wait_slice_ms = 10;
    const int max_wait_ms = 1000;
    for (int waited = 0; waited < max_wait_ms; waited += wait_slice_ms)
    {
      bool rx_running = false;
      bool proc_running = false;

      portENTER_CRITICAL(&uart_spinlock);
      rx_running = (uartTaskHandle != NULL);
      proc_running = (uartProcessTaskHandle != NULL);
      portEXIT_CRITICAL(&uart_spinlock);

      if (!rx_running && !proc_running)
      {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(wait_slice_ms));
    }

    portENTER_CRITICAL(&uart_spinlock);
    bool rx_still_running = (uartTaskHandle != NULL);
    bool proc_still_running = (uartProcessTaskHandle != NULL);
    portEXIT_CRITICAL(&uart_spinlock);
    if (rx_still_running || proc_still_running)
    {
      ESP_LOGW(TAG, "UART tasks did not stop cleanly (rx=%d, proc=%d)",
               rx_still_running, proc_still_running);
    }
  }
  
  // 清理队列中剩余的数据
  if (uartDataQueue)
  {
     uart_queue_msg_t msg;
     while(xQueueReceive(uartDataQueue, &msg, 0) == pdTRUE)
     {
        if(msg.data) free(msg.data);
     }
  }
}

void rx_wait()
{
  stop_rx_task();

  // 创建队列 (确保已创建)
  if (uartDataQueue == NULL)
    uartDataQueue = xQueueCreate(UART_QUEUE_LEN, sizeof(uart_queue_msg_t));

  // 1. 创建处理任务 (消费者) - 优先级较低，栈较大(涉及NVS/Network)
  // 栈大小设为 8192 或更高，因为它处理复杂的网络栈
  xTaskCreate(uart_processing_task, "uart_proc_task", 8192, NULL, configMAX_PRIORITIES - 3, &uartProcessTaskHandle);

  // 2. 创建接收任务 (生产者) - 优先级高，栈较小
  // 栈大小 4096 足够，因为它只做基本的内存拷贝
  xTaskCreate(rx_task, "uart_rx_task", 4096, NULL, configMAX_PRIORITIES - 2, &uartTaskHandle);
  
  ESP_LOGI(TAG, "UART 任务组已启动 (RX + Process)");
}

void uart_cleanup_buffers(void)
{
  if (uart_response != NULL)
  {
    free(uart_response);
    uart_response = NULL;
    uart_response_size = 0;
  }
  if (uart_tx_data != NULL)
  {
    free(uart_tx_data);
    uart_tx_data = NULL;
    uart_tx_data_size = 0;
  }
}
