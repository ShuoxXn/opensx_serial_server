/*
 * @Author: Orion
 * @Date: 2024-12-20 15:40:52
 * @LastEditors: Orion
 * @LastEditTime: 2025-01-17 15:20:13
 * @Description:
 *
 */
#include "sx_log.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "sx_web_server.h"
#define LOG_BUFFER_SIZE 1024
#define SIMPLE_LOG_BUFFER_SIZE 1024

// 日志缓冲区和位置指针
static char log_buffer[LOG_BUFFER_SIZE];
static char simple_log_buffer[SIMPLE_LOG_BUFFER_SIZE];
static size_t log_position = 0;
static size_t simple_log_position = 0;

// 互斥锁
static SemaphoreHandle_t log_mutex = NULL;
static SemaphoreHandle_t simple_log_mutex = NULL;
#define LOG_MSG_SIZE 256
static char static_log_buffer[LOG_MSG_SIZE];  // 静态缓冲区用于WebSocket消息
// 系统日志输出函数 - 负责标准串口输出和详细日志记录
static int log_output_func(const char* fmt, va_list args) {
    // 检查互斥锁是否已初始化
    if (log_mutex == NULL) {
        // 如果互斥锁未初始化，直接输出到标准串口并返回
        return vprintf(fmt, args);
    }

    // 尝试获取互斥锁，使用较短的超时时间
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        // 如果无法获取互斥锁，直接输出到标准串口并返回
        // 这样可以避免在互斥锁获取失败时的断言错误
        return vprintf(fmt, args);
    }

    int written = 0;

    // 使用try-finally模式确保互斥锁总是被释放
    do {
        // 首先输出到标准串口
        written = vprintf(fmt, args);

        // 准备WebSocket消息
        va_list args_copy;
        va_copy(args_copy, args);
        int len = vsnprintf(static_log_buffer, LOG_MSG_SIZE, fmt, args_copy);
        va_end(args_copy);

        if (len <= 0 || len >= LOG_MSG_SIZE) {
            // 如果格式化失败或缓冲区不足，跳过后续处理
            break;
        }

        // 确保字符串以换行符结束
        if (static_log_buffer[len-1] != '\n') {
            if (len < LOG_MSG_SIZE-1) {
                static_log_buffer[len] = '\n';
                static_log_buffer[len+1] = '\0';
                len++;
            }
        }

        // 发送到WebSocket - 使用try-catch模式避免异常导致互斥锁未释放
        // 注意：这里不使用ESP_LOGD，因为它会递归调用log_output_func
        // 直接使用printf代替，避免递归调用
        // printf("Sending log to WebSocket: %s", static_log_buffer);

        // 注意：如果send_log_to_websocket可能失败，应该在这里添加错误处理
        // 尝试发送到WebSocket，但不要让它影响主要的日志功能
        send_log_to_websocket(static_log_buffer);

        // 写入到详细日志缓冲区
        if (log_position + len >= LOG_BUFFER_SIZE) {
            // 如果缓冲区将要溢出，移除最早的日志
            size_t move_size = LOG_BUFFER_SIZE - len;
            memmove(log_buffer,
                   log_buffer + (log_position + len - LOG_BUFFER_SIZE),
                   move_size);
            log_position = move_size;
        }

        // 添加新日志到缓冲区
        memcpy(log_buffer + log_position, static_log_buffer, len);
        log_position += len;
        log_buffer[log_position] = '\0';
    } while (0); // 这个do-while(0)结构允许使用break跳出

    // 确保在所有情况下都释放互斥锁
    xSemaphoreGive(log_mutex);
    return written;
}

// 初始化日志系统
void sx_log_init(void) {
    if (log_mutex == NULL) {
        log_mutex = xSemaphoreCreateMutex();
    }
    if (simple_log_mutex == NULL) {
        simple_log_mutex = xSemaphoreCreateMutex();
    }

    // 清空缓冲区
    memset(log_buffer, 0, LOG_BUFFER_SIZE);
    memset(simple_log_buffer, 0, SIMPLE_LOG_BUFFER_SIZE);
    log_position = 0;
    simple_log_position = 0;

    // 设置系统日志输出函数
    esp_log_set_vprintf(log_output_func);
}

// 获取详细日志缓冲区内容
char* sx_log_get_buffer(void) {
    if (log_mutex == NULL) return NULL;

    char* result = NULL;
    if (xSemaphoreTake(log_mutex, portMAX_DELAY) == pdTRUE) {
        if (log_position > 0) {
            result = strdup(log_buffer);
        }
        xSemaphoreGive(log_mutex);
    }
    return result;
}

// 获取简明日志缓冲区内容
char* sx_log_get_simple_buffer(void) {
    if (simple_log_mutex == NULL) return NULL;

    char* result = NULL;
    if (xSemaphoreTake(simple_log_mutex, portMAX_DELAY) == pdTRUE) {
        if (simple_log_position > 0) {
            result = strdup(simple_log_buffer);
        }
        xSemaphoreGive(simple_log_mutex);
    }
    return result;
}

// 清除所有日志
void sx_log_clear(void) {
    // 清除详细日志
    if (log_mutex) {
        if (xSemaphoreTake(log_mutex, portMAX_DELAY) == pdTRUE) {
            memset(log_buffer, 0, LOG_BUFFER_SIZE);
            log_position = 0;
            xSemaphoreGive(log_mutex);
        }
    }

    // 清除简明日志
    if (simple_log_mutex) {
        if (xSemaphoreTake(simple_log_mutex, portMAX_DELAY) == pdTRUE) {
            memset(simple_log_buffer, 0, SIMPLE_LOG_BUFFER_SIZE);
            simple_log_position = 0;
            xSemaphoreGive(simple_log_mutex);
        }
    }
}

// 写入详细日志
void sx_log_write(const char* format, ...) {
    va_list args;
    va_start(args, format);

    // 直接使用系统日志输出函数
    log_output_func(format, args);

    va_end(args);
}

void sx_log_write_simple(const char* format, ...) {
    if (simple_log_mutex == NULL) return;

    // 获取互斥锁
    if (xSemaphoreTake(simple_log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    // 使用静态缓冲区
    static char temp_buffer[256];
    static char ws_buffer[384];  // 更大的缓冲区用于添加[SIMPLE]标记
    va_list args;
    va_start(args, format);

    // 格式化日志消息
    int len = vsnprintf(temp_buffer, sizeof(temp_buffer), format, args);
    va_end(args);

    if (len > 0 && len < sizeof(temp_buffer)) {
        // 确保字符串以换行符结束
        if (temp_buffer[len-1] != '\n') {
            if (len < sizeof(temp_buffer)-1) {
                temp_buffer[len] = '\n';
                temp_buffer[len+1] = '\0';
                len++;
            }
        }

        // 为WebSocket添加[SIMPLE]标记
        snprintf(ws_buffer, sizeof(ws_buffer), "[SIMPLE] %s", temp_buffer);
        send_log_to_websocket(ws_buffer);

        // 处理简明日志缓冲区
        if (simple_log_position + len >= SIMPLE_LOG_BUFFER_SIZE) {
            // 查找第一个换行符的位置
            char *first_newline = strchr(simple_log_buffer, '\n');
            if (first_newline != NULL) {
                size_t remove_len = first_newline - simple_log_buffer + 1;
                memmove(simple_log_buffer,
                       first_newline + 1,
                       simple_log_position - remove_len);
                simple_log_position -= remove_len;
            } else {
                simple_log_position = 0;
            }
        }

        // 添加到简明日志缓冲区
        if (simple_log_position + len < SIMPLE_LOG_BUFFER_SIZE) {
            memcpy(simple_log_buffer + simple_log_position, temp_buffer, len);
            simple_log_position += len;
            simple_log_buffer[simple_log_position] = '\0';
        }

        // 输出到标准串口
        printf("%s", temp_buffer);
    }

    xSemaphoreGive(simple_log_mutex);
}
