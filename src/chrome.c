/* Headless Chrome over the DevTools protocol on --remote-debugging-pipe
 * (fd 3 in, fd 4 out, NUL-framed JSON): load a page, collect errors, probe. */
#include "chrome.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../vendor/cJSON.h"

static const char *chrome_path(void) {
	const char *e = getenv("DIOTIMA_CHROME");
	return e && *e ? e : "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
}

const char *chrome_unavailable(void) {
	return access(chrome_path(), X_OK) ? "no headless browser (install Google Chrome or set DIOTIMA_CHROME)" : NULL;
}

typedef struct { int in, out; char *buf; size_t len, cap; char session[128]; } cdp_t;

static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void send_cmd(cdp_t *c, int id, const char *method, cJSON *params, int in_session) {
	cJSON *m = cJSON_CreateObject();
	cJSON_AddNumberToObject(m, "id", id);
	cJSON_AddStringToObject(m, "method", method);
	cJSON_AddItemToObject(m, "params", params ? params : cJSON_CreateObject());
	if (in_session && c->session[0]) cJSON_AddStringToObject(m, "sessionId", c->session);
	char *s = cJSON_PrintUnformatted(m);
	size_t n = strlen(s) + 1, off = 0; /* the NUL is the frame */
	while (off < n) { ssize_t w = write(c->in, s + off, n - off); if (w <= 0) break; off += (size_t)w; }
	free(s);
	cJSON_Delete(m);
}

/* Next message, or NULL on timeout/EOF. */
static cJSON *next_msg(cdp_t *c, long deadline) {
	for (;;) {
		char *z = memchr(c->buf, 0, c->len);
		if (z) {
			cJSON *m = cJSON_Parse(c->buf);
			size_t used = (size_t)(z - c->buf) + 1;
			memmove(c->buf, z + 1, c->len - used);
			c->len -= used;
			if (m) return m;
			continue;
		}
		long left = deadline - now_ms();
		if (left <= 0) return NULL;
		struct pollfd p = { .fd = c->out, .events = POLLIN };
		if (poll(&p, 1, (int)left) <= 0) continue;
		if (c->len + 65536 > c->cap) c->buf = realloc(c->buf, c->cap = (c->len + 65536) * 2);
		ssize_t r = read(c->out, c->buf + c->len, c->cap - c->len);
		if (r <= 0) return NULL;
		c->len += (size_t)r;
	}
}

typedef struct { char *p; size_t n; int count; } rep_t;
static void add(rep_t *r, const char *kind, const char *text) {
	if (r->count >= 20) { r->count++; return; }
	size_t l = strlen(kind) + strlen(text ? text : "") + 8;
	r->p = realloc(r->p, r->n + l + 1);
	r->n += (size_t)snprintf(r->p + r->n, l + 1, "- %s: %.400s\n", kind, text ? text : "");
	r->count++;
}

/* Record an event's error, if it is one. */
static void on_event(const cJSON *m, rep_t *errs) {
	const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(m, "method"));
	const cJSON *p = cJSON_GetObjectItem(m, "params");
	if (!method) return;
	if (!strcmp(method, "Runtime.exceptionThrown")) {
		const cJSON *d = cJSON_GetObjectItem(p, "exceptionDetails");
		const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(d, "exception"), "description"));
		add(errs, "uncaught error", t ? t : cJSON_GetStringValue(cJSON_GetObjectItem(d, "text")));
	} else if (!strcmp(method, "Runtime.consoleAPICalled")) {
		const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(p, "type"));
		if (!type || strcmp(type, "error")) return;
		const cJSON *a = cJSON_GetArrayItem(cJSON_GetObjectItem(p, "args"), 0);
		const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(a, "value"));
		if (!t) t = cJSON_GetStringValue(cJSON_GetObjectItem(a, "description"));
		add(errs, "console error", t);
	} else if (!strcmp(method, "Log.entryAdded")) {
		const cJSON *e = cJSON_GetObjectItem(p, "entry");
		const char *lvl = cJSON_GetStringValue(cJSON_GetObjectItem(e, "level"));
		if (!lvl || strcmp(lvl, "error")) return;
		char t[1200];
		snprintf(t, sizeof t, "%s%s%s", cJSON_GetStringValue(cJSON_GetObjectItem(e, "text")) ?: "",
			cJSON_GetStringValue(cJSON_GetObjectItem(e, "url")) ? " " : "", cJSON_GetStringValue(cJSON_GetObjectItem(e, "url")) ?: "");
		add(errs, "failed request / log error", t);
	}
}

/* Wait for the response to id, handling events meanwhile. */
static cJSON *await_id(cdp_t *c, int id, long deadline, rep_t *errs) {
	for (cJSON *m; (m = next_msg(c, deadline)); cJSON_Delete(m)) {
		const cJSON *mid = cJSON_GetObjectItem(m, "id");
		if (cJSON_IsNumber(mid) && mid->valueint == id) return m;
		on_event(m, errs);
	}
	return NULL;
}

char *chrome_test(const char *html_path, const char *probe, int *passed) {
	*passed = 0;
	const char *why = chrome_unavailable();
	if (why) return strdup(why);
	char prof[] = "/tmp/diotima-chrome-XXXXXX";
	if (!mkdtemp(prof)) return strdup("could not create a browser profile directory");
	int to[2], from[2];
	if (pipe(to) || pipe(from)) return strdup("pipe failed");
	pid_t pid = fork();
	if (pid == 0) {
		setpgid(0, 0);
		int nul = open("/dev/null", O_RDWR);
		dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
		int a = dup(to[0]), b = dup(from[1]); /* move clear of 3/4 before placing them */
		dup2(a, 3); dup2(b, 4);
		for (int fd = 5; fd < 256; fd++) close(fd);
		char dir[64];
		snprintf(dir, sizeof dir, "--user-data-dir=%s", prof);
		execl(chrome_path(), "chrome", "--headless=new", "--remote-debugging-pipe", dir, "--no-first-run",
			"--no-default-browser-check", "--disable-gpu", "--disable-extensions", "about:blank", (char *)NULL);
		_exit(127);
	}
	close(to[0]); close(from[1]);
	cdp_t c = { .in = to[1], .out = from[0] };
	rep_t errs = {0};
	char *verdict = NULL;
	long deadline = now_ms() + 30000;
	cJSON *p = cJSON_CreateObject(), *r;
	cJSON_AddStringToObject(p, "url", "about:blank");
	send_cmd(&c, 1, "Target.createTarget", p, 0);
	r = await_id(&c, 1, deadline, &errs);
	const char *target = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "result"), "targetId"));
	if (!target) { verdict = strdup("the headless browser did not start"); cJSON_Delete(r); goto out; }
	p = cJSON_CreateObject();
	cJSON_AddStringToObject(p, "targetId", target);
	cJSON_AddTrueToObject(p, "flatten");
	cJSON_Delete(r);
	send_cmd(&c, 2, "Target.attachToTarget", p, 0);
	r = await_id(&c, 2, deadline, &errs);
	const char *sess = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "result"), "sessionId"));
	if (!sess) { verdict = strdup("could not attach to the page"); cJSON_Delete(r); goto out; }
	snprintf(c.session, sizeof c.session, "%s", sess);
	cJSON_Delete(r);
	const char *en[] = { "Runtime.enable", "Log.enable", "Page.enable" };
	for (int i = 0; i < 3; i++) { send_cmd(&c, 3 + i, en[i], NULL, 1); cJSON_Delete(await_id(&c, 3 + i, deadline, &errs)); }
	char url[1200];
	snprintf(url, sizeof url, "file://%s", html_path);
	p = cJSON_CreateObject();
	cJSON_AddStringToObject(p, "url", url);
	send_cmd(&c, 6, "Page.navigate", p, 1);
	/* until load (or 15s), then settle 2s collecting errors */
	long load_by = now_ms() + 15000;
	int loaded = 0;
	for (cJSON *m; !loaded && (m = next_msg(&c, load_by)); cJSON_Delete(m)) {
		const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(m, "method"));
		if (method && !strcmp(method, "Page.loadEventFired")) loaded = 1;
		on_event(m, &errs);
	}
	if (!loaded) add(&errs, "load", "the page did not finish loading within 15s");
	for (cJSON *m; (m = next_msg(&c, now_ms() + 2000)); cJSON_Delete(m)) on_event(m, &errs);
	char probe_line[1200] = "";
	if (probe && *probe) {
		p = cJSON_CreateObject();
		cJSON_AddStringToObject(p, "expression", probe);
		cJSON_AddTrueToObject(p, "returnByValue");
		cJSON_AddTrueToObject(p, "awaitPromise");
		send_cmd(&c, 7, "Runtime.evaluate", p, 1);
		r = await_id(&c, 7, now_ms() + 5000, &errs);
		const cJSON *res = cJSON_GetObjectItem(r, "result"), *ex = cJSON_GetObjectItem(res, "exceptionDetails");
		if (ex) add(&errs, "probe threw", cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(ex, "exception"), "description")) ?: "exception");
		else if (res) {
			const cJSON *v = cJSON_GetObjectItem(cJSON_GetObjectItem(res, "result"), "value");
			char *vs = v ? cJSON_PrintUnformatted(v) : strdup("undefined");
			snprintf(probe_line, sizeof probe_line, " Probe result: %.1000s.", vs);
			free(vs);
		} else add(&errs, "probe", "no answer from the page");
		cJSON_Delete(r);
	}
	send_cmd(&c, 8, "Browser.close", NULL, 0);
	if (!errs.count) {
		*passed = 1;
		size_t n = strlen(probe_line) + 160;
		verdict = malloc(n);
		snprintf(verdict, n, "PASSED: no console errors, no uncaught errors, no failed requests.%s", probe_line);
	} else {
		size_t n = errs.n + strlen(probe_line) + 160;
		verdict = malloc(n);
		snprintf(verdict, n, "FAILED: %d problem%s.%s\n%s%s", errs.count, errs.count == 1 ? "" : "s", probe_line, errs.p,
			errs.count > 20 ? "- (more omitted)\n" : "");
	}
out:
	close(c.in); close(c.out);
	kill(-pid, SIGKILL);
	waitpid(pid, NULL, 0);
	free(c.buf); free(errs.p);
	char *argv_rm[] = { "rm", "-rf", prof, NULL };
	pid_t rm = fork();
	if (rm == 0) { execvp("rm", argv_rm); _exit(127); }
	if (rm > 0) waitpid(rm, NULL, 0);
	return verdict;
}
