#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>

#include "can_bridge.h"

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

#define LED0_NODE DT_ALIAS(led0)

enum status_led_mode {
	STATUS_LED_BLINK,
	STATUS_LED_SOLID,
	STATUS_LED_ERROR,
};

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static atomic_t status_led_mode = ATOMIC_INIT(STATUS_LED_BLINK);
static atomic_t heartbeat_period_ms = ATOMIC_INIT(1000);
static atomic_t network_should_run;
static atomic_t network_is_up;
static atomic_t network_is_ready;

struct usbd_context *usb_device_init(void);
int http_server_start(void);

static struct net_mgmt_event_callback ipv4_addr_cb;
static struct k_work_delayable usb_ncm_network_work;

static void status_led_set_blink(int period_ms)
{
	atomic_set(&heartbeat_period_ms, period_ms);
	atomic_set(&status_led_mode, STATUS_LED_BLINK);
}

static void status_led_set_solid(void)
{
	atomic_set(&status_led_mode, STATUS_LED_SOLID);
}

static void status_led_set_error(void)
{
	atomic_set(&heartbeat_period_ms, 100);
	atomic_set(&status_led_mode, STATUS_LED_ERROR);
}

static void usb_ncm_network_work_handler(struct k_work *work)
{
	struct net_if *iface = net_if_get_default();
	bool should_run = atomic_get(&network_should_run) != 0;

	ARG_UNUSED(work);

	if (iface == NULL) {
		LOG_ERR("No default network interface");
		status_led_set_error();
		return;
	}

	if (should_run && !atomic_get(&network_is_up)) {
		net_if_up(iface);
		atomic_set(&network_is_up, 1);
		LOG_INF("CDC-NCM waiting for IPv4 link-local autoconfiguration");
		return;
	}

	if (!should_run && atomic_get(&network_is_up)) {
		net_if_down(iface);
		atomic_set(&network_is_up, 0);
		atomic_set(&network_is_ready, 0);
		status_led_set_blink(500);
		LOG_INF("CDC-NCM network interface down");
	}
}

static void set_usb_ncm_network_state(bool up)
{
	atomic_set(&network_should_run, up ? 1 : 0);

	if (up) {
		k_work_schedule(&usb_ncm_network_work, K_SECONDS(1));
	} else {
		(void)k_work_cancel_delayable(&usb_ncm_network_work);
		k_work_schedule(&usb_ncm_network_work, K_NO_WAIT);
	}
}

static void usbd_msg_cb(struct usbd_context *const ctx,
			const struct usbd_msg *const msg)
{
	ARG_UNUSED(ctx);

	switch (msg->type) {
	case USBD_MSG_CONFIGURATION:
		LOG_INF("USBD message: %s", usbd_msg_type_string(msg->type));
		set_usb_ncm_network_state(true);
		break;
	case USBD_MSG_STACK_ERROR:
	case USBD_MSG_UDC_ERROR:
		LOG_WRN("USBD message: %s", usbd_msg_type_string(msg->type));
		atomic_set(&network_is_ready, 0);
		status_led_set_blink(500);
		set_usb_ncm_network_state(false);
		break;
	case USBD_MSG_RESET:
	case USBD_MSG_SUSPEND:
	case USBD_MSG_VBUS_REMOVED:
		LOG_DBG("USBD message: %s", usbd_msg_type_string(msg->type));
		atomic_set(&network_is_ready, 0);
		status_led_set_blink(500);
		set_usb_ncm_network_state(false);
		break;
	default:
		LOG_DBG("USBD message: %s", usbd_msg_type_string(msg->type));
		break;
	}
}

static void heartbeat_thread(void)
{
	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED GPIO is not ready");
		return;
	}

	if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) < 0) {
		LOG_ERR("Failed to configure LED GPIO");
		return;
	}

	while (1) {
		switch (atomic_get(&status_led_mode)) {
		case STATUS_LED_SOLID:
			(void)gpio_pin_set_dt(&led, 1);
			k_sleep(K_SECONDS(1));
			break;
		case STATUS_LED_ERROR:
		case STATUS_LED_BLINK:
		default:
			(void)gpio_pin_toggle_dt(&led);
			k_sleep(K_MSEC(atomic_get(&heartbeat_period_ms)));
			break;
		}
	}
}

K_THREAD_DEFINE(heartbeat_tid, 512, heartbeat_thread, NULL, NULL, NULL,
		7, 0, 0);

static void ipv4_addr_handler(struct net_mgmt_event_callback *cb,
			      uint64_t mgmt_event,
			      struct net_if *iface)
{
	struct net_if_config *cfg = net_if_get_config(iface);

	ARG_UNUSED(cb);
	ARG_UNUSED(mgmt_event);

	if (cfg == NULL || cfg->ip.ipv4 == NULL) {
		return;
	}

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *ifaddr = &cfg->ip.ipv4->unicast[i].ipv4;
		char addr[NET_IPV4_ADDR_LEN];

		if (!ifaddr->is_used || ifaddr->addr_type != NET_ADDR_AUTOCONF) {
			continue;
		}

		LOG_INF("CDC-NCM link-local IPv4 address: %s",
			net_addr_ntop(AF_INET, &ifaddr->address.in_addr,
				      addr, sizeof(addr)));
		LOG_INF("mDNS hostname: %s.local", CONFIG_NET_HOSTNAME);
		LOG_INF("HTTP service: _http._tcp.local on port 80");

		atomic_set(&network_is_ready, 1);
		status_led_set_solid();
	}
}

static void register_usb_ncm_network_callbacks(void)
{
	k_work_init_delayable(&usb_ncm_network_work, usb_ncm_network_work_handler);

	net_mgmt_init_event_callback(&ipv4_addr_cb, ipv4_addr_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_addr_cb);
}

int main(void)
{
	struct usbd_context *usbd;
	int ret;

	printk("Pico CAN Bridge CDC-NCM firmware\n");
	LOG_INF("Booted on %s", CONFIG_BOARD_TARGET);

	usbd = usb_device_init();
	if (usbd == NULL) {
		LOG_ERR("USB init failed");
		status_led_set_error();
		return 0;
	}

	register_usb_ncm_network_callbacks();

	ret = usbd_msg_register_cb(usbd, usbd_msg_cb);
	if (ret != 0) {
		LOG_ERR("USB message callback registration failed: %d", ret);
		status_led_set_error();
		return 0;
	}

	ret = usbd_enable(usbd);
	if (ret != 0) {
		LOG_ERR("USB enable failed: %d", ret);
		status_led_set_error();
		return 0;
	}

	status_led_set_blink(500);
	LOG_INF("USB CDC-NCM enabled");

	ret = can_bridge_init();
	if (ret != 0) {
		LOG_WRN("CAN bridge init failed: %d", ret);
	}

	ret = http_server_start();
	if (ret != 0) {
		LOG_ERR("HTTP server failed: %d", ret);
		status_led_set_error();
		return 0;
	}

	while (1) {
		k_sleep(K_SECONDS(10));
	}

	return 0;
}
