#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/dns_sd.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "can_bridge.h"

LOG_MODULE_REGISTER(http_server, CONFIG_LOG_DEFAULT_LEVEL);

#define HTTP_PORT 80
#define HTTP_STACK_SIZE 4096
#define MDNS_ANNOUNCE_STACK_SIZE 2048
#define WS_STACK_SIZE 4096
#define HTTP_PRIORITY 8
#define MDNS_ANNOUNCE_PRIORITY 8
#define WS_PRIORITY 8
#define MDNS_FAST_ANNOUNCE_COUNT 20
#define HTTP_REQUEST_MAX 1536
#define HTTP_SEND_CHUNK 256
#define WS_MAX_PAYLOAD 256
#define WS_RESPONSE_MAX 512

#define MDNS_PORT 5353
#define MDNS_PTR_TTL 4500
#define MDNS_HOST_TTL 120
#define DNS_CLASS_IN 1
#define DNS_CLASS_CACHE_FLUSH 0x8000
#define DNS_TYPE_A 1
#define DNS_TYPE_PTR 12
#define DNS_TYPE_TXT 16
#define DNS_TYPE_SRV 33
#define WS_OPCODE_CONTINUATION 0x0
#define WS_OPCODE_TEXT 0x1
#define WS_OPCODE_CLOSE 0x8
#define WS_OPCODE_PING 0x9
#define WS_OPCODE_PONG 0xa

static const char http_txt[] = "\x06" "path=/";
static const char ws_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

DNS_SD_REGISTER_TCP_SERVICE(pico_http, CONFIG_NET_HOSTNAME,
			    "_http", "local", http_txt, HTTP_PORT);

static int server_fd = -1;
static atomic_t started;
static atomic_t ws_active;
static K_THREAD_STACK_DEFINE(ws_stack, WS_STACK_SIZE);
static struct k_thread ws_thread;

struct websocket_client {
	int fd;
	uint8_t pending[HTTP_REQUEST_MAX];
	size_t pending_len;
	size_t pending_pos;
};

static struct websocket_client ws_client;

static const char index_html[] =
	"<!doctype html>"
	"<html lang=\"en\">"
	"<head>"
	"<meta charset=\"utf-8\">"
	"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	"<title>Pico CAN Bridge</title>"
	"<style>"
	":root{color-scheme:light dark;font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif}"
	"body{margin:0;background:#f3f5f7;color:#17202a}"
	"main{max-width:1120px;margin:0 auto;padding:20px}"
	"header{display:flex;align-items:center;justify-content:space-between;gap:16px;margin-bottom:14px}"
	"h1{font-size:24px;line-height:1.2;margin:0;font-weight:700}"
	".status{display:inline-flex;align-items:center;gap:8px;font-size:14px;font-weight:650}"
	".dot{width:10px;height:10px;border-radius:50%;background:#b7c0ca}"
	".ok .dot{background:#1f9d55}.bad .dot{background:#d64545}.wait .dot{background:#d99a22}"
	".topbar{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:14px}"
	".toolbar{display:flex;flex-wrap:wrap;gap:8px}"
	"button{border:1px solid #9aa7b2;background:#fff;color:#17202a;border-radius:6px;padding:8px 12px;font:inherit;font-weight:650;min-height:38px}"
	"button:disabled{opacity:.45}.primary{background:#17202a;color:#fff;border-color:#17202a}"
	".grid{display:grid;grid-template-columns:minmax(300px,360px) 1fr;gap:16px;align-items:start}"
	".stack{display:grid;gap:16px}"
	"section{margin-top:16px}.panel{border:1px solid #c7d0da;background:#fff;border-radius:8px;padding:14px}"
	"h2{font-size:15px;margin:0 0 12px;font-weight:700}"
	".metrics{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px}"
	".metric{border:1px solid #d7dee6;border-radius:6px;padding:8px;background:#f9fafb}"
	".metric span{display:block;font-size:11px;color:#5d6975}.metric strong{font-size:18px}"
	".stateOk{color:#1f9d55}.stateWarn{color:#b7791f}.stateBad{color:#d64545}.stateOff{color:#65717d}"
	"label{display:block;font-size:12px;font-weight:700;margin:0 0 5px;color:#303b46}"
	".form{display:grid;grid-template-columns:1fr 1fr;gap:10px}.wide{grid-column:1/-1}"
	"input,select{box-sizing:border-box;width:100%;border:1px solid #bdc7d1;border-radius:6px;background:#fff;color:#17202a;padding:8px;font:14px ui-monospace,SFMono-Regular,Menlo,monospace}"
	".checks{display:flex;align-items:center;gap:16px;margin-top:2px}.checks label{display:flex;gap:6px;align-items:center;margin:0;font-weight:650}"
	"input[type=checkbox]{width:auto}.hint{font-size:12px;color:#65717d;margin-top:6px}"
	".tableWrap{border:1px solid #c7d0da;border-radius:8px;overflow:auto;background:#fff;max-height:430px}"
	"table{width:100%;border-collapse:collapse;font:12px ui-monospace,SFMono-Regular,Menlo,monospace}"
	"th,td{padding:7px 8px;border-bottom:1px solid #edf0f3;text-align:left;white-space:nowrap}"
	"th{position:sticky;top:0;background:#f9fafb;font:12px -apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif;font-weight:700;color:#303b46}"
	".dirRx{color:#1f7a4d;font-weight:700}.dirTx{color:#285ea8;font-weight:700}.dirErr{color:#b42318;font-weight:700}"
	"pre{box-sizing:border-box;width:100%;min-height:120px;max-height:220px;overflow:auto;border:1px solid #c7d0da;border-radius:8px;background:#fff;color:#17202a;padding:10px;margin:0;font:12px ui-monospace,SFMono-Regular,Menlo,monospace;white-space:pre-wrap}"
	"@media(max-width:760px){main{padding:14px}.grid{grid-template-columns:1fr}.metrics{grid-template-columns:repeat(2,1fr)}header,.topbar{align-items:flex-start;flex-direction:column}.form{grid-template-columns:1fr}}"
	"@media(prefers-color-scheme:dark){body{background:#111820;color:#edf2f7}.panel,.tableWrap,pre,input,select,button{background:#17212b;color:#edf2f7;border-color:#3b4b5c}.metric,th{background:#1d2a36;border-color:#3b4b5c}.primary{background:#edf2f7;color:#111820}.hint,.metric span,label{color:#b9c3cf}td,th{border-bottom-color:#273544}}"
	"</style>"
	"</head>"
	"<body>"
	"<main>"
	"<header>"
	"<h1>Pico CAN Bridge</h1>"
	"<div id=\"status\" class=\"status bad\"><span class=\"dot\"></span><span id=\"statusText\">disconnected</span></div>"
	"</header>"
	"<div class=\"topbar\">"
	"<div class=\"toolbar\">"
	"<button id=\"connect\">Connect</button>"
	"<button id=\"disconnect\" disabled>Disconnect</button>"
	"<button id=\"clear\">Clear</button>"
	"</div>"
	"<div class=\"status\"><span id=\"bitrateLabel\">CAN -</span><span id=\"modeLabel\">mode -</span></div>"
	"</div>"
	"<section class=\"metrics\">"
	"<div class=\"metric\"><span>RX frames</span><strong id=\"rxCount\">0</strong></div>"
	"<div class=\"metric\"><span>TX requests</span><strong id=\"txCount\">0</strong></div>"
	"<div class=\"metric\"><span>CAN state</span><strong id=\"canState\">-</strong></div>"
	"<div class=\"metric\"><span>Last ID</span><strong id=\"lastId\">-</strong></div>"
	"</section>"
	"<section class=\"grid\">"
	"<div class=\"stack\">"
	"<div class=\"panel\">"
	"<h2>Transmit</h2>"
	"<div class=\"form\">"
	"<div><label for=\"canId\">ID</label><input id=\"canId\" value=\"123\" inputmode=\"text\"></div>"
	"<div><label for=\"dlc\">DLC</label><input id=\"dlc\" value=\"2\" inputmode=\"numeric\"></div>"
	"<div class=\"wide\"><label for=\"data\">Data bytes (hex)</label><input id=\"data\" value=\"01 02\" inputmode=\"text\"><div class=\"hint\">Hex bytes separated by spaces or commas</div></div>"
	"<div class=\"wide checks\"><label><input id=\"ext\" type=\"checkbox\"> Extended ID</label><label><input id=\"rtr\" type=\"checkbox\"> RTR</label></div>"
	"<div class=\"wide toolbar\"><button id=\"send\" class=\"primary\" disabled>Send Frame</button><button id=\"copyJson\">Copy JSON</button></div>"
	"</div>"
	"</div>"
	"<div class=\"panel\">"
	"<h2>Configuration</h2>"
	"<div class=\"form\">"
	"<div><label for=\"bitrate\">Bitrate</label><input id=\"bitrate\" value=\"500000\" inputmode=\"numeric\"></div>"
	"<div><label for=\"mode\">Mode</label><select id=\"mode\"><option value=\"normal\">normal</option><option value=\"loopback\">loopback</option><option value=\"listen-only\">listen-only</option></select></div>"
	"<div class=\"wide toolbar\"><button id=\"applyConfig\" disabled>Apply</button><button id=\"refreshStatus\" disabled>Status</button></div>"
	"<div class=\"wide hint\"><span id=\"errorLabel\">TX err 0 / RX err 0</span></div>"
	"</div>"
	"</div>"
	"</div>"
	"<div>"
	"<div class=\"tableWrap\">"
	"<table><thead><tr><th>Time</th><th>Dir</th><th>ID</th><th>Fmt</th><th>DLC</th><th>Data</th></tr></thead><tbody id=\"frames\"></tbody></table>"
	"</div>"
	"</div>"
	"</section>"
	"<section><pre id=\"log\"></pre></section>"
	"</main>"
	"<script>"
	"const statusEl=document.getElementById('status');"
	"const statusText=document.getElementById('statusText');"
	"const logEl=document.getElementById('log');"
	"const connectBtn=document.getElementById('connect');"
	"const disconnectBtn=document.getElementById('disconnect');"
	"const sendBtn=document.getElementById('send');"
	"const copyJsonBtn=document.getElementById('copyJson');"
	"const applyConfigBtn=document.getElementById('applyConfig');"
	"const refreshStatusBtn=document.getElementById('refreshStatus');"
	"const clearBtn=document.getElementById('clear');"
	"const framesEl=document.getElementById('frames');"
	"const rxCountEl=document.getElementById('rxCount');"
	"const txCountEl=document.getElementById('txCount');"
	"const canStateEl=document.getElementById('canState');"
	"const lastIdEl=document.getElementById('lastId');"
	"const bitrateLabel=document.getElementById('bitrateLabel');"
	"const modeLabel=document.getElementById('modeLabel');"
	"const errorLabel=document.getElementById('errorLabel');"
	"const idEl=document.getElementById('canId');"
	"const dlcEl=document.getElementById('dlc');"
	"const dataEl=document.getElementById('data');"
	"const extEl=document.getElementById('ext');"
	"const rtrEl=document.getElementById('rtr');"
	"const bitrateEl=document.getElementById('bitrate');"
	"const modeEl=document.getElementById('mode');"
	"let ws=null;"
	"let statusTimer=null;"
	"let rxCount=0,txCount=0,errCount=0;"
	"let configDirty=false;"
	"function now(){return new Date().toLocaleTimeString()}"
	"function log(line){logEl.textContent+=now()+'  '+line+'\\n';logEl.scrollTop=logEl.scrollHeight}"
	"function setStatus(kind,text){statusEl.className='status '+kind;statusText.textContent=text}"
	"function setConnected(on){connectBtn.disabled=on;disconnectBtn.disabled=!on;sendBtn.disabled=!on;applyConfigBtn.disabled=!on;refreshStatusBtn.disabled=!on}"
	"function parseId(text){const s=text.trim();return Number.parseInt(s,/^0x/i.test(s)?16:10)}"
	"function parseData(text){const s=text.trim();if(!s)return[];return s.split(/[ ,]+/).filter(Boolean).map(v=>Number.parseInt(v,/^0x/i.test(v)?16:16))}"
	"function fmtId(id,ext){return '0x'+Number(id).toString(16).toUpperCase().padStart(ext?8:3,'0')}"
	"function fmtData(data){return (data||[]).map(v=>Number(v).toString(16).toUpperCase().padStart(2,'0')).join(' ')}"
	"function esc(s){return String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]))}"
	"function updateCounts(){rxCountEl.textContent=rxCount;txCountEl.textContent=txCount}"
	"function stateView(state){if(state==='error-active')return['OK (error-active)','stateOk'];if(state==='error-warning')return['Warning (error-warning)','stateWarn'];if(state==='error-passive')return['Passive (error-passive)','stateWarn'];if(state==='bus-off')return['Bus off','stateBad'];if(state==='stopped')return['Stopped','stateOff'];return[state||'-','']}"
	"function updateStatus(msg){const st=stateView(msg.state);canStateEl.textContent=st[0];canStateEl.className=st[1];if(!configDirty){if(msg.bitrate)bitrateEl.value=msg.bitrate;if(msg.mode)modeEl.value=msg.mode}bitrateLabel.textContent=msg.bitrate?'CAN '+Math.round(msg.bitrate/1000)+' kbit/s':'CAN -';modeLabel.textContent='mode '+(msg.mode||'-');errorLabel.textContent='TX err '+(msg.txErr||0)+' / RX err '+(msg.rxErr||0)}"
	"function requestStatus(){if(ws&&ws.readyState===1)ws.send(JSON.stringify({type:'can.status'}))}"
	"function addFrame(dir,msg){const tr=document.createElement('tr');const cls=dir==='RX'?'dirRx':dir==='TX'?'dirTx':'dirErr';const dlc=msg.dlc==null?'':msg.dlc;tr.innerHTML='<td>'+now()+'</td><td class=\"'+cls+'\">'+dir+'</td><td>'+esc(fmtId(msg.id||0,msg.ext))+'</td><td>'+(msg.ext?'EXT':'STD')+(msg.rtr?' RTR':'')+'</td><td>'+esc(dlc)+'</td><td>'+esc(fmtData(msg.data))+'</td>';framesEl.prepend(tr);while(framesEl.children.length>80)framesEl.lastChild.remove();lastIdEl.textContent=fmtId(msg.id||0,msg.ext)}"
	"function makeFrame(){const data=parseData(dataEl.value);const dlc=Number.parseInt(dlcEl.value,10);if(data.some(v=>!Number.isInteger(v)||v<0||v>255))throw new Error('Invalid data byte');if(!Number.isInteger(dlc)||dlc<0||dlc>8)throw new Error('Invalid DLC');if(!rtrEl.checked&&data.length!==dlc)throw new Error('Data length must match DLC');const id=parseId(idEl.value);if(!Number.isInteger(id)||id<0)throw new Error('Invalid ID');return{type:'can.tx',bus:0,id,ext:extEl.checked,rtr:rtrEl.checked,dlc,data:rtrEl.checked?[]:data}}"
	"async function copyText(text){if(navigator.clipboard&&navigator.clipboard.writeText){await navigator.clipboard.writeText(text);return}const ta=document.createElement('textarea');ta.value=text;ta.style.position='fixed';ta.style.opacity='0';document.body.appendChild(ta);ta.focus();ta.select();document.execCommand('copy');ta.remove()}"
	"function connect(){"
	"if(ws&&ws.readyState<2)return;"
	"setStatus('wait','connecting');"
	"ws=new WebSocket('ws://'+location.host+'/can');"
	"ws.onopen=()=>{setStatus('ok','connected');setConnected(true);log('connected');requestStatus();statusTimer=setInterval(requestStatus,2000)};"
	"ws.onmessage=e=>{try{const msg=JSON.parse(e.data);if(msg.type!=='can.status')log('< '+e.data);if(msg.type==='can.rx'){rxCount++;addFrame('RX',msg)}else if(msg.type==='can.tx'){txCount++;addFrame('TX',msg)}else if(msg.type==='can.status'){updateStatus(msg)}else if(msg.type==='error'){errCount++;addFrame('ERR',msg)}updateCounts()}catch(_){log('< '+e.data)}};"
	"ws.onclose=e=>{if(statusTimer){clearInterval(statusTimer);statusTimer=null}setStatus('bad','disconnected');setConnected(false);log('closed '+e.code)};"
	"ws.onerror=()=>{setStatus('bad','error');errCount++;updateCounts();log('error')};"
	"}"
	"connectBtn.onclick=connect;"
	"disconnectBtn.onclick=()=>{if(ws)ws.close()};"
	"sendBtn.onclick=()=>{try{const frame=makeFrame();const text=JSON.stringify(frame);if(ws&&ws.readyState===1){ws.send(text);log('> '+text)}}catch(e){errCount++;updateCounts();log('! '+e.message)}};"
	"copyJsonBtn.onclick=async()=>{try{const text=JSON.stringify(makeFrame());await copyText(text);log('copied '+text)}catch(e){errCount++;updateCounts();log('! '+e.message)}};"
	"bitrateEl.oninput=()=>{configDirty=true};"
	"modeEl.onchange=()=>{configDirty=true};"
	"applyConfigBtn.onclick=()=>{const bitrate=Number.parseInt(bitrateEl.value,10);if(!Number.isInteger(bitrate)||bitrate<=0){log('! Invalid bitrate');return}const msg={type:'can.config.set',bitrate,mode:modeEl.value};const text=JSON.stringify(msg);if(ws&&ws.readyState===1){configDirty=false;ws.send(text);log('> '+text)}};"
	"refreshStatusBtn.onclick=()=>{configDirty=false;requestStatus()};"
	"clearBtn.onclick=()=>{logEl.textContent='';framesEl.textContent='';rxCount=0;txCount=0;errCount=0;lastIdEl.textContent='-';updateCounts()};"
	"log('ready');"
	"</script>"
	"</body>"
	"</html>";

static int send_all(int fd, const void *data, size_t len)
{
	const uint8_t *ptr = data;

	while (len > 0) {
		ssize_t ret;
		size_t chunk = MIN(len, HTTP_SEND_CHUNK);

		ret = zsock_send(fd, ptr, chunk, 0);
		if (ret < 0) {
			return -errno;
		}

		if (ret == 0) {
			return -ECONNRESET;
		}

		ptr += ret;
		len -= ret;
	}

	return 0;
}

struct sha1_ctx {
	uint32_t state[5];
	uint64_t bit_count;
	uint8_t buffer[64];
};

static uint32_t sha1_rol(uint32_t value, uint8_t bits)
{
	return (value << bits) | (value >> (32U - bits));
}

static void sha1_transform(struct sha1_ctx *ctx, const uint8_t block[64])
{
	uint32_t w[80];
	uint32_t a;
	uint32_t b;
	uint32_t c;
	uint32_t d;
	uint32_t e;

	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)block[i * 4] << 24) |
		       ((uint32_t)block[i * 4 + 1] << 16) |
		       ((uint32_t)block[i * 4 + 2] << 8) |
		       block[i * 4 + 3];
	}

	for (int i = 16; i < 80; i++) {
		w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	}

	a = ctx->state[0];
	b = ctx->state[1];
	c = ctx->state[2];
	d = ctx->state[3];
	e = ctx->state[4];

	for (int i = 0; i < 80; i++) {
		uint32_t f;
		uint32_t k;
		uint32_t temp;

		if (i < 20) {
			f = (b & c) | (~b & d);
			k = 0x5a827999;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ed9eba1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8f1bbcdc;
		} else {
			f = b ^ c ^ d;
			k = 0xca62c1d6;
		}

		temp = sha1_rol(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = sha1_rol(b, 30);
		b = a;
		a = temp;
	}

	ctx->state[0] += a;
	ctx->state[1] += b;
	ctx->state[2] += c;
	ctx->state[3] += d;
	ctx->state[4] += e;
}

static void sha1_init(struct sha1_ctx *ctx)
{
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xefcdab89;
	ctx->state[2] = 0x98badcfe;
	ctx->state[3] = 0x10325476;
	ctx->state[4] = 0xc3d2e1f0;
	ctx->bit_count = 0;
	memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

static void sha1_update(struct sha1_ctx *ctx, const uint8_t *data, size_t len)
{
	size_t index = (ctx->bit_count / 8U) % sizeof(ctx->buffer);

	ctx->bit_count += len * 8U;

	while (len > 0) {
		size_t space = sizeof(ctx->buffer) - index;
		size_t chunk = MIN(len, space);

		memcpy(&ctx->buffer[index], data, chunk);
		index += chunk;
		data += chunk;
		len -= chunk;

		if (index == sizeof(ctx->buffer)) {
			sha1_transform(ctx, ctx->buffer);
			index = 0;
		}
	}
}

static void sha1_final(struct sha1_ctx *ctx, uint8_t digest[20])
{
	uint8_t pad = 0x80;
	uint8_t zero = 0;
	uint8_t len_buf[8];
	uint64_t bits = ctx->bit_count;

	sha1_update(ctx, &pad, 1);

	while (((ctx->bit_count / 8U) % sizeof(ctx->buffer)) != 56U) {
		sha1_update(ctx, &zero, 1);
	}

	for (int i = 0; i < 8; i++) {
		len_buf[7 - i] = (uint8_t)(bits >> (i * 8));
	}

	sha1_update(ctx, len_buf, sizeof(len_buf));

	for (int i = 0; i < 5; i++) {
		digest[i * 4] = ctx->state[i] >> 24;
		digest[i * 4 + 1] = ctx->state[i] >> 16;
		digest[i * 4 + 2] = ctx->state[i] >> 8;
		digest[i * 4 + 3] = ctx->state[i];
	}
}

static int base64_encode_small(const uint8_t *src, size_t src_len, char *dst,
			       size_t dst_len)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t out = 0;

	for (size_t i = 0; i < src_len; i += 3) {
		uint32_t value = (uint32_t)src[i] << 16;
		size_t remain = src_len - i;

		if (remain > 1) {
			value |= (uint32_t)src[i + 1] << 8;
		}

		if (remain > 2) {
			value |= src[i + 2];
		}

		if (out + 4 >= dst_len) {
			return -ENOMEM;
		}

		dst[out++] = alphabet[(value >> 18) & 0x3f];
		dst[out++] = alphabet[(value >> 12) & 0x3f];
		dst[out++] = remain > 1 ? alphabet[(value >> 6) & 0x3f] : '=';
		dst[out++] = remain > 2 ? alphabet[value & 0x3f] : '=';
	}

	dst[out] = '\0';
	return 0;
}

static char ascii_tolower(char ch)
{
	if (ch >= 'A' && ch <= 'Z') {
		return ch + ('a' - 'A');
	}

	return ch;
}

static bool ascii_case_equal(const char *a, const char *b, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (ascii_tolower(a[i]) != ascii_tolower(b[i])) {
			return false;
		}
	}

	return true;
}

static const char *find_header_value(const char *request, const char *name)
{
	size_t name_len = strlen(name);
	const char *line = strstr(request, "\r\n");

	while (line != NULL && line[0] == '\r' && line[1] == '\n') {
		line += 2;

		if (line[0] == '\r' && line[1] == '\n') {
			return NULL;
		}

		if (ascii_case_equal(line, name, name_len) && line[name_len] == ':') {
			const char *value = line + name_len + 1;

			while (*value == ' ' || *value == '\t') {
				value++;
			}

			return value;
		}

		line = strstr(line, "\r\n");
	}

	return NULL;
}

static size_t header_value_len(const char *value)
{
	const char *end = strstr(value, "\r\n");

	if (end == NULL) {
		return 0;
	}

	while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
		end--;
	}

	return end - value;
}

static bool request_is_websocket(const char *request)
{
	return (strncmp(request, "GET /can ", 9) == 0 ||
		strncmp(request, "GET /ws ", 8) == 0) &&
	       find_header_value(request, "Sec-WebSocket-Key") != NULL;
}

static int build_websocket_accept(const char *request, char *accept,
				  size_t accept_len)
{
	const char *key = find_header_value(request, "Sec-WebSocket-Key");
	size_t key_len;
	struct sha1_ctx sha1;
	uint8_t digest[20];

	if (key == NULL) {
		return -EINVAL;
	}

	key_len = header_value_len(key);
	if (key_len == 0 || key_len > 64) {
		return -EINVAL;
	}

	sha1_init(&sha1);
	sha1_update(&sha1, key, key_len);
	sha1_update(&sha1, ws_guid, strlen(ws_guid));
	sha1_final(&sha1, digest);

	return base64_encode_small(digest, sizeof(digest), accept, accept_len);
}

static int send_websocket_frame(int client_fd, uint8_t opcode,
				const uint8_t *payload, size_t payload_len)
{
	uint8_t header[4];
	size_t header_len = 2;

	if (payload_len > 0xffff) {
		return -EMSGSIZE;
	}

	header[0] = 0x80 | opcode;

	if (payload_len <= 125) {
		header[1] = payload_len;
	} else {
		header[1] = 126;
		sys_put_be16(payload_len, &header[2]);
		header_len += 2;
	}

	int ret = send_all(client_fd, header, header_len);

	if (ret < 0) {
		return ret;
	}

	return payload_len > 0 ? send_all(client_fd, payload, payload_len) : 0;
}

static int ws_recv_exact(struct websocket_client *client, uint8_t *buf, size_t len)
{
	size_t received = 0;

	while (client->pending_pos < client->pending_len && received < len) {
		buf[received++] = client->pending[client->pending_pos++];
	}

	while (received < len) {
		ssize_t ret = zsock_recv(client->fd, &buf[received],
					 len - received, 0);

		if (ret <= 0) {
			return ret == 0 ? -ECONNRESET : -errno;
		}

		received += ret;
	}

	return 0;
}

static void websocket_echo_loop(struct websocket_client *client)
{
	uint8_t payload[WS_MAX_PAYLOAD];
	char response[WS_RESPONSE_MAX];

	if (client->pending_len > client->pending_pos) {
		LOG_INF("WebSocket has %u buffered bytes after HTTP upgrade",
			(unsigned int)(client->pending_len - client->pending_pos));
	}

	while (1) {
		struct zsock_pollfd pollfd = {
			.fd = client->fd,
			.events = ZSOCK_POLLIN,
		};
		uint8_t header[2];
		uint8_t mask[4];
		uint8_t opcode;
		bool masked;
		uint64_t payload_len;
		int ret;

		while (can_bridge_format_next_rx(response, sizeof(response)) == 0) {
			ret = send_websocket_frame(client->fd, WS_OPCODE_TEXT,
						   response, strlen(response));
			if (ret < 0) {
				LOG_WRN("WebSocket CAN RX send failed: %d", ret);
				return;
			}
		}

		if (client->pending_pos >= client->pending_len) {
			ret = zsock_poll(&pollfd, 1, 100);
			if (ret < 0) {
				LOG_INF("WebSocket poll failed: %d", errno);
				return;
			}

			if (ret == 0) {
				continue;
			}
		}

		ret = ws_recv_exact(client, header, sizeof(header));
		if (ret < 0) {
			LOG_INF("WebSocket header recv failed: %d", ret);
			return;
		}

		opcode = header[0] & 0x0f;
		masked = (header[1] & 0x80) != 0;
		payload_len = header[1] & 0x7f;

		LOG_DBG("WebSocket frame: raw=%02x %02x opcode=%u masked=%u len=%u",
			header[0], header[1], opcode, masked, (unsigned int)payload_len);

		if (payload_len == 126) {
			uint8_t ext[2];

			ret = ws_recv_exact(client, ext, sizeof(ext));
			if (ret < 0) {
				LOG_INF("WebSocket extended length recv failed: %d", ret);
				return;
			}

			payload_len = sys_get_be16(ext);
		} else if (payload_len == 127) {
			LOG_WRN("WebSocket payload too large");
			(void)send_websocket_frame(client->fd, WS_OPCODE_CLOSE, NULL, 0);
			return;
		}

		if (!masked || payload_len > sizeof(payload)) {
			LOG_WRN("Unsupported WebSocket frame");
			(void)send_websocket_frame(client->fd, WS_OPCODE_CLOSE, NULL, 0);
			return;
		}

		ret = ws_recv_exact(client, mask, sizeof(mask));
		if (ret < 0) {
			LOG_INF("WebSocket mask recv failed: %d", ret);
			return;
		}

		ret = ws_recv_exact(client, payload, payload_len);
		if (ret < 0) {
			LOG_INF("WebSocket payload recv failed: %d", ret);
			return;
		}

		for (uint64_t i = 0; i < payload_len; i++) {
			payload[i] ^= mask[i % 4];
		}

		if (opcode == WS_OPCODE_CLOSE) {
			LOG_INF("WebSocket close frame received");
			(void)send_websocket_frame(client->fd, WS_OPCODE_CLOSE,
						   payload, payload_len);
			return;
		}

		if (opcode == WS_OPCODE_PING) {
			(void)send_websocket_frame(client->fd, WS_OPCODE_PONG,
						   payload, payload_len);
			continue;
		}

		if (opcode != WS_OPCODE_TEXT && opcode != WS_OPCODE_CONTINUATION) {
			LOG_WRN("Unsupported WebSocket opcode: %u", opcode);
			(void)send_websocket_frame(client->fd, WS_OPCODE_CLOSE, NULL, 0);
			return;
		}

		ret = can_bridge_handle_ws_text(payload, payload_len,
						response, sizeof(response));
		if (ret < 0) {
			LOG_WRN("CAN WebSocket request failed: %d", ret);
		}

		ret = send_websocket_frame(client->fd, WS_OPCODE_TEXT,
					   response, strlen(response));
		if (ret < 0) {
			LOG_WRN("WebSocket send failed: %d", ret);
			return;
		}
	}
}

static void websocket_thread(void *p1, void *p2, void *p3)
{
	struct websocket_client *client = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	websocket_echo_loop(client);
	(void)zsock_close(client->fd);
	atomic_clear(&ws_active);
}

static int handle_websocket(int client_fd, const char *request,
			    const uint8_t *pending, size_t pending_len)
{
	char accept[32];
	char response_buf[192];
	int len;
	int ret;

	if (!atomic_cas(&ws_active, 0, 1)) {
		return -EBUSY;
	}

	ret = build_websocket_accept(request, accept, sizeof(accept));
	if (ret < 0) {
		atomic_clear(&ws_active);
		return ret;
	}

	len = snprintk(response_buf, sizeof(response_buf),
		      "HTTP/1.1 101 Switching Protocols\r\n"
		      "Upgrade: websocket\r\n"
		      "Connection: Upgrade\r\n"
		      "Sec-WebSocket-Accept: %s\r\n"
		      "\r\n",
		      accept);
	if (len < 0 || len >= sizeof(response_buf)) {
		atomic_clear(&ws_active);
		return -ENOMEM;
	}

	ret = send_all(client_fd, response_buf, len);
	if (ret < 0) {
		atomic_clear(&ws_active);
		return ret;
	}

	ws_client.fd = client_fd;
	ws_client.pending_len = MIN(pending_len, sizeof(ws_client.pending));
	ws_client.pending_pos = 0;

	if (ws_client.pending_len > 0) {
		memcpy(ws_client.pending, pending, ws_client.pending_len);
	}

	LOG_INF("WebSocket connected");
	k_thread_create(&ws_thread, ws_stack, K_THREAD_STACK_SIZEOF(ws_stack),
			websocket_thread, &ws_client, NULL, NULL,
			WS_PRIORITY, 0, K_NO_WAIT);
	return 0;
}

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
	char request[HTTP_REQUEST_MAX];
	char *header_end = NULL;
	size_t used = 0;

	while (used < sizeof(request) - 1) {
		ssize_t len = zsock_recv(client_fd, &request[used],
					 sizeof(request) - 1 - used, 0);

		if (len < 0) {
			LOG_WRN("recv failed: %d", errno);
			(void)zsock_close(client_fd);
			return;
		}

		if (len == 0) {
			(void)zsock_close(client_fd);
			return;
		}

		used += len;
		request[used] = '\0';

		header_end = strstr(request, "\r\n\r\n");
		if (header_end != NULL) {
			break;
		}
	}

	LOG_INF("HTTP request: %.*s", (int)MIN(used, 160), request);

	if (header_end == NULL) {
		static const char request_too_large[] =
			"HTTP/1.0 431 Request Header Fields Too Large\r\n"
			"Connection: close\r\n"
			"\r\n";

		LOG_WRN("HTTP request header incomplete after %u bytes",
			(unsigned int)used);
		(void)send_all(client_fd, request_too_large,
			       strlen(request_too_large));
		(void)zsock_close(client_fd);
		return;
	}

	if (request_is_websocket(request)) {
		uint8_t *pending = (uint8_t *)header_end + 4;
		size_t header_len = pending - (uint8_t *)request;
		size_t pending_len = used > header_len ? used - header_len : 0;
		LOG_INF("WebSocket upgrade header_len=%u pending_len=%u",
			(unsigned int)header_len, (unsigned int)pending_len);
		int ret = handle_websocket(client_fd, request, pending, pending_len);

		if (ret < 0) {
			LOG_WRN("WebSocket failed: %d", ret);
			(void)zsock_close(client_fd);
		}

		return;
	}

	char response_header[128];
	int response_header_len;

	response_header_len = snprintk(response_header, sizeof(response_header),
				       "HTTP/1.0 200 OK\r\n"
				       "Content-Type: text/html; charset=utf-8\r\n"
				       "Content-Length: %u\r\n"
				       "Connection: close\r\n"
				       "\r\n",
				       (unsigned int)strlen(index_html));

	if (response_header_len < 0 ||
	    response_header_len >= sizeof(response_header)) {
		LOG_WRN("HTTP response header too large");
		(void)zsock_close(client_fd);
		return;
	}

	if (send_all(client_fd, response_header, response_header_len) < 0 ||
	    send_all(client_fd, index_html, strlen(index_html)) < 0) {
		LOG_WRN("HTTP response send failed");
	}
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
