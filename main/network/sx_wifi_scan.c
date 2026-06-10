/*
 * @Author: Orion
 * @Date: 2024-01-23 17:05:29
 * @LastEditors: Orion
 * @LastEditTime: 2025-06-11 14:53:56
 * @FilePath: \ETH_TH\main\sx_wifi_scan.c
 * @Description: Implemented WIFI scanning function
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#include "sx_wifi_scan.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include <string.h>
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "sx_wifi_scan";

// 添加信号量用于防止多个扫描同时进行
static SemaphoreHandle_t wifi_scan_mutex = NULL;
// 上次扫描时间记录
static int64_t last_scan_time = 0;

// WiFi硬件测试函数
static bool test_wifi_hardware(void) {
  ESP_LOGI(TAG, "Testing WiFi hardware...");

  // 测试1: 检查WiFi是否已初始化
  wifi_mode_t mode;
  esp_err_t ret = esp_wifi_get_mode(&mode);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "WiFi hardware test FAILED: get_mode error %s", esp_err_to_name(ret));
    return false;
  }
  ESP_LOGI(TAG, "WiFi hardware test: get_mode OK, current mode=%d", mode);

  // 测试2: 尝试获取MAC地址
  uint8_t mac[6];
  ret = esp_wifi_get_mac(WIFI_IF_STA, mac);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "WiFi hardware test FAILED: get_mac error %s", esp_err_to_name(ret));
    return false;
  }
  ESP_LOGI(TAG, "WiFi hardware test: MAC address %02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // 测试3: 检查WiFi配置
  wifi_config_t config;
  ret = esp_wifi_get_config(WIFI_IF_STA, &config);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "WiFi hardware test: get_config warning %s", esp_err_to_name(ret));
  } else {
    ESP_LOGI(TAG, "WiFi hardware test: get_config OK");
  }

  ESP_LOGI(TAG, "WiFi hardware test PASSED");
  return true;
}

// 备用扫描函数 - 使用被动扫描
static esp_err_t fallback_wifi_scan(void) {
  ESP_LOGI(TAG, "Trying fallback scan with PASSIVE mode...");

  wifi_scan_config_t fallback_config = {
    .ssid = NULL,
    .bssid = NULL,
    .channel = 0,
    .show_hidden = true,
    .scan_type = WIFI_SCAN_TYPE_PASSIVE,
    .scan_time.passive = 1000  // 被动扫描时间1秒
  };

  return esp_wifi_scan_start(&fallback_config, true);
}

static void restore_wifi_mode_after_scan(wifi_mode_t original_mode) {
  wifi_mode_t restore_mode = original_mode;
  if (original_mode == WIFI_MODE_NULL) {
    restore_mode = WIFI_MODE_STA;
  } else if (original_mode == WIFI_MODE_AP) {
    restore_mode = WIFI_MODE_APSTA;
  }

  ESP_LOGI(TAG, "Restoring WiFi mode after scan: %d -> %d", original_mode,
           restore_mode);
  esp_err_t err = esp_wifi_set_mode(restore_mode);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to restore WiFi mode after scan: %s",
             esp_err_to_name(err));
  }
}

// 初始化互斥锁
static void ensure_mutex_initialized(void) {
  if (wifi_scan_mutex == NULL) {
    wifi_scan_mutex = xSemaphoreCreateMutex();
    if (wifi_scan_mutex == NULL) {
      ESP_LOGE(TAG, "Failed to create WiFi scan mutex");
    }
  }
}

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

/**
 * @description: 比较函数，用于按信号强度排序
 * @param {void*} a 第一个AP记录
 * @param {void*} b 第二个AP记录
 * @return {int} 比较结果
 */
static int compare_rssi(const void *a, const void *b) {
  const wifi_ap_record_t *ap1 = (const wifi_ap_record_t *)a;
  const wifi_ap_record_t *ap2 = (const wifi_ap_record_t *)b;
  // 信号值是负数，越大信号越强
  return ap2->rssi - ap1->rssi;
}

// 扫描只短暂停止STA重连，AP客户端不影响STA联网。
static void resume_wifi_reconnect_after_scan(void) {
  resume_wifi_reconnect();
}

/**
 * @description:
 * @param {WifiValueList} wifiValueList
 * @param {Total APs} scanned
 * @return {*}
 */
WifiValueList wifiSearchValue() {
  WifiValueList wifiValueList;
  uint16_t number = DEFAULT_SCAN_LIST_SIZE;
  memset(wifiValueList.ap_info, 0, sizeof(wifiValueList.ap_info));

  // 初始化返回值
  wifiValueList.ap_count = 0;
  wifiValueList.wait_time = 0;
  wifiValueList.error_code = 0; // 默认无错误

  ESP_LOGI(TAG, "=== WiFi scan request started ===");

  // 执行WiFi硬件测试
  if (!test_wifi_hardware()) {
    ESP_LOGE(TAG, "WiFi hardware test failed, aborting scan");
    wifiValueList.error_code = 3;
    return wifiValueList;
  }

  // 确保互斥锁已初始化
  ensure_mutex_initialized();

  // 尝试获取互斥锁，如果无法获取则返回错误
  if (wifi_scan_mutex == NULL || xSemaphoreTake(wifi_scan_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    ESP_LOGW(TAG, "Cannot acquire WiFi scan mutex, another scan might be in progress");
    wifiValueList.error_code = 3; // 其他错误
    return wifiValueList;
  }

  // 获取当前时间
  int64_t current_time = esp_timer_get_time() / 1000; // 转换为毫秒

  // 检查距离上次扫描是否太近（降低到5秒，提高用户体验）
  if (last_scan_time > 0 && (current_time - last_scan_time) < 5000) {
    ESP_LOGW(TAG, "Last scan was too recent (%lld ms ago), skipping", current_time - last_scan_time);
    wifiValueList.error_code = 1; // 扫描间隔过短
    wifiValueList.wait_time = 5000 - (current_time - last_scan_time); // 剩余等待时间
    xSemaphoreGive(wifi_scan_mutex);
    return wifiValueList;
  }

  // 检查WiFi状态，确保可以安全地扫描
  if (!is_wifi_scan_allowed()) {
    ESP_LOGW(TAG, "WiFi scan not allowed at this time due to reconnection in progress");
    wifiValueList.error_code = 2; // 扫描不允许
    xSemaphoreGive(wifi_scan_mutex);
    return wifiValueList;
  }

  // 暂停WiFi自动重连，确保扫描不会被重连干扰
  pause_wifi_reconnect();

  // 检查WiFi状态
  wifi_mode_t current_mode;
  esp_err_t mode_ret = esp_wifi_get_mode(&current_mode);
  if (mode_ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get WiFi mode: %s", esp_err_to_name(mode_ret));
    wifiValueList.error_code = 3; // WiFi模式获取失败
    resume_wifi_reconnect_after_scan();
    xSemaphoreGive(wifi_scan_mutex);
    return wifiValueList;
  }

  ESP_LOGI(TAG, "Current WiFi mode: %d", current_mode);

  // 检查当前WiFi模式是否支持扫描
  // 保存原始模式
  wifi_mode_t original_mode = current_mode;
  bool mode_changed = false;

  // AP开启时保持APSTA；AP关闭后STA也可以扫描。旧的NULL/AP状态先纠正。
  if (current_mode == WIFI_MODE_NULL || current_mode == WIFI_MODE_AP) {
    mode_changed = true;
    wifi_mode_t scan_mode =
        (current_mode == WIFI_MODE_AP) ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    ESP_LOGI(TAG, "Changing WiFi mode from %d to %d for scanning",
             current_mode, scan_mode);

    // 设置WiFi模式
    esp_err_t ret = esp_wifi_set_mode(scan_mode);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(ret));
      resume_wifi_reconnect_after_scan();
      xSemaphoreGive(wifi_scan_mutex);
      return wifiValueList;
    }
  }

  // 确保WiFi已启动
  esp_err_t ret = esp_wifi_start();
  if (ret != ESP_OK && ret != 0x3002) { // 0x3002 是 ESP_ERR_WIFI_ALREADY_STARTED 的值
    ESP_LOGE(TAG, "Failed to start WiFi: %s (0x%x)", esp_err_to_name(ret), ret);

    // 如果我们改变了模式，尝试恢复
    if (mode_changed) {
      restore_wifi_mode_after_scan(original_mode);
    }

    resume_wifi_reconnect_after_scan();
    xSemaphoreGive(wifi_scan_mutex);
    return wifiValueList;
  }

  // 尝试停止之前的扫描（如果有）
  esp_err_t stop_ret = esp_wifi_scan_stop();
  ESP_LOGI(TAG, "Stop previous scan result: %s", esp_err_to_name(stop_ret));

  // 等待一段时间，确保WiFi状态稳定
  vTaskDelay(pdMS_TO_TICKS(500));

  // 使用更宽松的扫描配置，提高扫描成功率
  wifi_scan_config_t scan_config = {
    .ssid = NULL,           // 扫描所有SSID
    .bssid = NULL,          // 扫描所有BSSID
    .channel = 0,           // 扫描所有信道 (1-13)
    .show_hidden = true,    // 显示隐藏网络
    .scan_type = WIFI_SCAN_TYPE_ACTIVE,  // 主动扫描
    .scan_time.active.min = 120,  // 每个信道最小扫描时间(ms)
    .scan_time.active.max = 500   // 每个信道最大扫描时间(ms) - 增加以确保发现更多网络
  };

  ESP_LOGI(TAG, "Scan config: type=%s, channels=ALL, show_hidden=%s, time=%u-%ums",
           scan_config.scan_type == WIFI_SCAN_TYPE_ACTIVE ? "ACTIVE" : "PASSIVE",
           scan_config.show_hidden ? "YES" : "NO",
           (unsigned int)scan_config.scan_time.active.min,
           (unsigned int)scan_config.scan_time.active.max);

  // 启动扫描并添加错误处理
  ESP_LOGI(TAG, "Starting WiFi scan with config: min=%u, max=%u, show_hidden=%d",
           (unsigned int)scan_config.scan_time.active.min, (unsigned int)scan_config.scan_time.active.max, scan_config.show_hidden);

  ret = esp_wifi_scan_start(&scan_config, true);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Primary WiFi scan (ACTIVE) failed: %s (0x%x)", esp_err_to_name(ret), ret);

    // 尝试备用扫描方法
    ESP_LOGI(TAG, "Attempting fallback scan...");
    ret = fallback_wifi_scan();

    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Fallback WiFi scan (PASSIVE) also failed: %s (0x%x)", esp_err_to_name(ret), ret);

      // 设置具体的错误代码
      if (ret == ESP_ERR_WIFI_NOT_INIT) {
        wifiValueList.error_code = 3; // WiFi未初始化
        ESP_LOGE(TAG, "WiFi not initialized");
      } else if (ret == ESP_ERR_WIFI_NOT_STARTED) {
        wifiValueList.error_code = 3; // WiFi未启动
        ESP_LOGE(TAG, "WiFi not started");
      } else if (ret == ESP_ERR_WIFI_TIMEOUT) {
        wifiValueList.error_code = 3; // 扫描超时
        ESP_LOGE(TAG, "WiFi scan timeout");
      } else {
        wifiValueList.error_code = 3; // 其他错误
      }

      // 如果我们改变了模式，尝试恢复
      if (mode_changed) {
        restore_wifi_mode_after_scan(original_mode);
      }

      resume_wifi_reconnect_after_scan();
      xSemaphoreGive(wifi_scan_mutex);
      return wifiValueList;
    } else {
      ESP_LOGI(TAG, "Fallback scan started successfully");
    }
  } else {
    ESP_LOGI(TAG, "Primary scan started successfully");
  }

  // 更新最后扫描时间
  last_scan_time = current_time;

  // 获取扫描结果
  ESP_LOGI(TAG, "WiFi scan completed, getting results...");

  // 首先获取AP数量
  uint16_t ap_num = 0;
  ret = esp_wifi_scan_get_ap_num(&ap_num);
  wifiValueList.ap_count = (int)ap_num;
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get AP count: %s", esp_err_to_name(ret));
    wifiValueList.error_code = 3; // 获取数量失败

    // 如果我们改变了模式，尝试恢复
    if (mode_changed) {
      restore_wifi_mode_after_scan(original_mode);
    }

    resume_wifi_reconnect_after_scan();
    xSemaphoreGive(wifi_scan_mutex);
    return wifiValueList;
  }

  ESP_LOGI(TAG, "Raw scan result: found %u APs", wifiValueList.ap_count);

  // 如果没有找到AP，直接返回
  if (wifiValueList.ap_count == 0) {
    ESP_LOGW(TAG, "No APs found in scan - this might indicate:");
    ESP_LOGW(TAG, "1. No WiFi networks in range");
    ESP_LOGW(TAG, "2. WiFi antenna issue");
    ESP_LOGW(TAG, "3. Scan parameters too restrictive");
    ESP_LOGW(TAG, "4. Environmental interference");

    // 恢复原始WiFi模式（如果已更改）
    if (mode_changed) {
      ESP_LOGI(TAG, "Restoring original WiFi mode: %d", original_mode);
      restore_wifi_mode_after_scan(original_mode);
    }

    resume_wifi_reconnect_after_scan();
    xSemaphoreGive(wifi_scan_mutex);
    return wifiValueList;
  }

  // 限制获取的记录数量
  if (wifiValueList.ap_count > DEFAULT_SCAN_LIST_SIZE) {
    ESP_LOGW(TAG, "Found %u APs, but limiting to %d", wifiValueList.ap_count, DEFAULT_SCAN_LIST_SIZE);
    number = DEFAULT_SCAN_LIST_SIZE;
    wifiValueList.ap_count = DEFAULT_SCAN_LIST_SIZE;
  } else {
    number = wifiValueList.ap_count;
  }

  ret = esp_wifi_scan_get_ap_records(&number, wifiValueList.ap_info);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get AP records: %s", esp_err_to_name(ret));
    wifiValueList.error_code = 3; // 获取结果失败

    // 如果我们改变了模式，尝试恢复
    if (mode_changed) {
      restore_wifi_mode_after_scan(original_mode);
    }

    resume_wifi_reconnect_after_scan();
    xSemaphoreGive(wifi_scan_mutex);
    return wifiValueList;
  }

  ESP_LOGI(TAG, "WiFi scan successful! Total APs found = %u, records retrieved = %u",
           wifiValueList.ap_count, number);

  // 打印前几个AP的详细信息用于调试
  for (int i = 0; i < (number < 3 ? number : 3); i++) {
    ESP_LOGI(TAG, "AP[%d]: SSID='%s', RSSI=%d, Channel=%d, AuthMode=%d",
             i, wifiValueList.ap_info[i].ssid, wifiValueList.ap_info[i].rssi,
             wifiValueList.ap_info[i].primary, wifiValueList.ap_info[i].authmode);
  }

  // 恢复原始WiFi模式（如果已更改）
  if (mode_changed) {
    ESP_LOGI(TAG, "Restoring original WiFi mode: %d", original_mode);
    restore_wifi_mode_after_scan(original_mode);
  }

  // 确保ap_count不超过DEFAULT_SCAN_LIST_SIZE
  if (wifiValueList.ap_count > DEFAULT_SCAN_LIST_SIZE) {
    wifiValueList.ap_count = DEFAULT_SCAN_LIST_SIZE;
  }

  // 过滤和清理无效数据，处理隐藏网络
  uint16_t valid_count = 0;
  wifi_ap_record_t valid_aps[DEFAULT_SCAN_LIST_SIZE];

  for (int i = 0; i < wifiValueList.ap_count; i++) {
    // 检查记录是否有效 - RSSI和Channel都不应该为0
    if (wifiValueList.ap_info[i].rssi != 0 && wifiValueList.ap_info[i].primary != 0) {
      // 处理隐藏网络 - 如果SSID长度为0，给它一个特殊标记
      if (strlen((const char*)wifiValueList.ap_info[i].ssid) == 0) {
        // 为隐藏网络指定一个名称
        char hidden_name[33]; // ESP32的SSID最大长度为32字节
        snprintf(hidden_name, sizeof(hidden_name), "Hidden_Network_%02X%02X%02X",
                wifiValueList.ap_info[i].bssid[3],
                wifiValueList.ap_info[i].bssid[4],
                wifiValueList.ap_info[i].bssid[5]);
        memcpy(wifiValueList.ap_info[i].ssid, hidden_name, sizeof(hidden_name));
        ESP_LOGI(TAG, "发现隐藏网络，分配名称: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X",
                hidden_name,
                wifiValueList.ap_info[i].bssid[0], wifiValueList.ap_info[i].bssid[1],
                wifiValueList.ap_info[i].bssid[2], wifiValueList.ap_info[i].bssid[3],
                wifiValueList.ap_info[i].bssid[4], wifiValueList.ap_info[i].bssid[5]);
      }

      // 复制有效记录
      memcpy(&valid_aps[valid_count], &wifiValueList.ap_info[i], sizeof(wifi_ap_record_t));
      valid_count++;
    } else {
      ESP_LOGW(TAG, "过滤掉无效的AP记录: RSSI=%d, Channel=%d",
               wifiValueList.ap_info[i].rssi, wifiValueList.ap_info[i].primary);
    }
  }

  // 如果有效记录数量小于总数，则更新列表
  if (valid_count < wifiValueList.ap_count) {
    ESP_LOGI(TAG, "过滤后的有效AP数量: %u (原始: %u)", valid_count, wifiValueList.ap_count);
    // 清空原数组并复制有效记录
    memset(wifiValueList.ap_info, 0, sizeof(wifiValueList.ap_info));
    memcpy(wifiValueList.ap_info, valid_aps, valid_count * sizeof(wifi_ap_record_t));
    wifiValueList.ap_count = valid_count;
  }

  // 按信号强度排序
  if (wifiValueList.ap_count > 0) {
    qsort(wifiValueList.ap_info, wifiValueList.ap_count, sizeof(wifi_ap_record_t), compare_rssi);
  }

  // 检查AP模式客户端并决定是否恢复WiFi重连
  resume_wifi_reconnect_after_scan();

  // 释放互斥锁
  xSemaphoreGive(wifi_scan_mutex);

  return wifiValueList;
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
