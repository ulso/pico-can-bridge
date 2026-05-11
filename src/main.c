#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

#define LED0_NODE DT_ALIAS(led0)

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static atomic_t heartbeat_period_ms = ATOMIC_INIT(1000);

struct usbd_context *usb_device_init(void);
int http_server_start(void);

static struct net_mgmt_event_callback ipv4_addr_cb;

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
		(void)gpio_pin_toggle_dt(&led);
		k_sleep(K_MSEC(atomic_get(&heartbeat_period_ms)));
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
	}
}

static void configure_usb_ncm_network(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface == NULL) {
		LOG_ERR("No default network interface");
		return;
	}

	net_mgmt_init_event_callback(&ipv4_addr_cb, ipv4_addr_handler,
				     NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_addr_cb);

	net_if_up(iface);

	LOG_INF("CDC-NCM waiting for IPv4 link-local autoconfiguration");
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
		atomic_set(&heartbeat_period_ms, 100);
		return 0;
	}

	configure_usb_ncm_network();

	ret = usbd_enable(usbd);
	if (ret != 0) {
		LOG_ERR("USB enable failed: %d", ret);
		atomic_set(&heartbeat_period_ms, 100);
		return 0;
	}

	atomic_set(&heartbeat_period_ms, 500);
	LOG_INF("USB CDC-NCM enabled");

	ret = http_server_start();
	if (ret != 0) {
		LOG_ERR("HTTP server failed: %d", ret);
		atomic_set(&heartbeat_period_ms, 100);
		return 0;
	}

	while (1) {
		k_sleep(K_SECONDS(10));
	}

	return 0;
}
