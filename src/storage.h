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

typedef int (*storage_read_cb_t)(void *ctx, uint8_t *buf, size_t max_len,
				 size_t *len);
typedef int (*storage_write_cb_t)(void *ctx, const uint8_t *buf, size_t len);

int storage_mount_once(void);
void storage_get_status(struct storage_status *status);
int storage_format_status_json(char *response, size_t response_len);
int storage_user_index_size(size_t *len);
int storage_stream_user_index(storage_write_cb_t writer, void *ctx);
int storage_write_user_index_stream(size_t len, storage_read_cb_t reader,
				    void *ctx);

#endif
