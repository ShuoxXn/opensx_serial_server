#ifndef SX_LOG_H
#define SX_LOG_H

#include <stddef.h>
#include <esp_err.h>

// 初始化日志系统
void sx_log_init(void);

// 获取详细日志内容
char* sx_log_get_buffer(void);

// 获取简明日志内容
char* sx_log_get_simple_buffer(void);

// 清除所有日志
void sx_log_clear(void);

// 写入详细日志
void sx_log_write(const char* format, ...);

// 写入简明日志（通信相关）
void sx_log_write_simple(const char* format, ...);

#endif // SX_LOG_H
