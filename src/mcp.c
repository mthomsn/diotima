#include "mcp.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAXSEEN 512
#define ADMIT_WAIT_S 30

static char token[33];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t seen_cv = PTHREAD_COND_INITIALIZER;
/* One channel per concurrent claude child: its own tool set and admission table. */
static struct {
	char config[512];
	const tool_t *tools;
	void *ctx;
	int ntools, nseen, next;
	struct { char id[96]; char name[96]; cJSON *input; int used; } seen[MAXSEEN];
} ch[MCP_CHANNELS];

const char *mcp_config(int c) { return ch[c].config; }

void mcp_set_tools(int c, const tool_t *t, int n) {
	pthread_mutex_lock(&mu);
	ch[c].tools = t; ch[c].ntools = n;
	pthread_mutex_unlock(&mu);
}

void mcp_set_ctx(int c, void *ctx) {
	pthread_mutex_lock(&mu);
	ch[c].ctx = ctx;
	pthread_mutex_unlock(&mu);
}

static __thread void *call_ctx;
static __thread char *call_images[8];
static __thread int call_nimages;
void *mcp_call_ctx(void) { return call_ctx; }
void mcp_attach_image(const char *png) { if (call_nimages < 8) call_images[call_nimages++] = strdup(png); }

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
char *mcp_base64(const unsigned char *p, size_t n) {
	char *o = malloc(4 * ((n + 2) / 3) + 1), *q = o;
	for (size_t i = 0; i < n; i += 3) {
		unsigned v = (unsigned)p[i] << 16 | (i + 1 < n ? (unsigned)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
		*q++ = B64[v >> 18 & 63]; *q++ = B64[v >> 12 & 63];
		*q++ = i + 1 < n ? B64[v >> 6 & 63] : '=';
		*q++ = i + 2 < n ? B64[v & 63] : '=';
	}
	*q = 0;
	return o;
}

void mcp_note_tool_use(int c, const char *id, const char *name, const cJSON *input) {
	pthread_mutex_lock(&mu);
	int nseen = ch[c].nseen;
	typeof(ch[c].seen[0]) *seen = ch[c].seen;
	int slot = nseen < MAXSEEN ? ch[c].nseen++ : (ch[c].next++ % MAXSEEN);
	if (seen[slot].input) cJSON_Delete(seen[slot].input);
	snprintf(seen[slot].id, sizeof seen[slot].id, "%s", id);
	snprintf(seen[slot].name, sizeof seen[slot].name, "%s", name);
	seen[slot].input = cJSON_Duplicate(input, 1);
	seen[slot].used = 0;
	pthread_cond_broadcast(&seen_cv);
	pthread_mutex_unlock(&mu);
}

/* Wait for the stream to announce tool_use `id`; claim it. Returns an error text or NULL. */
static const char *admit(int c, const char *id, const char *name, const cJSON *args) {
	struct timespec dl;
	clock_gettime(CLOCK_REALTIME, &dl);
	dl.tv_sec += ADMIT_WAIT_S;
	const char *err = NULL;
	pthread_mutex_lock(&mu);
	for (;;) {
		int i, nseen = ch[c].nseen;
		typeof(ch[c].seen[0]) *seen = ch[c].seen;
		for (i = 0; i < nseen; i++) if (!strcmp(seen[i].id, id)) break;
		if (i < nseen) {
			char full[200];
			snprintf(full, sizeof full, MCP_PREFIX "%s", name);
			if (seen[i].used) err = "tool_use id already executed";
			else if (strcmp(seen[i].name, full)) err = "tool name does not match the stream";
			else if (!cJSON_Compare(seen[i].input, args, 1)) err = "arguments do not match the stream";
			else seen[i].used = 1;
			break;
		}
		if (pthread_cond_timedwait(&seen_cv, &mu, &dl)) { err = "call not announced by the stream (unadmitted)"; break; }
	}
	pthread_mutex_unlock(&mu);
	return err;
}

static void send_all(int fd, const char *s, size_t n) {
	while (n > 0) { ssize_t w = write(fd, s, n); if (w <= 0) return; s += w; n -= (size_t)w; }
}

static void reply(int fd, int code, const char *body) {
	char head[200];
	int n = snprintf(head, sizeof head, "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: keep-alive\r\n\r\n",
		code, body ? strlen(body) : 0);
	send_all(fd, head, (size_t)n);
	if (body) send_all(fd, body, strlen(body));
}

static cJSON *rpc_error(const cJSON *id, int code, const char *msg) {
	cJSON *r = cJSON_CreateObject(), *e = cJSON_AddObjectToObject(r, "error");
	cJSON_AddStringToObject(r, "jsonrpc", "2.0");
	cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, 1));
	cJSON_AddNumberToObject(e, "code", code);
	cJSON_AddStringToObject(e, "message", msg);
	return r;
}

static cJSON *tool_result(const char *text, int is_error) {
	cJSON *res = cJSON_CreateObject(), *content = cJSON_AddArrayToObject(res, "content"), *c = cJSON_CreateObject();
	cJSON_AddStringToObject(c, "type", "text");
	cJSON_AddStringToObject(c, "text", text);
	cJSON_AddItemToArray(content, c);
	for (int i = 0; i < call_nimages; i++) {
		FILE *f = fopen(call_images[i], "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long n = ftell(f);
			rewind(f);
			unsigned char *buf = malloc((size_t)n + 1);
			size_t got = fread(buf, 1, (size_t)n, f);
			fclose(f);
			char *b = mcp_base64(buf, got);
			cJSON *img = cJSON_CreateObject();
			cJSON_AddStringToObject(img, "type", "image");
			cJSON_AddStringToObject(img, "data", b);
			cJSON_AddStringToObject(img, "mimeType", "image/png");
			cJSON_AddItemToArray(content, img);
			free(b); free(buf);
		}
		free(call_images[i]);
	}
	call_nimages = 0;
	if (is_error) cJSON_AddTrueToObject(res, "isError");
	return res;
}

static char *handle_rpc(int c, const cJSON *req) {
	const cJSON *id = cJSON_GetObjectItem(req, "id"), *params = cJSON_GetObjectItem(req, "params");
	const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(req, "method"));
	if (!id || !method) return NULL; /* notification */
	cJSON *out;
	if (!strcmp(method, "initialize")) {
		cJSON *result = cJSON_CreateObject(), *caps = cJSON_AddObjectToObject(result, "capabilities"), *info = cJSON_AddObjectToObject(result, "serverInfo");
		cJSON_AddStringToObject(result, "protocolVersion", "2025-03-26");
		cJSON_AddObjectToObject(caps, "tools");
		cJSON_AddStringToObject(info, "name", "diotima");
		cJSON_AddStringToObject(info, "version", "0.1.0");
		out = cJSON_CreateObject();
		cJSON_AddStringToObject(out, "jsonrpc", "2.0");
		cJSON_AddItemToObject(out, "id", cJSON_Duplicate(id, 1));
		cJSON_AddItemToObject(out, "result", result);
	} else if (!strcmp(method, "ping")) {
		out = cJSON_CreateObject();
		cJSON_AddStringToObject(out, "jsonrpc", "2.0");
		cJSON_AddItemToObject(out, "id", cJSON_Duplicate(id, 1));
		cJSON_AddObjectToObject(out, "result");
	} else if (!strcmp(method, "tools/list")) {
		cJSON *result = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(result, "tools");
		pthread_mutex_lock(&mu);
		for (int i = 0; i < ch[c].ntools; i++) {
			const tool_t *tools = ch[c].tools;
			cJSON *t = cJSON_CreateObject();
			cJSON_AddStringToObject(t, "name", tools[i].name);
			cJSON_AddStringToObject(t, "description", tools[i].description);
			cJSON *schema = cJSON_Parse(tools[i].schema);
			cJSON_AddItemToObject(t, "inputSchema", schema ? schema : cJSON_CreateObject());
			cJSON_AddItemToArray(arr, t);
		}
		pthread_mutex_unlock(&mu);
		out = cJSON_CreateObject();
		cJSON_AddStringToObject(out, "jsonrpc", "2.0");
		cJSON_AddItemToObject(out, "id", cJSON_Duplicate(id, 1));
		cJSON_AddItemToObject(out, "result", result);
	} else if (!strcmp(method, "tools/call")) {
		const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(params, "name"));
		const cJSON *args = cJSON_GetObjectItem(params, "arguments");
		const char *tuid = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(params, "_meta"), "claudecode/toolUseId"));
		const tool_t *tool = NULL;
		pthread_mutex_lock(&mu);
		for (int i = 0; name && i < ch[c].ntools; i++) if (!strcmp(ch[c].tools[i].name, name)) tool = &ch[c].tools[i];
		pthread_mutex_unlock(&mu);
		cJSON *result = NULL;
		if (!tool || !tuid) { out = rpc_error(id, -32602, !tool ? "unknown tool" : "missing tool-use identity"); goto done; }
		cJSON *empty = args ? NULL : cJSON_CreateObject();
		const char *err = admit(c, tuid, name, args ? args : empty);
		if (err) { cJSON_Delete(empty); out = rpc_error(id, -32001, err); goto done; }
		int is_error = 0;
		pthread_mutex_lock(&mu);
		call_ctx = ch[c].ctx;
		pthread_mutex_unlock(&mu);
		call_nimages = 0;
		char *text = tool->run(args ? args : empty, &is_error);
		cJSON_Delete(empty);
		result = tool_result(text ? text : "", is_error);
		free(text);
		out = cJSON_CreateObject();
		cJSON_AddStringToObject(out, "jsonrpc", "2.0");
		cJSON_AddItemToObject(out, "id", cJSON_Duplicate(id, 1));
		cJSON_AddItemToObject(out, "result", result);
	} else {
		out = rpc_error(id, -32601, "method not found");
	}
done:;
	char *s = cJSON_PrintUnformatted(out);
	cJSON_Delete(out);
	return s;
}

static void *serve(void *arg) {
	int fd = (int)(long)arg;
	size_t cap = 1 << 20, have = 0;
	char *buf = malloc(cap), want[64];
	snprintf(want, sizeof want, "Bearer %s", token);
	for (;;) {
		char *eoh;
		while (!(eoh = memmem(buf, have, "\r\n\r\n", 4))) {
			if (have + 1 >= cap) cap *= 2, buf = realloc(buf, cap);
			ssize_t r = read(fd, buf + have, cap - have - 1);
			if (r <= 0) goto out;
			have += (size_t)r;
		}
		size_t hlen = (size_t)(eoh - buf) + 4;
		char *hdr = strndup(buf, hlen);
		char *cl = strcasestr(hdr, "content-length:");
		size_t blen = cl ? (size_t)atol(cl + 15) : 0;
		int authed = strstr(hdr, want) != NULL, post = !strncmp(hdr, "POST ", 5), c = -1;
		if (!strncmp(hdr, "POST /mcp/", 10) && hdr[10] >= '0' && hdr[10] < '0' + MCP_CHANNELS && hdr[11] == ' ') c = hdr[10] - '0';
		free(hdr);
		while (have < hlen + blen) {
			if (have + 1 >= cap) cap *= 2, buf = realloc(buf, cap);
			ssize_t r = read(fd, buf + have, cap - have - 1);
			if (r <= 0) goto out;
			have += (size_t)r;
		}
		char *body = strndup(buf + hlen, blen);
		memmove(buf, buf + hlen + blen, have - hlen - blen);
		have -= hlen + blen;
		if (!authed) reply(fd, 401, "{}");
		else if (!post) reply(fd, 405, "{}");
		else if (c < 0) reply(fd, 404, "{}");
		else {
			cJSON *req = cJSON_Parse(body);
			char *o = req ? handle_rpc(c, req) : NULL;
			if (o) { reply(fd, 200, o); free(o); } else reply(fd, 202, NULL);
			cJSON_Delete(req);
		}
		free(body);
	}
out:
	free(buf);
	close(fd);
	return NULL;
}

static void *accept_loop(void *arg) {
	int ls = (int)(long)arg;
	for (;;) {
		int fd = accept(ls, NULL, NULL);
		if (fd < 0) continue;
		pthread_t t;
		pthread_create(&t, NULL, serve, (void *)(long)fd);
		pthread_detach(t);
	}
	return NULL;
}

int mcp_start(void) {
	FILE *ur = fopen("/dev/urandom", "r");
	if (!ur) return -1;
	for (int i = 0; i < 32; i++) token[i] = "0123456789abcdef"[fgetc(ur) & 15];
	fclose(ur);
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 16)) return -1;
	socklen_t al = sizeof a;
	getsockname(ls, (struct sockaddr *)&a, &al);
	for (int c = 0; c < MCP_CHANNELS; c++)
		snprintf(ch[c].config, sizeof ch[c].config,
			"{\"mcpServers\":{\"diotima\":{\"type\":\"http\",\"url\":\"http://127.0.0.1:%d/mcp/%d\",\"headers\":{\"Authorization\":\"Bearer %s\"}}}}",
			ntohs(a.sin_port), c, token);
	pthread_t th;
	pthread_create(&th, NULL, accept_loop, (void *)(long)ls);
	pthread_detach(th);
	return 0;
}
