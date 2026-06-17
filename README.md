# 开发指南

本文档说明如何在当前固件框架中编写一个新的工作任务。
当前项目的目标是把通用固件能力和具体业务逻辑分开，以便于二次开发：

- `drivers/`：设备驱动，例如 UART、GPIO、Modbus 底层辅助能力。
- `protocols/`：网络协议收发能力，例如基于 BSD socket/lwIP 的 TCP Server、TCP Client、HTTP、MQTT 等。
- `system/`：通用系统能力，例如 NVS、日志、定时任务、工作模式注册表。
- `web/` 和 `static/`：Web 配置界面和 HTTP API。
- `modes/`：具体工作任务。业务逻辑只应该放在这里。

当前内置工作模式：

| 模式名 | 文件 | 用途 |
| --- | --- | --- |
| `tcp_transparent` | `main/modes/mode_tcp_transparent.c` | 纯 TCP 透传示例模式，用于演示如何实现业务逻辑 |
| `protocol_test` | `main/modes/mode_protocol_test.c` | MQTT/TCP 协议回显测试模式，并每 1 秒通过 HTTP 协议配置 POST `test`，用于测试固件网络协议功能 |


## 工作模式接口

工作模式由 `main/include/sx_work_mode.h` 定义：

```c
typedef void (*sx_work_mode_uart_handler_t)(const uint8_t *data, size_t len);
typedef esp_err_t (*sx_work_mode_tcp_handler_t)(const uint8_t *data,
                                                size_t len);
typedef esp_err_t (*sx_work_mode_mqtt_handler_t)(const char *topic,
                                                 const uint8_t *data,
                                                 size_t len);
typedef esp_err_t (*sx_work_mode_lifecycle_fn_t)(void);
typedef esp_err_t (*sx_work_mode_config_load_fn_t)(nvs_handle_t handle,
                                                   cJSON *response);
typedef esp_err_t (*sx_work_mode_config_save_fn_t)(nvs_handle_t handle,
                                                   const cJSON *request);

typedef struct {
  sx_work_mode_config_load_fn_t load;
  sx_work_mode_config_save_fn_t save;
} sx_work_mode_config_ops_t;

typedef struct {
  const char *name;
  const char *label;
  sx_work_mode_lifecycle_fn_t start;
  sx_work_mode_lifecycle_fn_t stop;
  sx_work_mode_lifecycle_fn_t after_start;
  sx_work_mode_uart_handler_t on_uart_data;
  sx_work_mode_tcp_handler_t on_tcp_data;
  sx_work_mode_mqtt_handler_t on_mqtt_data;
  const sx_work_mode_config_ops_t *config;
} sx_work_mode_t;
```

字段含义：

| 字段 | 说明 |
| --- | --- |
| `name` | 工作模式名称，保存到 NVS 的 `w_mode` 字段中 |
| `label` | Web/API 展示名称；不影响内部模式名 |
| `start` | 启动模式时调用，通常创建任务、队列、状态机 |
| `stop` | 停止模式时调用，释放任务、队列、内存等资源 |
| `after_start` | 可选钩子，适合启动后追加初始化 |
| `on_uart_data` | UART 收到数据后的回调 |
| `on_tcp_data` | TCP Server/Client 收到数据后的回调；为空时保持默认写 UART 行为 |
| `on_mqtt_data` | MQTT 收到业务数据后的回调；为空时保持默认写 UART 行为 |
| `config` | 可选配置回调；用于 `/work_mode_set` 保存和 `/work_mode_info` 加载模式私有配置 |

系统提供的注册和调度接口：

```c
esp_err_t sx_work_mode_register(const sx_work_mode_t *mode);
esp_err_t sx_work_mode_register_builtin_modes(void);
esp_err_t sx_work_mode_start_by_name(const char *name);
esp_err_t sx_work_mode_stop_current(void);
sx_work_mode_uart_handler_t sx_work_mode_get_uart_handler(void);
sx_work_mode_tcp_handler_t sx_work_mode_get_tcp_handler(void);
sx_work_mode_mqtt_handler_t sx_work_mode_get_mqtt_handler(void);
bool sx_work_mode_is_valid(const char *name);
const sx_work_mode_t *sx_work_mode_get_current(void);
const sx_work_mode_t *sx_work_mode_find_by_name(const char *name);
esp_err_t sx_work_mode_load_config(const char *name, nvs_handle_t handle,
                                   cJSON *response);
esp_err_t sx_work_mode_save_config(const char *name, nvs_handle_t handle,
                                   const cJSON *request);
esp_err_t sx_work_mode_append_available_modes(cJSON *array);
```

调用链如下：

1. `app_main()` 读取 NVS 中的 `w_mode`。
2. `sx_work_mode_start_by_name(work_mode)` 根据名称启动模式。
3. `sx_async_uart.c` 收到串口数据。
4. UART 驱动通过 `sx_work_mode_get_uart_handler()` 找到当前模式的 `on_uart_data`。
5. 当前模式决定如何处理数据，例如转发到 TCP、写入缓存、触发业务状态机。

## 工作模式配置接口

Web 与后端传递工作模式配置时使用统一结构：

```json
{
  "work_mode": "tcp_transparent",
  "mode_config": {}
}
```

后端处理流程：

1. `/work_mode_set` 校验 `work_mode` 是否已注册。
2. `nvs_set_work_mode()` 保存 `w_mode`。
3. `sx_work_mode_save_config(work_mode, handle, request)` 调用当前模式的 `config->save`。
4. `nvs_commit()` 统一提交。

配置读取流程：

1. `/work_mode_info` 读取 `w_mode`。
2. `sx_work_mode_append_available_modes()` 输出可选模式列表。
3. `sx_work_mode_load_config(work_mode, handle, response)` 调用当前模式的 `config->load`。
4. 返回值中包含 `mode_config`。

新增模式的私有配置应优先放在 `mode_config` 中，由该模式自己的 `save/load` 回调负责解析和保存。Web 的 `/work_mode_set` 和 MQTT 配置下发都会走 `sx_work_mode_save_config()`，读取则由 `/work_mode_info` 走 `sx_work_mode_load_config()`。框架公共配置，例如 TCP、MQTT、HTTP、UART、网络参数，继续使用原有框架配置接口，不要塞进模式私有配置。

## 推荐协议 API

工作模式代码位于 `main/modes/`。模式侧调用 TCP、MQTT、HTTP、UDP 时，推荐统一使用：

```c
#include "sx_protocol_api.h"
```

主要接口：

```c
esp_err_t sx_protocol_tcp_send(sx_protocol_tcp_target_t target,
                               const uint8_t *data, size_t len);
esp_err_t sx_protocol_tcp_server_send(const uint8_t *data, size_t len);
esp_err_t sx_protocol_tcp_client_send(const uint8_t *data, size_t len);
esp_err_t sx_protocol_mqtt_publish(const char *topic, const uint8_t *data,
                                   size_t len, int qos, bool retain);
esp_err_t sx_protocol_http_post_configured(const uint8_t *data, size_t len,
                                           const char *content_type);
esp_err_t sx_protocol_udp_send_to(const char *host, uint16_t port,
                                  const uint8_t *data, size_t len);
```

调用前条件：

| API | 调用前条件 | 常见失败 |
| --- | --- | --- |
| `sx_protocol_tcp_send()` | 协议管理器已按全局 TCP 配置启动 TCP Server 或 TCP Client | TCP Client 未连接，TCP Server 无客户端，参数为空 |
| `sx_protocol_mqtt_publish()` | MQTT Client 已启动并连接 | MQTT 未初始化，topic 为空，网络断开 |
| `sx_protocol_http_post_configured()` | `use_http=1`，`http_url` 非空，网络可访问目标 URL | HTTP 未启用，URL 为空，请求超时或失败 |
| `sx_protocol_udp_send_to()` | 网络已获取 IP，目标 host 可解析，端口非 0 | host 无效，DNS 失败，sendto 失败 |

TCP 按当前全局配置自动选择 TCP Server 或 TCP Client：

```c
esp_err_t err =
    sx_protocol_tcp_send(SX_PROTOCOL_TCP_AUTO, data, data_len);
```

MQTT 发布到指定 topic：

```c
esp_err_t err = sx_protocol_mqtt_publish(topic, data, data_len, 0, false);
```

HTTP 使用全局 `http_url` 执行 POST：

```c
esp_err_t err =
    sx_protocol_http_post_configured(data, data_len, "application/json");
```

UDP 主动发送需要显式指定目标：

```c
esp_err_t err =
    sx_protocol_udp_send_to("192.168.1.100", 6000, data, data_len);
```

当前框架采用全局配置和全局协议调度设计。工作模式不负责调用这些内部生命周期函数：

```c
start_tcp_server();
start_tcp_client();
mqtt_app_start();
start_udp_server();
```

以下头文件属于框架内部模块或历史兼容接口，工作模式不建议直接调用：

| 头文件 | 推荐替代 |
| --- | --- |
| `sx_tcp_server.h` | 使用 `sx_protocol_tcp_send()` 或 `sx_protocol_tcp_server_send()` |
| `sx_tcp_client.h` | 使用 `sx_protocol_tcp_send()` 或 `sx_protocol_tcp_client_send()` |
| `sx_mqtt_client.h` | 使用 `sx_protocol_mqtt_publish()` |
| `sx_http_client.h` | 使用 `sx_protocol_http_post_configured()` |
| `sx_udp_server.h` | 使用 `sx_protocol_udp_send_to()` 发送 UDP |

常见返回值：

| 返回值 | 含义 |
| --- | --- |
| `ESP_OK` | 调用成功 |
| `ESP_ERR_INVALID_ARG` | 参数为空、长度为 0 或长度过大 |
| `ESP_ERR_INVALID_STATE` | 协议未启用、配置缺失或当前状态不可发送 |
| `ESP_ERR_NOT_FOUND` | 目标地址解析失败 |
| `ESP_FAIL` | 底层发送失败 |

当前 `protocol_test` 工作模式使用 TCP/MQTT 输入回调实现协议回显，并在模式启动后每 1 秒调用 `sx_protocol_http_post_configured()` 向全局 `http_url` POST 文本 `test`。这不是 HTTP 接收回显，也不使用 Web Server 作为测试入口。

## mode_config 保存和读取示例

`mode_config` 是工作模式的私有配置对象。框架只负责把请求交给当前模式的 `config->save`，以及在读取时调用当前模式的 `config->load`。具体字段、校验和 NVS key 由工作模式自己负责。

保存工作模式和私有配置的请求示例：

```json
{
  "work_mode": "protocol_report",
  "mode_config": {
    "use_mqtt": true,
    "mqtt_topic": "/device/data",
    "use_http": false,
    "use_udp": true,
    "udp_host": "192.168.1.100",
    "udp_port": 6000
  }
}
```

模式私有字段建议带模式名前缀，避免污染框架公共配置：

| JSON 字段 | NVS key 示例 |
| --- | --- |
| `use_mqtt` | `protocol_report_use_mqtt` |
| `mqtt_topic` | `protocol_report_mqtt_topic` |
| `use_http` | `protocol_report_use_http` |
| `use_udp` | `protocol_report_use_udp` |
| `udp_host` | `protocol_report_udp_host` |
| `udp_port` | `protocol_report_udp_port` |

不要把模式私有字段保存成 `tcp_server`、`mqtt_server`、`http_url` 这类框架公共 key。公共 key 仍由网络、协议和 Web 公共配置页面维护。

模式的 `save` 回调应只保存自己关心的字段。缺失字段可以保持原值，字段类型错误应返回 `ESP_ERR_INVALID_ARG`。

```c
esp_err_t mode_xxx_save_config(nvs_handle_t handle, const cJSON *request) {
  if (request == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  const cJSON *mode_config = cJSON_GetObjectItem(request, "mode_config");
  if (mode_config == NULL) {
    return ESP_OK;
  }
  if (!cJSON_IsObject(mode_config)) {
    return ESP_ERR_INVALID_ARG;
  }

  const cJSON *use_mqtt = cJSON_GetObjectItem(mode_config, "use_mqtt");
  if (use_mqtt != NULL) {
    if (!cJSON_IsBool(use_mqtt)) {
      return ESP_ERR_INVALID_ARG;
    }
    ESP_ERROR_CHECK(nvs_set_str(handle, "protocol_report_use_mqtt",
                                cJSON_IsTrue(use_mqtt) ? "1" : "0"));
  }

  const cJSON *mqtt_topic = cJSON_GetObjectItem(mode_config, "mqtt_topic");
  if (mqtt_topic != NULL) {
    if (!cJSON_IsString(mqtt_topic) || mqtt_topic->valuestring == NULL) {
      return ESP_ERR_INVALID_ARG;
    }
    ESP_ERROR_CHECK(nvs_set_str(handle, "protocol_report_mqtt_topic",
                                mqtt_topic->valuestring));
  }

  return ESP_OK;
}
```

模式的 `load` 回调负责创建并填充 `mode_config`：

```c
esp_err_t mode_xxx_load_config(nvs_handle_t handle, cJSON *response) {
  if (response == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  char mqtt_topic[128] = "/device/data";
  size_t len = sizeof(mqtt_topic);
  nvs_get_str(handle, "protocol_report_mqtt_topic", mqtt_topic, &len);

  cJSON *mode_config = cJSON_CreateObject();
  if (mode_config == NULL) {
    return ESP_ERR_NO_MEM;
  }

  cJSON_AddBoolToObject(mode_config, "use_mqtt", false);
  cJSON_AddStringToObject(mode_config, "mqtt_topic", mqtt_topic);
  cJSON_AddItemToObject(response, "mode_config", mode_config);
  return ESP_OK;
}
```

前端收集表单后只需要把模式私有字段放进 `mode_config`：

```js
const payload = {
  work_mode: "protocol_report",
  mode_config: {
    use_mqtt: document.querySelector("#useMqtt").checked,
    mqtt_topic: document.querySelector("#mqttTopic").value,
    use_http: document.querySelector("#useHttp").checked,
    use_udp: document.querySelector("#useUdp").checked,
    udp_host: document.querySelector("#udpHost").value,
    udp_port: Number(document.querySelector("#udpPort").value)
  }
};

await fetch("/work_mode_set", {
  method: "POST",
  headers: {"Content-Type": "application/json"},
  body: JSON.stringify(payload)
});
```

读取时使用 `/work_mode_info`：

```js
const info = await fetch("/work_mode_info").then((res) => res.json());
const config = info.mode_config || {};
```

当前框架推荐保存配置后重启生效。这样可以避免运行时切换工作模式时遗留任务、队列、socket 或 MQTT 状态。如果后续要支持运行时切换，必须显式调用 `sx_work_mode_stop_current()` 和 `sx_work_mode_start_by_name(new_mode)`，并确认当前模式的 `stop` 已释放所有任务、队列、定时器和动态内存。

## 当前 TCP 透传示例

`tcp_transparent` 的模式名定义在 `main/include/sx_work_mode_ids.h`：

```c
#define WORK_MODE_TCP_TRANSPARENT "tcp_transparent"
#define WORK_MODE_DEFAULT WORK_MODE_TCP_TRANSPARENT
```

它注册在 `main/modes/sx_builtin_modes.c`：

```c
static const sx_work_mode_t tcp_transparent_mode = {
    .name = WORK_MODE_TCP_TRANSPARENT,
    .label = "TCP透传",
    .start = mode_tcp_transparent_start,
    .stop = mode_tcp_transparent_stop,
    .after_start = NULL,
    .on_uart_data = mode_tcp_transparent_on_uart_data,
    .config = &tcp_transparent_config,
};
```

它的核心行为：

- UART 收到数据后进入模式队列。
- 模式任务从队列取出数据。
- 根据 TCP 配置选择 TCP Server 或 TCP Client。
- 调用协议 API 发送数据。
- 不调用 MQTT。
- 不处理 Modbus，但不代表框架删除了 Modbus 能力。
- 通过 `mode_tcp_transparent_load_config()` 和 `mode_tcp_transparent_save_config()` 接入统一配置框架。

发送 TCP 数据应通过 `main/include/sx_protocol_api.h`：

```c
esp_err_t err = sx_protocol_tcp_send(SX_PROTOCOL_TCP_AUTO, data, data_len);
```

旧的 `sx_protocol_send_tcp_server()` 和 `sx_protocol_send_tcp_client()` 仍然保留用于兼容，新增工作模式应优先使用本文“推荐协议 API”中的接口。

协议层收到 TCP 数据后，当前 TCP Server/Client 会直接调用 UART 发送函数，将数据写回串口。

## 新增工作模式

新增一个模式时，可以从空文件创建，也可以直接复制 `docs/modes/` 中的示例框架：

```text
docs/modes/mode_protocol_report_example.c
docs/modes/mode_protocol_report_example.h
```

复制到：

```text
main/modes/
```

`docs/modes/` 只提供可复制的模式代码模板，不会自动完成注册、构建入口或 Web 接入。无论从零创建还是复制示例框架，都需要完成下面这些改动：

| 文件 | 必改内容 |
| --- | --- |
| `main/modes/mode_my_task.c` | 模式实现、生命周期、业务逻辑、配置保存/读取 |
| `main/modes/mode_my_task.h` | 模式函数声明 |
| `main/include/sx_work_mode_ids.h` | 增加模式名常量 |
| `main/modes/sx_builtin_modes.c` | 注册工作模式 |
| `main/CMakeLists.txt` | 把新的模式 `.c` 加入 `SRCS` |
| `main/static/web.html` | 增加工作模式单选项；如果有私有配置，增加配置区 |
| `main/static/web.js` | 保存时提交 `work_mode` 和 `mode_config`；读取 `/work_mode_info` 后恢复 UI |
| `main/static/i18n/zh-CN.json` | 增加中文模式名称和说明 |
| `main/static/i18n/en-US.json` | 增加英文模式名称和说明 |

如果模式需要独立的大文件上传、分页读取或特殊控制接口，才需要额外修改 `main/web/sx_web_server.c`。普通模式选择和 `mode_config` 保存不需要新增后端接口。

推荐按下面顺序做。

1. 添加模式名。

在 `main/include/sx_work_mode_ids.h` 中增加常量：

```c
#define WORK_MODE_MY_TASK "my_task"
```

2. 新建模式头文件。

例如 `main/modes/mode_my_task.h`：

```c
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t mode_my_task_start(void);
esp_err_t mode_my_task_stop(void);
void mode_my_task_on_uart_data(const uint8_t *data, size_t len);
```

3. 新建模式实现文件。

例如 `main/modes/mode_my_task.c`：

```c
#include "mode_my_task.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MODE_MY_TASK";
static TaskHandle_t s_task = NULL;
static QueueHandle_t s_queue = NULL;

typedef struct {
  uint8_t *data;
  size_t len;
} my_task_msg_t;

void mode_my_task_on_uart_data(const uint8_t *data, size_t len) {
  if (s_queue == NULL || data == NULL || len == 0) {
    return;
  }

  uint8_t *copy = malloc(len);
  if (copy == NULL) {
    ESP_LOGE(TAG, "failed to allocate UART payload");
    return;
  }
  memcpy(copy, data, len);

  my_task_msg_t msg = {.data = copy, .len = len};
  if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
    free(copy);
  }
}

static void my_task(void *arg) {
  while (true) {
    my_task_msg_t msg = {0};
    if (xQueueReceive(s_queue, &msg, portMAX_DELAY) == pdTRUE) {
      /* 在这里编写业务逻辑 */
      free(msg.data);
    }
  }
}

esp_err_t mode_my_task_start(void) {
  if (s_task != NULL) {
    return ESP_OK;
  }

  s_queue = xQueueCreate(10, sizeof(my_task_msg_t));
  if (s_queue == NULL) {
    return ESP_FAIL;
  }

  if (xTaskCreate(my_task, "mode_my_task", 4096, NULL, 5, &s_task) != pdPASS) {
    vQueueDelete(s_queue);
    s_queue = NULL;
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t mode_my_task_stop(void) {
  if (s_task != NULL) {
    vTaskDelete(s_task);
    s_task = NULL;
  }

  if (s_queue != NULL) {
    my_task_msg_t msg = {0};
    while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
      free(msg.data);
    }
    vQueueDelete(s_queue);
    s_queue = NULL;
  }

  return ESP_OK;
}
```

4. 注册模式。

在 `main/modes/sx_builtin_modes.c` 中添加：

```c
static const sx_work_mode_t my_task_mode = {
    .name = WORK_MODE_MY_TASK,
    .start = mode_my_task_start,
    .stop = mode_my_task_stop,
    .after_start = NULL,
    .on_uart_data = mode_my_task_on_uart_data,
};

esp_err_t err = sx_work_mode_register(&my_task_mode);
if (err != ESP_OK) {
  return err;
}
```

5. 加入构建。

在 `main/CMakeLists.txt` 的工作模式区域加入：

```cmake
"modes/mode_my_task.c"
```

6. 暴露到配置界面。

新增工作模式需要同步修改 Web 入口，让用户可以选择和配置该模式：

- `main/static/web.html`
- `main/static/web.js`
- `main/static/i18n/zh-CN.json`
- `main/static/i18n/en-US.json`
- 必要时修改 `main/web/sx_web_server.c`

提交到后端的 `work_mode` 必须等于模式名，例如 `my_task`。

Web 层只负责配置显示、字段保存和状态展示。新增模式字段时，应先判断它是框架公共配置还是该模式私有配置，避免把业务字段继续写成全局公共字段。NVS 公共字段归属见 `docs/配置字段说明.md`。

## Web 接入细节

Web 层只负责配置、状态展示、调试和 OTA，不应该承载具体业务任务。具体业务逻辑应放在 `main/modes/`。

主要文件：

| 路径 | 说明 |
| --- | --- |
| `main/static/web.html` | 主配置页面 |
| `main/static/web.js` | 前端交互、表单提交、状态轮询、WebSocket |
| `main/static/web.css` | 前端样式 |
| `main/static/i18n/zh-CN.json` | 中文文案 |
| `main/static/i18n/en-US.json` | 英文文案 |
| `main/web/sx_web_server.c` | HTTP Server、REST API、WebSocket、静态资源输出 |
| `main/web/sx_http_ota.c` | HTTP OTA 升级 |

静态资源通过 `main/CMakeLists.txt` 的 `EMBED_FILES` 嵌入固件。修改 `main/static/` 后必须重新执行 `idf.py build`。

当前主页面按框架职责组织：

| 页面 | 前端视图 | 说明 |
| --- | --- | --- |
| 基本信息 | `infoView` | 设备名、网络状态、IP、AP 状态、外网状态 |
| 网络管理 | `netView` | Ethernet/Wi-Fi、DHCP、静态 IP、DNS |
| 协议管理 | `protocolView` | MQTT、TCP、HTTP 通用协议配置 |
| 工作模式 | `serialView` | 当前工作模式、串口参数、串口调试 |
| 系统管理 | `sysView` | 登录配置、AP 开放时长、OTA、重启、重置 |
| 日志管理 | `logView` | 运行日志、导出、日志开关 |

`protocolView` 是协议管理配置，不代表当前内置工作模式一定会使用所有协议。当前 `tcp_transparent` 只使用 TCP，`protocol_test` 用于测试 MQTT/TCP 回显，并会在 HTTP 启用后每 1 秒向配置的 HTTP URI POST `test`。

多数配置流是：

```text
web.html 表单
  -> web.js 收集并校验字段
  -> POST API
  -> sx_web_server.c handler
  -> NVS namespace: nvs_namespace
  -> 重启或运行时刷新生效
```

工作模式选择使用独立字段：

| 字段 | 说明 |
| --- | --- |
| `w_mode` | 当前工作模式，默认 `tcp_transparent` |

Web 与后端的工作模式配置传输使用统一 JSON：

```json
{
  "work_mode": "tcp_transparent",
  "mode_config": {}
}
```

`GET /work_mode_info` 返回：

```json
{
  "work_mode": "tcp_transparent",
  "available_modes": [
    {
      "name": "tcp_transparent",
      "label": "TCP透传"
    },
    {
      "name": "protocol_test",
      "label": "协议测试"
    }
  ],
  "mode_config": {}
}
```

新增工作模式时，Web 层通常需要同步：

1. `main/static/web.html`
   - 增加工作模式单选项或配置区，尽量只改工作模式区域。
2. `main/static/web.js`
   - 在 `collectWorkModePayload()` 中把模式私有字段放入 `mode_config`。
   - 在 `applyWorkModeInfo()` 或对应模式 UI 函数中读取 `response.mode_config` 并显示配置。
3. `main/static/i18n/*.json`
   - 增加中英文文案。
4. 模式 C 文件
   - 实现 `config->save` 和 `config->load`，负责校验、保存、加载自己的 `mode_config`。
5. `main/modes/sx_builtin_modes.c`
   - 注册模式名、显示名称、生命周期函数和配置回调。
6. `main/web/sx_web_server.c`
   - 一般不需要为新模式增加字段分支；只有需要独立 API、分页读取或大文件上传时再补充 handler。
7. `docs/接口说明.md`
   - 记录新增字段和取值。

WebSocket 用于串口调试页面发送/接收 UART 数据和日志实时推送。串口调试的 WebSocket 不应该实现业务协议，只负责调试链路。业务数据处理应进入当前工作模式的 `on_uart_data`。

修改 Web 后至少执行：

```bash
idf.py build
```

真机阶段再验证：

- 页面能正常加载。
- 配置保存后 NVS 正确。
- 重启后配置仍存在。
- 串口调试 WebSocket 可用。
- 工作模式选择、模式说明和模式私有配置显示正确。

## 模式开发原则

工作模式应该只组织业务流程，不应该重新实现通用能力：

- 串口收发使用 UART 驱动。
- TCP/MQTT/HTTP/UDP 发送优先使用 `sx_protocol_api.h` 中的模式侧推荐 API。
- 配置读写使用 NVS 工具函数。
- 模式之间不要直接相互调用。
- 模式启动时创建的任务、队列、定时器，必须在 `stop` 中释放。
- UART 回调中避免长时间阻塞，推荐把数据复制到队列后由模式任务处理。

协议模块采用全局配置和全局协议管理器调度。工作模式不应该直接调用 `start_tcp_server()`、`start_tcp_client()`、`mqtt_app_start()` 等内部生命周期函数，也不应该依赖 TCP/MQTT/HTTP 模块中的全局变量。推荐 API 和示例见本文“推荐协议 API”。

## 验证

新增或修改工作模式后，至少执行：

```bash
idf.py build
```

修改 Web 配置入口后，还需要手动验证：

- 页面能正确显示模式。
- 提交后 NVS 中 `w_mode` 正确。
- 重启后能启动对应模式。
- UART 输入能进入 `on_uart_data`。
- 模式停止时不会遗留任务、队列或内存。
