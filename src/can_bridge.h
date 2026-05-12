#ifndef CAN_BRIDGE_H
#define CAN_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

int can_bridge_init(void);
int can_bridge_handle_ws_text(const uint8_t *payload, size_t payload_len,
			      char *response, size_t response_len);
int can_bridge_format_next_rx(char *response, size_t response_len);
int can_bridge_format_status(char *response, size_t response_len);

#endif
