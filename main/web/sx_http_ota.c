/* HTTP OTA helper. */

#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#define HASH_LEN 32
#define OTA_URL_SIZE 256
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");
static const char *TAG = "OTA";


/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

static int ota_progress_percent = 0;
static char ota_status[64] = "准备更新";

void ota_progress_callback(size_t downloaded, size_t total) {
    ota_progress_percent = (downloaded * 100) / total;
    snprintf(ota_status, sizeof(ota_status), "已下载: %d%%", ota_progress_percent);
}

int get_ota_progress(void) {
    return ota_progress_percent;
}

const char* get_ota_status(void) {
    return ota_status;
}


esp_err_t _http_event_handler_ota(esp_http_client_event_t *evt) {
  switch (evt->event_id) {
  case HTTP_EVENT_ERROR:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_ON_CONNECTED");
    break;
  case HTTP_EVENT_HEADER_SENT:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_HEADER_SENT");
    break;
  case HTTP_EVENT_ON_HEADER:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key,
             evt->header_value);
    break;
  case HTTP_EVENT_ON_DATA:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
    break;
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_ON_FINISH");
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_DISCONNECTED");
    break;
  case HTTP_EVENT_REDIRECT:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_REDIRECT");
    break;
  }
  return ESP_OK;
}

void simple_ota_task(char *updateUrl) {
  ESP_LOGI(TAG, "Starting OTA example task");
  // "http://manage.iot.whut-smart.com/api/geek-iot/file/download/20231025b5cd6b21-8383-48a7-b151-9c4f3d7807d5/test.bin"
  esp_http_client_config_t config = {
      .url = updateUrl,
      .cert_pem = (char *)server_cert_pem_start,
      .event_handler = _http_event_handler_ota,
      .keep_alive_enable = true,
  };

  config.skip_cert_common_name_check = true;

  esp_https_ota_config_t ota_config = {
      .http_config = &config,
  };
  ESP_LOGI(
      TAG,
      "======================================================================");
  ESP_LOGI(TAG, "Attempting to download update from %s", config.url);
  esp_err_t ret = esp_https_ota(&ota_config);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "OTA Succeed, Rebooting...");
    esp_restart();
  } else {
    ESP_LOGE(TAG, "Firmware upgrade failed");
  }
}

static void print_sha256(const uint8_t *image_hash, const char *label) {
  char hash_print[HASH_LEN * 2 + 1];
  hash_print[HASH_LEN * 2] = 0;
  for (int i = 0; i < HASH_LEN; ++i) {
    sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
  }
  ESP_LOGI(TAG, "%s %s", label, hash_print);
}

void get_sha256_of_partitions(void) {
  uint8_t sha_256[HASH_LEN] = {0};
  esp_partition_t partition;
  // get sha256 digest for bootloader
  partition.address = ESP_BOOTLOADER_OFFSET;
  partition.size = ESP_PARTITION_TABLE_OFFSET;
  partition.type = ESP_PARTITION_TYPE_APP;
  esp_partition_get_sha256(&partition, sha_256);
  print_sha256(sha_256, "SHA-256 for bootloader: ");
  // get sha256 digest for running partition
  esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
  print_sha256(sha_256, "SHA-256 for current firmware: ");
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
