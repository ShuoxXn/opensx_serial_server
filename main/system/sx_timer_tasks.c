/* Timer-based system tasks. */

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"  // 添加信号量头文件
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_http_client.h"



#include "sx_gpio.h"


#include "sx_utils.h"
#include "sx_network_manager.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// #include "sx_http_tls_fix.h" // 引入新的TLS修复头文件
#include "esp_tls.h"
#include <inttypes.h>
// 定义MIN宏，用于HTTP处理程序中
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
// 在此处手动定义禁用证书验证的宏
#ifndef CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
#define CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY 1
#endif
// 添加一个辅助宏来确保跳过证书验证
#define SKIP_CERT_VERIFICATION true

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

// 定义HTTP缓冲区大小
#define MAX_HTTP_OUTPUT_BUFFER 2048

static const char *TAG = "SX_TIMER";
char strftime_buf[64] = "--:--:--";

// 添加HTTP请求互斥量，避免同时执行多个HTTP请求
SemaphoreHandle_t http_mutex = NULL;

// 新增LED闪烁定时器回调函数声明
void time_sync_led_callback(void *arg);
// HTTP事件处理函数声明
esp_err_t _timer_http_event_handler(esp_http_client_event_t *evt);
// 新增：初始化按键扫描定时器函数声明
esp_err_t init_key_timer(void);

// 新增初始化LED闪烁定时器的函数，使用独立的定时器任务
esp_err_t init_led_timer(void);
// 按键定时器回调函数声明
static void key_timer_callback(void *arg);

// 定时器句柄（声明为extern后需要在此定义）
esp_timer_handle_t led_timer = NULL;
esp_timer_handle_t key_timer = NULL;
esp_timer_handle_t i2c_timer = NULL;
esp_timer_handle_t free_timer = NULL;
esp_timer_handle_t time_sync_led_timer = NULL; // 导出LED闪烁定时器句柄，去掉static关键字

static void request_network_recheck(void) {
    sx_net_event_t event = {
        .type = SX_NET_EVT_PERIODIC_CHECK,
        .source_if = SX_NET_IF_NONE,
    };
    sx_network_manager_post_event(&event);
}

static void update_uptime_string(void) {
    int64_t time_since_boot = esp_timer_get_time();
    int days = time_since_boot / (1000000LL * 60 * 60 * 24);
    int hours = (time_since_boot % (1000000LL * 60 * 60 * 24)) /
                (1000000LL * 60 * 60);
    int minutes = (time_since_boot % (1000000LL * 60 * 60)) /
                  (1000000LL * 60);

    if (days > 0) {
        snprintf(strftime_buf, sizeof(strftime_buf), "%d days, %d hours, %d minutes",
                 days, hours, minutes);
    } else if (hours > 0) {
        snprintf(strftime_buf, sizeof(strftime_buf), "%d hours, %d minutes",
                 hours, minutes);
    } else {
        snprintf(strftime_buf, sizeof(strftime_buf), "%d minutes", minutes);
    }
}

void init_load_time_from_nvs(void) {
    update_uptime_string();
}
static bool reset_requested = false;      // 标记是否请求重置设备
static bool restart_requested = false;    // 标记是否请求重启设备
static bool reset_task_running = false;   // 标记重置/重启任务是否已经在运行

// 时间同步任务句柄
TaskHandle_t time_sync_task_handle = NULL;

// 时间同步LED闪烁定时器回调函数
void time_sync_led_callback(void *arg) {
    static int intranet_flash_step = 0;  // 内网模式闪烁步骤计数器
    static int intranet_wait_counter = 0;  // 内网模式长灭等待计数器
    static bool last_intranet_mode = false;  // 记录上次的内网模式状态
    static bool last_network_connected = false;  // 记录上次的网络连接状态
    static bool last_wifi_active = false;  // 记录上次的WiFi状态
    extern bool is_network_connected; // 声明外部变量，用于判断网络是否连接
    extern bool is_intranet_mode;     // 声明外部变量，用于判断是否为内网模式
    extern bool is_wifi_active;       // 声明外部变量，用于判断是否为WiFi模式

    // 检测网络连接状态变化
    if (last_network_connected != is_network_connected) {
        ESP_LOGI(TAG, "LED检测到网络连接状态变化: %s -> %s",
                 last_network_connected ? "已连接" : "未连接",
                 is_network_connected ? "已连接" : "未连接");
        last_network_connected = is_network_connected;

        // 重置所有计数器和状态
        intranet_flash_step = 0;
        intranet_wait_counter = 0;
    }

    // 检测内网模式状态变化，重置相关计数器
    if (last_intranet_mode != is_intranet_mode) {
        ESP_LOGI(TAG, "LED检测到网络模式变化: %s -> %s",
                 last_intranet_mode ? "内网" : "外网",
                 is_intranet_mode ? "内网" : "外网");

        // 重置所有计数器和状态
        intranet_flash_step = 0;
        intranet_wait_counter = 0;

        last_intranet_mode = is_intranet_mode;
    }

    // 检测WiFi状态变化，重置相关计数器
    if (last_wifi_active != is_wifi_active) {
        ESP_LOGI(TAG, "LED检测到网络类型变化: %s -> %s",
                 last_wifi_active ? "WiFi" : "以太网",
                 is_wifi_active ? "WiFi" : "以太网");

        // 重置所有计数器和状态
        intranet_flash_step = 0;
        intranet_wait_counter = 0;

        last_wifi_active = is_wifi_active;
    }

    if (is_wifi_active) {
        LED_LAN_OFF();
    } else {
        LED_WIFI_OFF();
    }

    if (is_network_connected) {
        if (is_intranet_mode) {
            // 内网模式：快闪两下，正常灭一下的模式
            // 总周期约2秒：快闪(100ms亮+100ms灭) + 快闪(100ms亮+100ms灭) + 长灭(1.6秒)
            // 由于定时器现在是100ms周期，每次回调就是100ms间隔

            switch (intranet_flash_step) {
                case 0: // 第一次快闪 - 亮
                    if (is_wifi_active) {
                        LED_WIFI_ON();
                    } else {
                        LED_LAN_ON();
                    }
                    intranet_flash_step = 1;
                    intranet_wait_counter = 0; // 重置等待计数器
                    ESP_LOGD(TAG, "内网模式%sLED: 第一次快闪亮", is_wifi_active ? "WiFi" : "以太网");
                    break;
                case 1: // 第一次快闪 - 灭
                    if (is_wifi_active) {
                        LED_WIFI_OFF();
                    } else {
                        LED_LAN_OFF();
                    }
                    intranet_flash_step = 2;
                    ESP_LOGD(TAG, "内网模式%sLED: 第一次快闪灭", is_wifi_active ? "WiFi" : "以太网");
                    break;
                case 2: // 第二次快闪 - 亮
                    if (is_wifi_active) {
                        LED_WIFI_ON();
                    } else {
                        LED_LAN_ON();
                    }
                    intranet_flash_step = 3;
                    ESP_LOGD(TAG, "内网模式%sLED: 第二次快闪亮", is_wifi_active ? "WiFi" : "以太网");
                    break;
                case 3: // 第二次快闪 - 灭，然后进入长灭状态
                    if (is_wifi_active) {
                        LED_WIFI_OFF();
                    } else {
                        LED_LAN_OFF();
                    }
                    intranet_flash_step = 4;
                    intranet_wait_counter = 0; // 重置等待计数器
                    ESP_LOGD(TAG, "内网模式%sLED: 第二次快闪灭，开始长灭", is_wifi_active ? "WiFi" : "以太网");
                    break;
                case 4: // 长灭状态，等待1.6秒（16个100ms周期）后重新开始
                    // 确保LED在长灭期间保持关闭状态
                    if (is_wifi_active) {
                        LED_WIFI_OFF();
                    } else {
                        LED_LAN_OFF();
                    }
                    intranet_wait_counter++;
                    if (intranet_wait_counter >= 16) { // 16 * 100ms = 1.6秒
                        intranet_flash_step = 0; // 重新开始循环
                        intranet_wait_counter = 0; // 重置等待计数器
                        ESP_LOGD(TAG, "内网模式%sLED: 长灭结束，重新开始循环", is_wifi_active ? "WiFi" : "以太网");
                    }
                    break;
                default:
                    // 异常状态，重置到初始状态
                    intranet_flash_step = 0;
                    intranet_wait_counter = 0;
                    if (is_wifi_active) {
                        LED_WIFI_OFF();
                    } else {
                        LED_LAN_OFF();
                    }
                    ESP_LOGW(TAG, "内网模式%sLED: 异常状态，重置到初始状态", is_wifi_active ? "WiFi" : "以太网");
                    break;
            }
        } else {
            // 外网模式：常亮
            if (is_wifi_active) {
                LED_WIFI_ON();
                ESP_LOGD(TAG, "外网模式WiFi LED常亮");
            } else {
                LED_LAN_ON();
                ESP_LOGD(TAG, "外网模式以太网LED常亮");
            }
            intranet_flash_step = 0; // 重置内网闪烁步骤
            intranet_wait_counter = 0; // 重置内网等待计数器
        }
    } else {
        // 网络断开时LED长灭
        if (is_wifi_active) {
            LED_WIFI_OFF();
            ESP_LOGD(TAG, "WiFi网络断开，WiFi LED长灭");
        } else {
            LED_LAN_OFF();
            ESP_LOGD(TAG, "以太网断开，以太网LED长灭");
        }
        intranet_flash_step = 0; // 重置内网闪烁步骤
        intranet_wait_counter = 0; // 重置内网等待计数器
    }
}

// 初始化按键扫描定时器
esp_err_t init_key_timer(void) {
    const char *TAG = "init_key_timer";

    // 创建按键扫描定时器，10ms扫描一次
    esp_timer_create_args_t key_timer_args = {
        .callback = &key_timer_callback,
        .name = "key_timer",
        .dispatch_method = ESP_TIMER_TASK,  // 使用专用的定时器任务
        .skip_unhandled_events = true       // 跳过未处理的事件，避免积压
    };

    esp_err_t err = esp_timer_create(&key_timer_args, &key_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法创建按键定时器: %s", esp_err_to_name(err));
        return err;
    }

    // 启动按键定时器，10ms周期
    err = esp_timer_start_periodic(key_timer, 10 * 1000); // 10ms
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法启动按键定时器: %s", esp_err_to_name(err));
        esp_timer_delete(key_timer);
        key_timer = NULL;
        return err;
    }

    ESP_LOGI(TAG, "按键定时器初始化成功，10ms扫描周期");
    return ESP_OK;
}



// 新增初始化LED闪烁定时器的函数，使用独立的定时器任务
esp_err_t init_led_timer(void) {
    const char *TAG = "init_led_timer";

    // 如果LED定时器已存在，先停止并删除
    if (time_sync_led_timer != NULL) {
        ESP_LOGI(TAG, "清理已存在的LED闪烁定时器");
        esp_timer_stop(time_sync_led_timer);
        esp_timer_delete(time_sync_led_timer);
        time_sync_led_timer = NULL;
    }

    // 创建LED闪烁定时器，使用独立的定时器任务，避免被心跳检查阻塞
    esp_timer_create_args_t led_timer_args = {
        .callback = &time_sync_led_callback,
        .name = "time_sync_led_timer",
        .dispatch_method = ESP_TIMER_TASK,  // 使用专用的定时器任务，确保不被阻塞
        .skip_unhandled_events = true       // 跳过未处理的事件，避免积压
    };

    esp_err_t err = esp_timer_create(&led_timer_args, &time_sync_led_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法创建LED闪烁定时器: %s", esp_err_to_name(err));
        return err;
    }

    // 启动LED闪烁定时器，100ms周期，支持内网快闪
    err = esp_timer_start_periodic(time_sync_led_timer, 100 * 1000); // 100ms
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法启动LED闪烁定时器: %s", esp_err_to_name(err));
        esp_timer_delete(time_sync_led_timer);
        time_sync_led_timer = NULL;
        return err;
    }

    ESP_LOGI(TAG, "LED闪烁定时器初始化成功，100ms扫描周期，使用独立定时器任务防止阻塞");
    return ESP_OK;
}

// 初始化所有定时器任务，替代原来的FreeRTOS任务
esp_err_t init_timer_tasks(void) {
    // 创建HTTP请求互斥量
    if (http_mutex == NULL) {
        http_mutex = xSemaphoreCreateMutex();
        if (http_mutex == NULL) {
            ESP_LOGE(TAG, "无法创建HTTP请求互斥量");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "HTTP互斥量创建成功");
    } else {
        ESP_LOGI(TAG, "HTTP互斥量已存在，无需重新创建");
    }

    // 基本功能定时器（按键、I2C、LED、内存监控）已在app_main中启动
    // 网络状态刷新已收口到 sx_network_manager，这里只清理旧网络定时器。

    ESP_LOGI(TAG, "网络状态由 sx_network_manager 周期刷新");

    ESP_LOGI(TAG, "请求网络管理器执行初始网络检查");
    request_network_recheck();

    return ESP_OK;
}

// 停止所有定时器任务
esp_err_t stop_timer_tasks(void) {
    esp_err_t err;
    esp_err_t result = ESP_OK;  // 跟踪整体结果

    if (key_timer) {
        err = esp_timer_stop(key_timer);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "无法停止按键扫描定时器: %s", esp_err_to_name(err));
            result = err;
        }
        err = esp_timer_delete(key_timer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "无法删除按键扫描定时器: %s", esp_err_to_name(err));
            result = err;
        }
        key_timer = NULL;
    }

    if (i2c_timer) {
        err = esp_timer_stop(i2c_timer);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "无法停止I2C传感器读取定时器: %s", esp_err_to_name(err));
            result = err;
        }
        err = esp_timer_delete(i2c_timer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "无法删除I2C传感器读取定时器: %s", esp_err_to_name(err));
            result = err;
        }
        i2c_timer = NULL;
    }

    if (free_timer) {
        err = esp_timer_stop(free_timer);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "无法停止内存监控定时器: %s", esp_err_to_name(err));
            result = err;
        }
        err = esp_timer_delete(free_timer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "无法删除内存监控定时器: %s", esp_err_to_name(err));
            result = err;
        }
        free_timer = NULL;
    }

    if (time_sync_led_timer) {
        err = esp_timer_stop(time_sync_led_timer);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "无法停止时间同步LED闪烁定时器: %s", esp_err_to_name(err));
            result = err;
        }
        err = esp_timer_delete(time_sync_led_timer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "无法删除时间同步LED闪烁定时器: %s", esp_err_to_name(err));
            result = err;
        }
        time_sync_led_timer = NULL;
    }

    // 删除HTTP互斥量
    if (http_mutex != NULL) {
        vSemaphoreDelete(http_mutex);
        http_mutex = NULL;
    }

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "定时器任务已全部正常停止");
    } else {
        ESP_LOGW(TAG, "定时器任务停止过程中出现一些错误，但已尽可能清理");
    }
    return result;
}

// 处理重置和重启的任务
static void reset_restart_task(void *pvParameters) {
    if (reset_requested) {
        printf("执行完全重置...\n");

        // 先停止LED闪烁定时器，防止定时器回调覆盖LED状态
        if (time_sync_led_timer != NULL) {
            esp_err_t err = esp_timer_stop(time_sync_led_timer);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "停止LED定时器失败: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "LED闪烁定时器已停止");
            }
        }

        // 重置配置
        ESP_ERROR_CHECK(nvs_safe_reset());
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
    } else if (restart_requested) {
        printf("执行重启...\n");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    // 重置标志
    reset_requested = false;
    restart_requested = false;
    reset_task_running = false;

    // 重启设备
    esp_restart();
}

// 按键定时器回调函数
static void key_timer_callback(void *arg) {
    static int press_time = 0;

    if (gpio_get_level(KEY) == 0) {  // 按键按下
        if (KeyState == 0) {
            KeyState = 1;
            press_time = 0;
        } else {
            press_time++;

            if (press_time >= 500) {  // 长按超过5秒 (10ms×500=5000ms)
                if (!reset_task_running) {
                    printf("RESET ALL!\n");
                    // 设置复位标志
                    reset_requested = true;
                    press_time = 0;  // 防止重复触发

                    // 创建任务来处理重置（在任务中会闪烁LED）
                    BaseType_t task_created = xTaskCreate(reset_restart_task, "reset_task", 2048, NULL, 5, NULL);
                    if (task_created == pdPASS) {
                        reset_task_running = true;
                    } else {
                        reset_requested = false;
                        printf("创建reset_task失败: %ld\n", (long)task_created);
                    }
                }
            }
        }
    } else {
        if (KeyState == 1) {
            // 如果reset任务正在执行，不处理按键释放事件，避免干扰LED显示
            if (reset_task_running) {
                printf("Reset任务执行中，忽略按键释放\n");
                return;
            }
            
            if (press_time > 0 && press_time < 100) {  // 按下1秒以内释放 (10ms×100=1000ms)
                printf("RESTART!\n");
                // 创建任务来处理重启
                if (!reset_task_running) {
                    restart_requested = true;
                    BaseType_t task_created = xTaskCreate(reset_restart_task, "restart_task", 2048, NULL, 5, NULL);
                    if (task_created == pdPASS) {
                        reset_task_running = true;
                    } else {
                        restart_requested = false;
                        printf("创建restart_task失败: %ld\n", (long)task_created);
                    }
                }
            } else if (press_time >= 100 && press_time < 500) {  // 按下1-5秒释放
                printf("按键释放，未执行任何操作\n");
                // 1-5秒释放不做任何处理，不改变LED状态
            }
            KeyState = 0;
            press_time = 0;
        }
    }
}



// 内存监控定时器回调函数
void free_timer_callback(void *arg) {
    printf("=================================================\r\n");
    printf("\r\nremaining memory = %zu,minimum memory = %zu\r\n",
           xPortGetFreeHeapSize(), xPortGetMinimumEverFreeHeapSize());

    // 获取自系统启动以来的时间（单位：微秒）
    int64_t time_since_boot = esp_timer_get_time();
    // 转换为天、小时、分钟
    int days = time_since_boot / (1000000LL * 60 * 60 * 24);
    int hours = (time_since_boot % (1000000LL * 60 * 60 * 24)) / (1000000LL * 60 * 60);
    int minutes = (time_since_boot % (1000000LL * 60 * 60)) / (1000000LL * 60);

    update_uptime_string();
    if (days > 0) {
        printf("Time since boot: %d days, %d hours, %d minutes\n", days, hours, minutes);
    } else if (hours > 0) {
        printf("Time since boot: %d hours, %d minutes\n", hours, minutes);
    } else {
        printf("Time since boot: %d minutes\n", minutes);
    }

    esp_reset_reason_t reason = esp_reset_reason();
    printf("Reset reason: %s\n", reset_reason_to_string(reason));

    // APSTA要求AP保持开启，不再根据运行时间自动切换到STA模式。

    printf("=================================================\r\n");
}

esp_err_t _timer_http_event_handler(esp_http_client_event_t *evt) {
    static char *timer_output_buffer = NULL; // 静态缓冲区，避免与其他函数的静态变量冲突
    static int timer_output_len = 0;         // 静态长度，避免与其他函数的静态变量冲突
    static bool disconnected = false;        // 标记是否已经收到过断开连接事件

    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
        // 确保在错误事件中释放资源，但不释放用户提供的缓冲区
        if (timer_output_buffer != NULL && evt->user_data != timer_output_buffer) {
            free(timer_output_buffer);
            timer_output_buffer = NULL;
            timer_output_len = 0;
        }
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        // 每次新连接时重置断开标志
        disconnected = false;
        // 连接时清理旧缓冲区，确保每次请求都使用新的缓冲区
        // 但不释放用户提供的缓冲区
        if (timer_output_buffer != NULL && evt->user_data != timer_output_buffer) {
            free(timer_output_buffer);
            timer_output_buffer = NULL;
            timer_output_len = 0;
        }
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (evt->data_len > 0) {
            // 如果用户数据直接可用（传递了一个预分配的缓冲区）
            if (evt->user_data) {
                // 确保不会溢出用户缓冲区
                int copy_len = MIN(evt->data_len, MAX_HTTP_OUTPUT_BUFFER - timer_output_len - 1);
                if (copy_len > 0) {
                    memcpy((char*)evt->user_data + timer_output_len, evt->data, copy_len);
                    timer_output_len += copy_len;
                    // 确保字符串以NULL结尾
                    ((char*)evt->user_data)[timer_output_len] = '\0';
                }
            } else {
                // 动态分配内存的情况
                int content_length = esp_http_client_get_content_length(evt->client);
                if (content_length <= 0) {
                    content_length = MAX_HTTP_OUTPUT_BUFFER; // 默认大小
                }

                // 第一次接收数据时分配内存
                if (timer_output_buffer == NULL) {
                    timer_output_buffer = malloc(content_length + 1); // +1 用于NULL终止符
                    if (timer_output_buffer == NULL) {
                        ESP_LOGE(TAG, "无法为HTTP响应分配内存");
                        return ESP_FAIL;
                    }
                    memset(timer_output_buffer, 0, content_length + 1); // 初始化内存
                    timer_output_len = 0;
                }

                // 确保不会溢出缓冲区
                if (timer_output_len + evt->data_len <= content_length) {
                    memcpy(timer_output_buffer + timer_output_len, evt->data, evt->data_len);
                    timer_output_len += evt->data_len;
                    timer_output_buffer[timer_output_len] = '\0'; // 确保字符串以NULL结尾
                } else {
                    ESP_LOGW(TAG, "数据长度超过预分配缓冲区大小，截断数据");
                    int copy_len = content_length - timer_output_len;
                    if (copy_len > 0) {
                        memcpy(timer_output_buffer + timer_output_len, evt->data, copy_len);
                        timer_output_len += copy_len;
                        timer_output_buffer[timer_output_len] = '\0';
                    }
                }
            }

            // 输出收到的数据用于调试
            ESP_LOGI(TAG, "收到的数据: %.*s", evt->data_len, (char*)evt->data);
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
        // 重置输出长度，但不立即释放缓冲区，等待断开连接时再释放
        timer_output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        // 防止重复处理断开事件
        if (disconnected) {
            ESP_LOGW(TAG, "忽略重复的断开连接事件");
            return ESP_OK;
        }

        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        disconnected = true; // 标记已收到断开事件

        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
        if (err != 0) {
            ESP_LOGE(TAG, "TLS错误: ESP错误码=0x%x, mbedTLS错误码=0x%x", err, mbedtls_err);
            ESP_LOGE(TAG, "TLS错误详情：%s", esp_err_to_name(err));
        }

        // 在释放缓冲区之前等待较长时间，确保TCP处理完成
        vTaskDelay(100 / portTICK_PERIOD_MS);

        // 确保安全释放缓冲区
        if (timer_output_buffer != NULL) {
            // 检查是否安全释放内存
            bool should_free = true;

            // 只有在buffer不是用户数据时才释放
            if (evt->user_data == timer_output_buffer) {
                ESP_LOGI(TAG, "跳过释放用户提供的缓冲区");
                should_free = false;
            }

            if (should_free) {
                ESP_LOGI(TAG, "释放动态分配的输出缓冲区");
                free(timer_output_buffer);
            }

            timer_output_buffer = NULL;
            timer_output_len = 0;
        }
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
        // 处理重定向事件
        esp_http_client_set_redirection(evt->client);
        break;
    }
    return ESP_OK;
}
