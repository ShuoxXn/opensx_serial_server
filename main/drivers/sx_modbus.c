#include "sx_modbus.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_async_uart.h"
#include "sx_utils.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>


static const char *TAG = "MODBUS";
static TaskHandle_t modbus_task_handle = NULL;
static volatile bool modbus_stop_requested = false;
SemaphoreHandle_t modbus_rtu_response_sem = NULL;
#define MODBUS_RTU_FRAME_TIME_MS 50

static inline void modbus_delay_with_wdt(int delay_ms) {
  while (delay_ms > 0) {
    if (modbus_stop_requested) {
      return;
    }
    int chunk = (delay_ms > 500) ? 500 : delay_ms;
    vTaskDelay(pdMS_TO_TICKS(chunk));
    esp_task_wdt_reset();
    delay_ms -= chunk;
  }
}

// CRC计算函数
uint16_t calculate_crc(uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

void modbus_poll_task(void *arg) {
  // 检查参数有效性
  if (arg == NULL) {
    ESP_LOGE(TAG, "无效的任务参数");
    vTaskDelete(NULL);
    return;
  }

  ModbusTaskConfig *config = (ModbusTaskConfig *)arg;
  if (config->items == NULL || config->items_count <= 0) {
    ESP_LOGE(TAG, "无效的 Modbus 配置");
    // 清理传入的配置内存
    if (config->items) free(config->items);
    free(config);
    vTaskDelete(NULL);
    return;
  }
  
  ESP_LOGI(TAG, "Modbus轮询任务开始，配置项数量: %d", config->items_count);
  TickType_t next_poll_time;
  static const char *TAG = "MODBUS_POLL";

  // 从 NVS 中读取 poll_time
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READONLY, &nvs_handle);
  int poll_time = 30000; // 默认值 30秒（毫秒）

  if (err == ESP_OK) {
    char poll_time_str[32];
    size_t len = sizeof(poll_time_str);
    if (nvs_get_str(nvs_handle, "poll_time", poll_time_str, &len) == ESP_OK) {
      poll_time = atoi(poll_time_str);
    }
    nvs_close(nvs_handle);
  }

    // 计算所有启用模板的总时间（接收超时时间 + 间隔时间）
  int total_template_time = 0;
  int enabled_count = 0;
  for (int i = 0; i < config->items_count; i++) {
    ModbusItemConfig *item = &config->items[i];
    if (item->enabled) {
      int timeout = atoi(item->timeout);
      int interval_time = atoi(item->interval_time);
      int template_total_time = timeout + interval_time;
      total_template_time += template_total_time;
      enabled_count++;
      ESP_LOGI(TAG, "模板 %d: 超时=%dms, 间隔=%dms, 总计=%dms",
               i+1, timeout, interval_time, template_total_time);
    }
  }

  // 验证轮询间隔时间是否足够
  if (total_template_time > 0 && enabled_count > 0) {
    int min_poll_time = total_template_time + 1000; // 加1秒缓冲
    ESP_LOGI(TAG, "总共%d个启用模板，总执行时间=%dms，最小轮询时间=%dms",
             enabled_count, total_template_time, min_poll_time);

    if (poll_time < min_poll_time) {
      ESP_LOGW(TAG, "轮询间隔时间 %d ms 小于所需的最小时间 %d ms，自动调整", poll_time, min_poll_time);
      poll_time = min_poll_time;

      // 将调整后的值保存回NVS
      nvs_handle_t nvs_write_handle;
      if (nvs_open("nvs_namespace", NVS_READWRITE, &nvs_write_handle) == ESP_OK) {
        char adjusted_poll_time_str[32];
        snprintf(adjusted_poll_time_str, sizeof(adjusted_poll_time_str), "%d", poll_time);
        nvs_set_str(nvs_write_handle, "poll_time", adjusted_poll_time_str);
        nvs_commit(nvs_write_handle);
        nvs_close(nvs_write_handle);
        ESP_LOGI(TAG, "轮询间隔时间已自动调整为 %d ms (%.1f 秒)", poll_time, poll_time / 1000.0);
      }
    }
  }

  ESP_LOGI(TAG, "轮询间隔时间设置为 %d ms (%.1f 秒)", poll_time, poll_time / 1000.0);

  modbus_stop_requested = false;

  if (modbus_rtu_response_sem == NULL) {
    modbus_rtu_response_sem = xSemaphoreCreateBinary();
    if (modbus_rtu_response_sem == NULL) {
      ESP_LOGE(TAG, "无法创建Modbus RTU响应信号量");
      if (config->items) free(config->items);
      free(config);
      vTaskDelete(NULL);
      return;
    }
  }

  // 注册当前任务到看门狗
  esp_err_t wdt_err = esp_task_wdt_add(NULL);
  if (wdt_err == ESP_OK) {
    ESP_LOGI(TAG, "Modbus任务已注册到看门狗");
  } else {
    ESP_LOGW(TAG, "Modbus任务看门狗注册失败: %s", esp_err_to_name(wdt_err));
  }

  next_poll_time = xTaskGetTickCount();

  while (!modbus_stop_requested) {
    // 喂狗，防止看门狗重启
    esp_task_wdt_reset();
    
    // 检查内存状况，防止内存不足导致死机
    size_t free_heap = esp_get_free_heap_size();
    if (free_heap < 8192) { // 小于8KB时警告
      ESP_LOGW(TAG, "内存不足警告: 剩余堆内存 %zu bytes", free_heap);
      if (free_heap < 2048) { // 小于2KB时停止任务
        ESP_LOGE(TAG, "内存严重不足，停止Modbus任务以防死机");
        break;
      }
    }
    
    TickType_t cycle_start_time = xTaskGetTickCount();
    ESP_LOGI(TAG, "开始轮询周期，共有 %d 个命令，剩余内存: %zu bytes", 
             config->items_count, free_heap);

    for (int i = 0; i < config->items_count && !modbus_stop_requested; i++) {
      ModbusItemConfig *item = &config->items[i];

      // 验证配置项有效性
      if (item == NULL) {
        ESP_LOGE(TAG, "配置项 %d 为空，跳过", i + 1);
        continue;
      }

      if (!item->enabled) {
        ESP_LOGI(TAG, "命令 %d 被禁用，跳过", i + 1);
        continue;
      }
      
      // 在每个命令处理前重置看门狗（防止单个命令执行时间过长）
      esp_task_wdt_reset();

      // 打印数据格式
      ESP_LOGI(TAG, "命令 %d 数据格式: %s", i + 1, item->data_format);

      // 设置当前处理的模板索引，并打印日志
      current_modbus_template_target = i;
      ESP_LOGI(TAG, "处理命令 %d (索引: %d)", i + 1,
               current_modbus_template_target);

      // 验证串口参数有效性
      int baud_rate = atoi(item->baud_rate);
      int data_bit = atoi(item->data_bit);
      float stop_bit = atof(item->stop_bit);
      
      if (baud_rate <= 0 || data_bit < 5 || data_bit > 8 || stop_bit < 1.0) {
        ESP_LOGE(TAG, "无效的串口参数 %d: baud=%d, data=%d, stop=%.1f", 
                 i + 1, baud_rate, data_bit, stop_bit);
        continue;
      }

      // 更新串口参数
      uart_parity_t parity = UART_PARITY_DISABLE;
      if (strcmp(item->check_bit, "Odd") == 0) {
        parity = UART_PARITY_ODD;
      } else if (strcmp(item->check_bit, "Even") == 0) {
        parity = UART_PARITY_EVEN;
      }

      // 重新配置串口（添加错误处理）
      int interval_time_raw = atoi(item->interval_time);
      if (interval_time_raw <= 0) {
        interval_time_raw = 100;
      }
      int frame_time = MODBUS_RTU_FRAME_TIME_MS;

      ESP_LOGI(TAG,
               "配置串口: baud=%d, data=%d, parity=%d, stop=%.1f, frame_time=%d",
               baud_rate, data_bit, parity, stop_bit, frame_time);

      uart_reinit(baud_rate, data_bit, parity, stop_bit, frame_time, 512, false);

      vTaskDelay(pdMS_TO_TICKS(50));
      esp_task_wdt_reset();

      // 构建Modbus请求，验证参数范围
      int slave_addr = atoi(item->slave_addr);
      int function_code = atoi(item->function_code);
      int reg_addr = atoi(item->register_addr);
      int reg_num = atoi(item->register_num);
      
      // 验证Modbus参数
      if (slave_addr < 1 || slave_addr > 247) {
        ESP_LOGE(TAG, "无效的从站地址 %d，跳过命令 %d", slave_addr, i + 1);
        continue;
      }
      if (function_code < 1 || function_code > 6) {
        ESP_LOGE(TAG, "无效的功能码 %d，跳过命令 %d", function_code, i + 1);
        continue;
      }
      if (reg_addr < 0 || reg_addr > 65535 || reg_num < 1 || reg_num > 125) {
        ESP_LOGE(TAG, "无效的寄存器参数 addr=%d num=%d，跳过命令 %d", 
                 reg_addr, reg_num, i + 1);
        continue;
      }
      
      uint8_t request[8];
      request[0] = (uint8_t)slave_addr;
      request[1] = (uint8_t)function_code;
      request[2] = (reg_addr >> 8) & 0xFF;
      request[3] = reg_addr & 0xFF;
      request[4] = (reg_num >> 8) & 0xFF;
      request[5] = reg_num & 0xFF;

      uint16_t crc = calculate_crc(request, 6);
      request[6] = crc & 0xFF;
      request[7] = (crc >> 8) & 0xFF;

      ESP_LOGI(TAG, "发送Modbus请求 %d: slave=%d, func=%d, addr=%d, num=%d", 
               i + 1, slave_addr, function_code, reg_addr, reg_num);

      while (xSemaphoreTake(modbus_rtu_response_sem, 0) == pdTRUE) {
      }
      tx_tasks(request, sizeof(request));

      // 等待响应超时时间（验证范围）
      int timeout = atoi(item->timeout);
      if (timeout < 100 || timeout > 10000) {
        ESP_LOGW(TAG, "超时时间 %d ms 超出范围，调整为 1000 ms", timeout);
        timeout = 1000;
      }

      ESP_LOGI(TAG, "等待Modbus响应，超时=%dms", timeout);
      bool got_response = false;
      int waited_ms = 0;
      while (!modbus_stop_requested && waited_ms < timeout) {
        int wait_ms = (timeout - waited_ms > 500) ? 500 : (timeout - waited_ms);
        if (xSemaphoreTake(modbus_rtu_response_sem, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
          got_response = true;
          break;
        }
        waited_ms += wait_ms;
        esp_task_wdt_reset();
      }

      int interval_time = interval_time_raw;
      ESP_LOGI(TAG, "命令 %d %s，等待模板间隔 %dms",
               i + 1, got_response ? "收到响应" : "响应超时", interval_time);
      if (interval_time > 0) {
        modbus_delay_with_wdt(interval_time);
      }
    }

    // 计算本次循环实际耗时
    TickType_t current_time = xTaskGetTickCount();
    int elapsed_ms = (current_time - cycle_start_time) * portTICK_PERIOD_MS;

    ESP_LOGI(TAG, "循环完成，耗时 %d 毫秒", elapsed_ms);

    // 计算剩余时间，并在等待期间定期喂狗
    int remaining_time = poll_time - elapsed_ms;
    if (remaining_time > 0) {
      // 每秒喂一次狗
      modbus_delay_with_wdt(remaining_time);
    }

    next_poll_time += pdMS_TO_TICKS(poll_time);
  }
  
  // 正常情况下不会到达这里，但为了安全起见添加清理代码
  ESP_LOGW(TAG, "Modbus轮询任务退出，清理资源");
  
  // 取消看门狗注册
  esp_task_wdt_delete(NULL);
  
  if (config) {
    if (config->items) free(config->items);
    free(config);
  }

  modbus_stop_requested = false;
  modbus_task_handle = NULL;
  vTaskDelete(NULL);
}

// 停止Modbus任务（安全方式）
void stop_modbus_tasks(void) {
  if (modbus_task_handle == NULL) {
    return;
  }

  ESP_LOGI(TAG, "正在安全停止Modbus任务...");
  modbus_stop_requested = true;

  // 修复：增加等待时间到10秒，并使用任务通知立即唤醒
  // 如果任务在阻塞等待，通知它立即检查停止标志
  if (modbus_task_handle != NULL) {
    xTaskNotifyGive(modbus_task_handle);
  }

  // 等待任务自行退出
  const int wait_slice_ms = 50;
  for (int i = 0; i < 200; i++) { // 最多等待10秒
    if (modbus_task_handle == NULL) {
      ESP_LOGI(TAG, "Modbus任务已安全停止");
      modbus_stop_requested = false;
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(wait_slice_ms));
  }

  // 如果10秒后任务仍未退出，记录详细错误信息
  ESP_LOGE(TAG, "严重：Modbus任务10秒后仍未退出");
  ESP_LOGE(TAG, "堆内存: %zu bytes", esp_get_free_heap_size());
  ESP_LOGE(TAG, "警告：将强制删除任务，可能导致资源泄漏或死锁");

  // 强制删除（最后手段）
  vTaskDelete(modbus_task_handle);
  modbus_task_handle = NULL;
  modbus_stop_requested = false;

  // 建议重启设备以完全清理状态
  ESP_LOGE(TAG, "建议重启设备以完全清理可能泄漏的资源");
}

// 启动Modbus任务
esp_err_t start_modbus_task(ModbusTaskConfig *config) {
  if (config == NULL || config->items == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  // 确保之前的任务已经完全停止
  if (modbus_task_handle != NULL) {
    ESP_LOGW(TAG, "Modbus任务正在运行，先停止现有任务");
    stop_modbus_tasks();
  }

  modbus_stop_requested = false;

  // 创建任务（增加栈大小以防止栈溢出）
  BaseType_t ret =
      xTaskCreate(modbus_poll_task, "modbus_task", 8192, (void *)config,
                  configMAX_PRIORITIES - 3, &modbus_task_handle);

  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create Modbus task, free heap: %" PRIu32 " bytes", 
             esp_get_free_heap_size());
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Modbus任务启动成功，句柄: %p", modbus_task_handle);
  return ESP_OK;
}
