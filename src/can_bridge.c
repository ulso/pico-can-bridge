#include "can_bridge.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/data/json.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(can_bridge, CONFIG_LOG_DEFAULT_LEVEL);

#define CAN_NODE DT_CHOSEN(zephyr_canbus)
#define CAN_TX_TIMEOUT K_MSEC(100)
#define CAN_WS_JSON_MAX 256
#define CAN_TX_QUEUE_LEN 8
#define CAN_RX_QUEUE_LEN 8
#define CAN_TX_STACK_SIZE 2048
#define CAN_TX_PRIORITY 8

enum can_json_field {
	CAN_JSON_TYPE,
	CAN_JSON_BUS,
	CAN_JSON_ID,
	CAN_JSON_EXT,
	CAN_JSON_RTR,
	CAN_JSON_DLC,
	CAN_JSON_DATA,
	CAN_JSON_BITRATE,
	CAN_JSON_MODE,
};

struct can_json_message {
	const char *type;
	int32_t bus;
	int32_t id;
	bool ext;
	bool rtr;
	int32_t dlc;
	int32_t data[CAN_MAX_DLC];
	size_t data_len;
	int32_t bitrate;
	const char *mode;
};

static const struct device *const can_dev = DEVICE_DT_GET(CAN_NODE);
static atomic_t can_ready;
static uint32_t current_bitrate = CONFIG_CAN_DEFAULT_BITRATE;
static can_mode_t current_mode = CAN_MODE_NORMAL;
K_MSGQ_DEFINE(can_tx_queue, sizeof(struct can_frame),
	      CAN_TX_QUEUE_LEN, sizeof(uint32_t));
K_MSGQ_DEFINE(can_rx_queue, sizeof(struct can_frame),
	      CAN_RX_QUEUE_LEN, sizeof(uint32_t));
static K_THREAD_STACK_DEFINE(can_tx_stack, CAN_TX_STACK_SIZE);
static struct k_thread can_tx_thread_data;
static K_MUTEX_DEFINE(can_lock);
static int can_rx_filter_id = -1;
static atomic_t can_rx_streaming;

static const struct json_obj_descr can_json_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct can_json_message, type, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct can_json_message, bus, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct can_json_message, id, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct can_json_message, ext, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM(struct can_json_message, rtr, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM(struct can_json_message, dlc, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_ARRAY(struct can_json_message, data, CAN_MAX_DLC,
			     data_len, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct can_json_message, bitrate, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct can_json_message, mode, JSON_TOK_STRING),
};

static int ws_reply(char *response, size_t response_len, const char *code,
		    const char *message, int err)
{
	return snprintk(response, response_len,
			"{\"type\":\"error\",\"ok\":false,\"code\":\"%s\","
			"\"err\":%d,\"message\":\"%s\"}",
			code, err, message);
}

static const char *can_state_name(enum can_state state)
{
	switch (state) {
	case CAN_STATE_ERROR_ACTIVE:
		return "error-active";
	case CAN_STATE_ERROR_WARNING:
		return "error-warning";
	case CAN_STATE_ERROR_PASSIVE:
		return "error-passive";
	case CAN_STATE_BUS_OFF:
		return "bus-off";
	case CAN_STATE_STOPPED:
		return "stopped";
	default:
		return "unknown";
	}
}

static const char *can_mode_name(can_mode_t mode)
{
	if ((mode & CAN_MODE_LOOPBACK) != 0) {
		return "loopback";
	}

	if ((mode & CAN_MODE_LISTENONLY) != 0) {
		return "listen-only";
	}

	return "normal";
}

static int can_mode_from_name(const char *name, can_mode_t *mode)
{
	if (name == NULL || strcmp(name, "normal") == 0) {
		*mode = CAN_MODE_NORMAL;
		return 0;
	}

	if (strcmp(name, "loopback") == 0) {
		*mode = CAN_MODE_LOOPBACK;
		return 0;
	}

	if (strcmp(name, "listen-only") == 0) {
		*mode = CAN_MODE_LISTENONLY;
		return 0;
	}

	return -EINVAL;
}

int can_bridge_format_status(char *response, size_t response_len)
{
	struct can_bus_err_cnt err_cnt = { 0 };
	enum can_state state = CAN_STATE_STOPPED;
	int ret;

	k_mutex_lock(&can_lock, K_FOREVER);
	ret = can_get_state(can_dev, &state, &err_cnt);
	k_mutex_unlock(&can_lock);
	if (ret < 0) {
		return ws_reply(response, response_len, "can_status_failed",
				"Could not read CAN controller status", ret);
	}

	return snprintk(response, response_len,
			"{\"type\":\"can.status\",\"ok\":true,\"bus\":0,"
			"\"ready\":%s,\"state\":\"%s\",\"bitrate\":%u,"
			"\"mode\":\"%s\",\"txErr\":%u,\"rxErr\":%u,"
			"\"txQueueUsed\":%u,\"txQueueFree\":%u,"
			"\"rxQueueUsed\":%u,\"rxQueueFree\":%u}",
			atomic_get(&can_ready) ? "true" : "false",
			can_state_name(state), current_bitrate,
			can_mode_name(current_mode), err_cnt.tx_err_cnt,
			err_cnt.rx_err_cnt,
			k_msgq_num_used_get(&can_tx_queue),
			k_msgq_num_free_get(&can_tx_queue),
			k_msgq_num_used_get(&can_rx_queue),
			k_msgq_num_free_get(&can_rx_queue));
}

static void can_rx_callback(const struct device *dev, struct can_frame *frame,
			    void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (!atomic_get(&can_rx_streaming)) {
		return;
	}

	if (k_msgq_put(&can_rx_queue, frame, K_NO_WAIT) != 0) {
		LOG_WRN("CAN RX software queue full");
	}
}

void can_bridge_set_rx_streaming(bool enabled)
{
	if (enabled) {
		k_msgq_purge(&can_rx_queue);
		atomic_set(&can_rx_streaming, 1);
		return;
	}

	atomic_clear(&can_rx_streaming);
	k_msgq_purge(&can_rx_queue);
}

static void can_tx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		struct can_frame frame;
		int ret;

		k_msgq_get(&can_tx_queue, &frame, K_FOREVER);

		LOG_INF("CAN TX worker sending: id=%u ext=%d rtr=%d dlc=%d",
			frame.id, (frame.flags & CAN_FRAME_IDE) != 0,
			(frame.flags & CAN_FRAME_RTR) != 0, frame.dlc);

		k_mutex_lock(&can_lock, K_FOREVER);
		ret = can_send(can_dev, &frame, CAN_TX_TIMEOUT, NULL, NULL);
		k_mutex_unlock(&can_lock);
		if (ret < 0) {
			LOG_WRN("CAN TX failed: %d", ret);
			continue;
		}

		LOG_INF("CAN TX submitted");
	}
}

int can_bridge_init(void)
{
	int ret;

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device is not ready");
		return -ENODEV;
	}

	ret = can_set_mode(can_dev, current_mode);
	if (ret < 0) {
		LOG_ERR("CAN normal mode failed: %d", ret);
		return ret;
	}

	ret = can_start(can_dev);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("CAN start failed: %d", ret);
		return ret;
	}

	const struct can_filter catch_all = {
		.id = 0,
		.mask = 0,
		.flags = 0,
	};

	can_rx_filter_id = can_add_rx_filter(can_dev, can_rx_callback, NULL,
					     &catch_all);
	if (can_rx_filter_id < 0) {
		LOG_ERR("CAN RX filter failed: %d", can_rx_filter_id);
		return can_rx_filter_id;
	}

	atomic_set(&can_ready, 1);
	k_thread_create(&can_tx_thread_data, can_tx_stack,
			K_THREAD_STACK_SIZEOF(can_tx_stack),
			can_tx_thread, NULL, NULL, NULL,
			CAN_TX_PRIORITY, 0, K_NO_WAIT);
	LOG_INF("CAN bus ready in normal mode at %u bit/s",
		CONFIG_CAN_DEFAULT_BITRATE);
	return 0;
}

int can_bridge_format_next_rx(char *response, size_t response_len)
{
	struct can_frame frame;
	size_t offset = 0;
	int ret;

	ret = k_msgq_get(&can_rx_queue, &frame, K_NO_WAIT);
	if (ret != 0) {
		return -EAGAIN;
	}

	ret = snprintk(response, response_len,
		       "{\"type\":\"can.rx\",\"bus\":0,\"id\":%u,"
		       "\"ext\":%s,\"rtr\":%s,\"dlc\":%u,\"data\":[",
		       frame.id,
		       (frame.flags & CAN_FRAME_IDE) != 0 ? "true" : "false",
		       (frame.flags & CAN_FRAME_RTR) != 0 ? "true" : "false",
		       frame.dlc);
	if (ret < 0 || ret >= response_len) {
		return -ENOMEM;
	}

	offset = ret;
	for (uint8_t i = 0; i < frame.dlc; i++) {
		ret = snprintk(&response[offset], response_len - offset,
			       "%s%u", i == 0 ? "" : ",", frame.data[i]);
		if (ret < 0 || ret >= response_len - offset) {
			return -ENOMEM;
		}
		offset += ret;
	}

	ret = snprintk(&response[offset], response_len - offset,
		       "],\"ts\":%lld}",
		       (long long)k_ticks_to_us_floor64(k_uptime_ticks()));
	if (ret < 0 || ret >= response_len - offset) {
		return -ENOMEM;
	}

	return 0;
}

static int format_can_tx_response(const struct can_json_message *msg,
				  char *response, size_t response_len)
{
	size_t offset;
	int ret;

	ret = snprintk(response, response_len,
		       "{\"type\":\"can.tx\",\"ok\":true,\"bus\":%d,"
		       "\"id\":%d,\"ext\":%s,\"rtr\":%s,\"dlc\":%d,"
		       "\"data\":[",
		       msg->bus, msg->id, msg->ext ? "true" : "false",
		       msg->rtr ? "true" : "false", msg->dlc);
	if (ret < 0 || ret >= response_len) {
		return -ENOMEM;
	}

	offset = ret;
	for (size_t i = 0; i < msg->data_len; i++) {
		ret = snprintk(&response[offset], response_len - offset,
			       "%s%d", i == 0 ? "" : ",", msg->data[i]);
		if (ret < 0 || ret >= response_len - offset) {
			return -ENOMEM;
		}
		offset += ret;
	}

	ret = snprintk(&response[offset], response_len - offset,
		       "],\"queued\":true,\"ts\":%lld}",
		       (long long)k_ticks_to_us_floor64(k_uptime_ticks()));
	if (ret < 0 || ret >= response_len - offset) {
		return -ENOMEM;
	}

	return 0;
}

static int validate_can_json(const struct can_json_message *msg, int64_t fields,
			     char *response, size_t response_len)
{
	const int64_t required = BIT(CAN_JSON_BUS) | BIT(CAN_JSON_ID) |
				 BIT(CAN_JSON_DLC);
	bool has_data = (fields & BIT(CAN_JSON_DATA)) != 0;

	if ((fields & required) != required) {
		return ws_reply(response, response_len, "missing_field",
				"CAN JSON must include bus, id and dlc", -EINVAL);
	}

	if (msg->bus != 0) {
		return ws_reply(response, response_len, "unsupported_bus",
				"Only CAN bus 0 is available", -ENODEV);
	}

	if (msg->dlc < 0 || msg->dlc > CAN_MAX_DLC) {
		return ws_reply(response, response_len, "invalid_dlc",
				"CAN dlc must be between 0 and 8", -EINVAL);
	}

	if (msg->id < 0 ||
	    (!msg->ext && msg->id > CAN_STD_ID_MASK) ||
	    (msg->ext && msg->id > CAN_EXT_ID_MASK)) {
		return ws_reply(response, response_len, "invalid_id",
				"CAN id is outside the selected frame format",
				-EINVAL);
	}

	if (!msg->rtr && msg->dlc > 0 && !has_data) {
		return ws_reply(response, response_len, "missing_data",
				"Data frames with dlc > 0 must include data",
				-EINVAL);
	}

	if (!msg->rtr && msg->data_len != (size_t)msg->dlc) {
		return ws_reply(response, response_len, "data_length_mismatch",
				"data length must match dlc", -EINVAL);
	}

	if (msg->rtr && has_data && msg->data_len > 0) {
		return ws_reply(response, response_len, "rtr_has_data",
				"RTR frames must not include data bytes", -EINVAL);
	}

	for (size_t i = 0; i < msg->data_len; i++) {
		if (msg->data[i] < 0 || msg->data[i] > 0xff) {
			return ws_reply(response, response_len, "invalid_data",
					"CAN data bytes must be between 0 and 255",
					-EINVAL);
		}
	}

	return 0;
}

static int handle_can_config_set(const struct can_json_message *msg,
				 int64_t fields, char *response,
				 size_t response_len)
{
	uint32_t bitrate = current_bitrate;
	can_mode_t mode = current_mode;
	int ret;

	if ((fields & BIT(CAN_JSON_BITRATE)) != 0) {
		if (msg->bitrate <= 0) {
			return ws_reply(response, response_len, "invalid_bitrate",
					"CAN bitrate must be positive", -EINVAL);
		}

		bitrate = (uint32_t)msg->bitrate;
	}

	if ((fields & BIT(CAN_JSON_MODE)) != 0) {
		ret = can_mode_from_name(msg->mode, &mode);
		if (ret < 0) {
			return ws_reply(response, response_len, "invalid_mode",
					"CAN mode must be normal, loopback or listen-only",
					ret);
		}
	}

	atomic_clear(&can_ready);
	k_mutex_lock(&can_lock, K_FOREVER);

	ret = can_stop(can_dev);
	if (ret < 0 && ret != -EALREADY) {
		goto out;
	}

	ret = can_set_mode(can_dev, mode);
	if (ret < 0) {
		goto out;
	}

	ret = can_set_bitrate(can_dev, bitrate);
	if (ret < 0) {
		goto out;
	}

	ret = can_start(can_dev);
	if (ret < 0 && ret != -EALREADY) {
		goto out;
	}

	current_mode = mode;
	current_bitrate = bitrate;
	atomic_set(&can_ready, 1);

out:
	k_mutex_unlock(&can_lock);
	if (ret < 0) {
		int config_ret = ret;

		k_mutex_lock(&can_lock, K_FOREVER);
		ret = can_start(can_dev);
		if (ret == 0 || ret == -EALREADY) {
			atomic_set(&can_ready, 1);
		}
		k_mutex_unlock(&can_lock);
		return ws_reply(response, response_len, "can_config_failed",
				"Could not apply CAN configuration", config_ret);
	}

	LOG_INF("CAN config applied: bitrate=%u mode=%s",
		current_bitrate, can_mode_name(current_mode));
	return can_bridge_format_status(response, response_len);
}

int can_bridge_handle_ws_text(const uint8_t *payload, size_t payload_len,
			      char *response, size_t response_len)
{
	struct can_json_message msg = { 0 };
	struct can_frame frame = { 0 };
	char json[CAN_WS_JSON_MAX];
	int64_t fields;
	int ret;

	if (!atomic_get(&can_ready)) {
		return ws_reply(response, response_len, "can_not_ready",
				"CAN controller is not ready", -ENODEV);
	}

	if (payload_len >= sizeof(json)) {
		return ws_reply(response, response_len, "payload_too_large",
				"WebSocket JSON payload is too large", -EMSGSIZE);
	}

	memcpy(json, payload, payload_len);
	json[payload_len] = '\0';

	fields = json_obj_parse(json, payload_len, can_json_descr,
				ARRAY_SIZE(can_json_descr), &msg);
	if (fields < 0) {
		return ws_reply(response, response_len, "invalid_json",
				"Could not parse CAN JSON payload", (int)fields);
	}

	if ((fields & BIT(CAN_JSON_TYPE)) != 0) {
		if (strcmp(msg.type, "can.status") == 0 ||
		    strcmp(msg.type, "can.config.get") == 0) {
			return can_bridge_format_status(response, response_len);
		}

		if (strcmp(msg.type, "can.config.set") == 0) {
			return handle_can_config_set(&msg, fields, response,
						     response_len);
		}

		if (strcmp(msg.type, "can.tx") != 0) {
			return ws_reply(response, response_len, "unsupported_type",
					"Unsupported WebSocket message type",
					-ENOTSUP);
		}
	}

	ret = validate_can_json(&msg, fields, response, response_len);
	if (ret != 0) {
		return ret;
	}

	frame.id = (uint32_t)msg.id;
	frame.dlc = (uint8_t)msg.dlc;
	frame.flags = (msg.ext ? CAN_FRAME_IDE : 0) |
		      (msg.rtr ? CAN_FRAME_RTR : 0);

	for (size_t i = 0; i < msg.data_len; i++) {
		frame.data[i] = (uint8_t)msg.data[i];
	}

	LOG_INF("CAN TX request accepted: bus=%d id=%d ext=%d rtr=%d dlc=%d",
		msg.bus, msg.id, msg.ext, msg.rtr, msg.dlc);

	ret = k_msgq_put(&can_tx_queue, &frame, K_NO_WAIT);
	if (ret < 0) {
		LOG_WRN("CAN TX software queue full: %d", ret);
		(void)ws_reply(response, response_len, "can_tx_failed",
			       "CAN TX software queue is full", ret);
		return ret;
	}

	return format_can_tx_response(&msg, response, response_len);
}
