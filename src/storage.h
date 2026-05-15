#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct storage_status {
	bool attempted;
	bool mounted;
	int mount_ret;
	unsigned long total_kib;
	unsigned long free_kib;
};

int storage_mount_once(void);
void storage_get_status(struct storage_status *status);
int storage_format_status_json(char *response, size_t response_len);
int storage_read_user_index(char *buf, size_t buf_len, size_t *len);
int storage_write_user_index(const uint8_t *data, size_t len);

#endif
