/*
 * @Author: Orion
 * @Date: 2024-01-11 11:20:48
 * @LastEditors: Orion
 * @LastEditTime: 2025-03-13 17:25:57
 * @FilePath: \SERIIAL_SERVER\main\include\sx_tcp_client.h
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#pragma once

/*
 * Internal TCP Client module.
 *
 * Work modes should prefer sx_protocol_api.h instead of calling this module
 * directly. This header is kept for framework internals and backward
 * compatibility.
 */

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
void start_tcp_client(void);
void stop_tcp_client(void);
void tcpclient_message(char *message,int rxBytes);
extern int global_client_sock;
extern int global_tcp_server;
#ifdef __cplusplus
}
#endif
