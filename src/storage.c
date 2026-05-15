#include "storage.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(storage, CONFIG_LOG_DEFAULT_LEVEL);

#define STORAGE_PATH "/lfs"
#define USER_DIR_PATH STORAGE_PATH "/user"
#define USER_INDEX_PATH STORAGE_PATH "/user/index.html"
#define STORAGE_PARTITION storage_partition

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);

static struct fs_mount_t storage_mount = {
	.type = FS_LITTLEFS,
	.fs_data = &storage,
	.storage_dev = (void *)PARTITION_ID(STORAGE_PARTITION),
	.mnt_point = STORAGE_PATH,
};

static K_MUTEX_DEFINE(storage_lock);
static bool mount_attempted;
static bool mounted;
static int mount_ret = -ENODEV;
static unsigned long total_kib;
static unsigned long free_kib;

static void update_space_locked(void)
{
	struct fs_statvfs stat;
	int ret;

	total_kib = 0;
	free_kib = 0;

	if (!mounted) {
		return;
	}

	ret = fs_statvfs(STORAGE_PATH, &stat);
	if (ret < 0) {
		LOG_WRN("LittleFS statvfs failed: %d", ret);
		return;
	}

	total_kib = (unsigned long)((uint64_t)stat.f_frsize * stat.f_blocks / 1024U);
	free_kib = (unsigned long)((uint64_t)stat.f_frsize * stat.f_bfree / 1024U);
}

int storage_mount_once(void)
{
	int ret;

	k_mutex_lock(&storage_lock, K_FOREVER);
	if (mount_attempted) {
		ret = mount_ret;
		k_mutex_unlock(&storage_lock);
		return ret;
	}

	mount_attempted = true;
	ret = fs_mount(&storage_mount);
	mount_ret = ret;
	mounted = (ret == 0);

	if (ret == 0) {
		update_space_locked();
		LOG_INF("LittleFS mounted at %s: %lu KiB total, %lu KiB free",
			STORAGE_PATH, total_kib, free_kib);
	} else {
		LOG_WRN("LittleFS mount failed: %d", ret);
	}

	k_mutex_unlock(&storage_lock);
	return ret;
}

void storage_get_status(struct storage_status *status)
{
	k_mutex_lock(&storage_lock, K_FOREVER);
	update_space_locked();
	status->attempted = mount_attempted;
	status->mounted = mounted;
	status->mount_ret = mount_ret;
	status->total_kib = total_kib;
	status->free_kib = free_kib;
	k_mutex_unlock(&storage_lock);
}

int storage_format_status_json(char *response, size_t response_len)
{
	struct storage_status status;

	(void)storage_mount_once();
	storage_get_status(&status);

	return snprintk(response, response_len,
			"{\"type\":\"fs.status\",\"ok\":%s,"
			"\"attempted\":%s,\"mounted\":%s,"
			"\"mountRet\":%d,\"path\":\"%s\","
			"\"totalKiB\":%lu,\"freeKiB\":%lu}",
			status.mounted ? "true" : "false",
			status.attempted ? "true" : "false",
			status.mounted ? "true" : "false",
			status.mount_ret, STORAGE_PATH,
			status.total_kib, status.free_kib);
}

int storage_read_user_index(char *buf, size_t buf_len, size_t *len)
{
	struct fs_file_t file;
	ssize_t read_len;
	int ret;

	if (buf_len == 0 || len == NULL) {
		return -EINVAL;
	}

	ret = storage_mount_once();
	if (ret < 0) {
		return ret;
	}

	fs_file_t_init(&file);
	ret = fs_open(&file, USER_INDEX_PATH, FS_O_READ);
	if (ret < 0) {
		return ret;
	}

	read_len = fs_read(&file, buf, buf_len);
	(void)fs_close(&file);
	if (read_len < 0) {
		return (int)read_len;
	}

	if ((size_t)read_len == buf_len) {
		return -EFBIG;
	}

	*len = (size_t)read_len;
	return 0;
}

int storage_write_user_index(const uint8_t *data, size_t len)
{
	struct fs_file_t file;
	struct fs_dirent entry;
	ssize_t written;
	int ret;

	if (data == NULL && len > 0) {
		return -EINVAL;
	}

	ret = storage_mount_once();
	if (ret < 0) {
		return ret;
	}

	ret = fs_stat(USER_DIR_PATH, &entry);
	if (ret == -ENOENT) {
		ret = fs_mkdir(USER_DIR_PATH);
		if (ret < 0) {
			return ret;
		}
	} else if (ret < 0) {
		return ret;
	} else if (entry.type != FS_DIR_ENTRY_DIR) {
		return -ENOTDIR;
	}

	fs_file_t_init(&file);
	ret = fs_open(&file, USER_INDEX_PATH,
		      FS_O_CREATE | FS_O_TRUNC | FS_O_WRITE);
	if (ret < 0) {
		return ret;
	}

	written = fs_write(&file, data, len);
	ret = fs_sync(&file);
	(void)fs_close(&file);
	if (written < 0) {
		return (int)written;
	}

	if (ret < 0) {
		return ret;
	}

	if ((size_t)written != len) {
		return -EIO;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);
	update_space_locked();
	k_mutex_unlock(&storage_lock);

	LOG_INF("Wrote %u bytes to %s", (unsigned int)len, USER_INDEX_PATH);
	return 0;
}
