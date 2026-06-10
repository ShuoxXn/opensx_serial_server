/*
 * @Author: Orion
 * @Date: 2024-01-18 14:06:11
 * @LastEditors: Orion
 * @LastEditTime: 2024-07-15 23:03:22
 * @FilePath: \SERIIAL_SERVER\main\include\sx_udp_server.h
 * @Description:
 *
 * Copyright (c) 2024 by SHUOXIN-IOT, All Rights Reserved.
 */

#pragma once

/*
 * Internal UDP Server module.
 *
 * Work modes should prefer sx_protocol_api.h for UDP sending. This header is
 * kept for framework internals and backward compatibility.
 */

#ifdef __cplusplus
extern "C" {
#endif
void start_udp_server(void);
void kill_udp_server(void);
#ifdef __cplusplus
}
#endif
