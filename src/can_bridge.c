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
	CAN_JSON_BUS,
	CAN_JSON_ID,
	CAN_JSON_EXT,
	CAN_JSON_RTR,
	CAN_JSON_DLC,
	CAN_JSON_DATA,
};

struct can_json_message {
	int32_t bus;
	int32_t id;
	bool ext;
	bool rtr;
	int32_t dlc;
	int32_t data[CAN_MAX_DLC];
	size_t data_len;
};

static const struct device *const can_dev = DEVICE_DT_GET(CAN_NODE);
static atomic_t can_ready;
K_MSGQ_DEFINE(can_tx_queue, sizeof(struct can_frame),
	      CAN_TX_QUEUE_LEN, sizeof(uint32_t));
K_MSGQ_DEFINE(can_rx_queue, sizeof(struct can_frame),
	      CAN_RX_QUEUE_LEN, sizeof(uint32_t));
static K_THREAD_STACK_DEFINE(can_tx_stack, CAN_TX_STACK_SIZE);
static struct k_thread can_tx_thread_data;
static int can_rx_filter_id = -1;

static const struct json_obj_descr can_json_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct can_json_message, bus, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct can_json_message, id, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct can_json_message, ext, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM(struct can_json_message, rtr, JSON_TOK_TRUE),
	JSON_OBJ_DESCR_PRIM(struct can_json_message, dlc, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_ARRAY(struct can_json_message, data, CAN_MAX_DLC,
			     data_len, JSON_TOK_NUMBER),
};

static int ws_reply(char *response, size_t response_len, const char *code,
		    const char *message, int err)
{
	return snprintk(response, response_len,
			"{\"type\":\"error\",\"ok\":false,\"code\":\"%s\","
			"\"err\":%d,\"message\":\"%s\"}",
			code, err, message);
}

static void can_rx_callback(const struct device *dev, struct can_frame *frame,
			    void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (k_msgq_put(&can_rx_queue, frame, K_NO_WAIT) != 0) {
		LOG_WRN("CAN RX software queue full");
	}
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

		ret = can_send(can_dev, &frame, CAN_TX_TIMEOUT, NULL, NULL);
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

	ret = can_set_mode(can_dev, CAN_MODE_NORMAL);
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

	return snprintk(response, response_len,
			"{\"type\":\"can.tx\",\"ok\":true,\"bus\":%d,"
			"\"id\":%d,\"ext\":%s,\"rtr\":%s,\"dlc\":%d,"
			"\"queued\":true,\"ts\":%lld}",
			msg.bus, msg.id, msg.ext ? "true" : "false",
			msg.rtr ? "true" : "false", msg.dlc,
			(long long)k_ticks_to_us_floor64(k_uptime_ticks()));
}
