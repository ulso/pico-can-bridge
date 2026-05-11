#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dns_sd.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(http_server, CONFIG_LOG_DEFAULT_LEVEL);

#define HTTP_PORT 80
#define HTTP_STACK_SIZE 2048
#define MDNS_ANNOUNCE_STACK_SIZE 2048
#define HTTP_PRIORITY 8
#define MDNS_ANNOUNCE_PRIORITY 8
#define MDNS_FAST_ANNOUNCE_COUNT 20

#define MDNS_PORT 5353
#define MDNS_PTR_TTL 4500
#define MDNS_HOST_TTL 120
#define DNS_CLASS_IN 1
#define DNS_CLASS_CACHE_FLUSH 0x8000
#define DNS_TYPE_A 1
#define DNS_TYPE_PTR 12
#define DNS_TYPE_TXT 16
#define DNS_TYPE_SRV 33

static const char http_txt[] = "\x06" "path=/";

DNS_SD_REGISTER_TCP_SERVICE(pico_http, CONFIG_NET_HOSTNAME,
			    "_http", "local", http_txt, HTTP_PORT);

static int server_fd = -1;
static atomic_t started;

static const char response[] =
	"HTTP/1.0 200 OK\r\n"
	"Content-Type: text/plain\r\n"
	"Connection: close\r\n"
	"\r\n"
	"pico-can-bridge\r\n"
	"USB CDC-NCM link-local HTTP service\r\n";

static void put_u16(uint8_t *buf, size_t *offset, uint16_t value)
{
	buf[(*offset)++] = value >> 8;
	buf[(*offset)++] = value & 0xff;
}

static void put_u32(uint8_t *buf, size_t *offset, uint32_t value)
{
	buf[(*offset)++] = value >> 24;
	buf[(*offset)++] = (value >> 16) & 0xff;
	buf[(*offset)++] = (value >> 8) & 0xff;
	buf[(*offset)++] = value & 0xff;
}

static void put_bytes(uint8_t *buf, size_t *offset, const void *data, size_t len)
{
	memcpy(&buf[*offset], data, len);
	*offset += len;
}

static void put_label(uint8_t *buf, size_t *offset, const char *label)
{
	size_t len = strlen(label);

	buf[(*offset)++] = len;
	put_bytes(buf, offset, label, len);
}

static void put_host_name(uint8_t *buf, size_t *offset)
{
	put_label(buf, offset, CONFIG_NET_HOSTNAME);
	put_label(buf, offset, "local");
	buf[(*offset)++] = 0;
}

static void put_http_service_name(uint8_t *buf, size_t *offset)
{
	put_label(buf, offset, "_http");
	put_label(buf, offset, "_tcp");
	put_label(buf, offset, "local");
	buf[(*offset)++] = 0;
}

static void put_http_instance_name(uint8_t *buf, size_t *offset)
{
	put_label(buf, offset, CONFIG_NET_HOSTNAME);
	put_label(buf, offset, "_http");
	put_label(buf, offset, "_tcp");
	put_label(buf, offset, "local");
	buf[(*offset)++] = 0;
}

static void put_rr_header(uint8_t *buf, size_t *offset, uint16_t type,
			  uint16_t class_, uint32_t ttl, uint16_t rdlength)
{
	put_u16(buf, offset, type);
	put_u16(buf, offset, class_);
	put_u32(buf, offset, ttl);
	put_u16(buf, offset, rdlength);
}

static bool get_link_local_addr(struct in_addr *addr)
{
	struct net_if *iface = net_if_get_default();
	struct net_if_config *cfg;

	if (iface == NULL) {
		return false;
	}

	cfg = net_if_get_config(iface);
	if (cfg == NULL || cfg->ip.ipv4 == NULL) {
		return false;
	}

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *ifaddr = &cfg->ip.ipv4->unicast[i].ipv4;

		if (!ifaddr->is_used || ifaddr->addr_type != NET_ADDR_AUTOCONF ||
		    ifaddr->addr_state != NET_ADDR_PREFERRED) {
			continue;
		}

		addr->s_addr = ifaddr->address.in_addr.s_addr;
		return true;
	}

	return false;
}

static size_t build_mdns_http_announcement(uint8_t *buf, size_t buf_size,
					   const struct in_addr *addr)
{
	size_t offset = 0;
	size_t rdlength_offset;
	size_t rdata_start;

	ARG_UNUSED(buf_size);

	/* mDNS response, authoritative answer, four answer records. */
	put_u16(buf, &offset, 0);
	put_u16(buf, &offset, 0x8400);
	put_u16(buf, &offset, 0);
	put_u16(buf, &offset, 4);
	put_u16(buf, &offset, 0);
	put_u16(buf, &offset, 0);

	put_http_service_name(buf, &offset);
	rdlength_offset = offset + 8;
	put_rr_header(buf, &offset, DNS_TYPE_PTR, DNS_CLASS_IN, MDNS_PTR_TTL, 0);
	rdata_start = offset;
	put_http_instance_name(buf, &offset);
	sys_put_be16(offset - rdata_start, &buf[rdlength_offset]);

	put_http_instance_name(buf, &offset);
	rdlength_offset = offset + 8;
	put_rr_header(buf, &offset, DNS_TYPE_TXT,
		      DNS_CLASS_IN | DNS_CLASS_CACHE_FLUSH, MDNS_PTR_TTL, 0);
	rdata_start = offset;
	put_bytes(buf, &offset, http_txt, sizeof(http_txt) - 1);
	sys_put_be16(offset - rdata_start, &buf[rdlength_offset]);

	put_http_instance_name(buf, &offset);
	rdlength_offset = offset + 8;
	put_rr_header(buf, &offset, DNS_TYPE_SRV,
		      DNS_CLASS_IN | DNS_CLASS_CACHE_FLUSH, MDNS_HOST_TTL, 0);
	rdata_start = offset;
	put_u16(buf, &offset, 0);
	put_u16(buf, &offset, 0);
	put_u16(buf, &offset, HTTP_PORT);
	put_host_name(buf, &offset);
	sys_put_be16(offset - rdata_start, &buf[rdlength_offset]);

	put_host_name(buf, &offset);
	put_rr_header(buf, &offset, DNS_TYPE_A,
		      DNS_CLASS_IN | DNS_CLASS_CACHE_FLUSH, MDNS_HOST_TTL, 4);
	put_bytes(buf, &offset, &addr->s_addr, sizeof(addr->s_addr));

	return offset;
}

static void mdns_announce_thread(void)
{
	struct sockaddr_in dst = {
		.sin_family = AF_INET,
		.sin_port = sys_cpu_to_be16(MDNS_PORT),
		.sin_addr = { .s_addr = htonl(0xe00000fb) },
	};
	uint8_t packet[384];
	int sock;

	while (server_fd < 0) {
		k_sleep(K_MSEC(100));
	}

	sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_ERR("mDNS announce socket failed: %d", errno);
		return;
	}

	(void)zsock_setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL,
			       &(int){ 255 }, sizeof(int));

	while (1) {
		struct in_addr addr;

		if (get_link_local_addr(&addr)) {
			size_t len = build_mdns_http_announcement(packet,
								  sizeof(packet),
								  &addr);
			int ret = zsock_sendto(sock, packet, len, 0,
					       (struct sockaddr *)&dst, sizeof(dst));
			static int fast_announces;

			if (ret < 0) {
				LOG_WRN("mDNS HTTP announcement failed: %d", errno);
			} else {
				LOG_INF("Announced _http._tcp.local via mDNS");
			}

			if (fast_announces < MDNS_FAST_ANNOUNCE_COUNT) {
				fast_announces++;
				k_sleep(K_SECONDS(1));
				continue;
			}
		} else {
			k_sleep(K_MSEC(250));
			continue;
		}

		k_sleep(K_SECONDS(10));
	}
}

static void handle_client(int client_fd)
{
	char request[256];
	ssize_t len;

	len = zsock_recv(client_fd, request, sizeof(request) - 1, 0);
	if (len < 0) {
		LOG_WRN("recv failed: %d", errno);
		(void)zsock_close(client_fd);
		return;
	}

	request[len] = '\0';
	LOG_INF("HTTP request: %.*s", (int)MIN(len, 32), request);

	(void)zsock_send(client_fd, response, strlen(response), 0);
	(void)zsock_close(client_fd);
}

static void http_thread(void)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = sys_cpu_to_be16(HTTP_PORT),
		.sin_addr = { .s_addr = htonl(INADDR_ANY) },
	};

	server_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server_fd < 0) {
		LOG_ERR("socket failed: %d", errno);
		return;
	}

	if (zsock_bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("bind failed: %d", errno);
		(void)zsock_close(server_fd);
		server_fd = -1;
		return;
	}

	if (zsock_listen(server_fd, 1) < 0) {
		LOG_ERR("listen failed: %d", errno);
		(void)zsock_close(server_fd);
		server_fd = -1;
		return;
	}

	LOG_INF("HTTP server listening on port %d", HTTP_PORT);

	while (1) {
		int client_fd = zsock_accept(server_fd, NULL, NULL);

		if (client_fd < 0) {
			LOG_WRN("accept failed: %d", errno);
			k_sleep(K_MSEC(100));
			continue;
		}

		handle_client(client_fd);
	}
}

K_THREAD_DEFINE(http_tid, HTTP_STACK_SIZE, http_thread, NULL, NULL, NULL,
		HTTP_PRIORITY, 0, -1);
K_THREAD_DEFINE(mdns_announce_tid, MDNS_ANNOUNCE_STACK_SIZE,
		mdns_announce_thread, NULL, NULL, NULL,
		MDNS_ANNOUNCE_PRIORITY, 0, -1);

int http_server_start(void)
{
	if (!atomic_cas(&started, 0, 1)) {
		return 0;
	}

	k_thread_start(http_tid);
	k_thread_start(mdns_announce_tid);
	return 0;
}
