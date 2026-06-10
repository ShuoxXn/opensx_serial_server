#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "sx_network_manager.h"

void sx_protocol_manager_init(void);
void sx_protocol_manager_on_network_state(sx_net_state_t old_state,
                                          sx_net_state_t new_state);
void sx_protocol_manager_stop_all(void);

#ifdef __cplusplus
}
#endif
