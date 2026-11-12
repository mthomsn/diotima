#include "term.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include "prose.h"

pthread_mutex_t term_mu = PTHREAD_MUTEX_INITIALIZER;

static int cols(void) { struct winsize ws; return !ioctl(1, TIOCGWINSZ, &ws) && ws.ws_col ? ws.ws_col : 80; }

/* ---- the waiting spinner: drawn at the cursor (saved and restored), erased by the next term_lock ---- */

static int spin_on, spin_drawn;
static double last_out;
static pthread_once_t spin_once = PTHREAD_ONCE_INIT;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }

static void *spinner(void *arg) {
	(void)arg;
	static const char *fr[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
	for (unsigned i = 0;; i++) {
		struct timespec d = { 0, 100000000 };
		nanosleep(&d, NULL);
		if (pthread_mutex_trylock(&term_mu)) continue; /* someone is printing or asking */
		int c = prose_col();
		if (spin_on && now_s() - last_out >= 0.3 && c + 12 < cols()) {
			printf("\0337%s\033[2m%s thinking\033[0m\0338", c ? " " : "", fr[i % 10]);
			fflush(stdout);
			spin_drawn = 1;
		}
		pthread_mutex_unlock(&term_mu);
	}
	return NULL;
}
static void spin_start(void) { pthread_t t; if (!pthread_create(&t, NULL, spinner, NULL)) pthread_detach(t); }

void term_lock(void) {
	pthread_mutex_lock(&term_mu);
	if (spin_drawn) { fputs("\033[K", stdout); fflush(stdout); spin_drawn = 0; }
}
void term_unlock(void) { last_out = now_s(); pthread_mutex_unlock(&term_mu); }

void term_spin(int on) {
	if (on && !isatty(1)) return;
	if (on) pthread_once(&spin_once, spin_start);
	term_lock();
	spin_on = on;
	term_unlock();
}

/* ---- typed input re-wrapped at word boundaries once entered ---- */

static int vis(const char *s) {
	int w = 0;
	for (; *s; s++) {
		if (*s == 033) { while (s[1] && *s != 'm') s++; continue; }
		if (((unsigned char)*s & 0xC0) != 0x80) w++;
	}
	return w;
}

/* The terminal echoed label+text wrapped mid-word; erase that and print it again by words. */
static void rewrap(const char *label, const char *text) {
	const char *ll = strrchr(label, '\n');
	ll = ll ? ll + 1 : label;
	int lw = vis(ll), tw = vis(text), C = cols(), W = C < 88 ? C : 88;
	if (lw + tw <= W) return;
	int rows = (lw + tw + C - 1) / C, ind = lw <= W / 3 ? lw : 2, col = lw, first = 1;
	printf("\033[%dA\r\033[J%s", rows, ll);
	for (const char *p = text; *p; ) {
		if (*p == ' ') { p++; continue; }
		size_t n = strcspn(p, " ");
		char w[n + 1];
		memcpy(w, p, n);
		w[n] = 0;
		p += n;
		int ww = vis(w);
		if (!first && col + 1 + ww > W) { printf("\n%*s", ind, ""); col = ind; }
		else if (!first) { putchar(' '); col++; }
		first = 0;
		for (const char *c = w; *c; ) { /* a word longer than the line breaks where it must */
			int k = 1;
			while ((c[k] & 0xC0) == 0x80) k++;
			if (col >= W) { printf("\n%*s", ind, ""); col = ind; }
			fwrite(c, 1, (size_t)k, stdout);
			col++;
			c += k;
		}
	}
	putchar('\n');
	fflush(stdout);
}

/* Our own buffer over fd 0, so poll() sees exactly what is unread. */
static char ibuf[1 << 16];
static size_t ilen;
static int wake_fd[2] = { -1, -1 };
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void init(void) {
	if (pipe(wake_fd) == 0) { fcntl(wake_fd[0], F_SETFL, O_NONBLOCK); fcntl(wake_fd[1], F_SETFL, O_NONBLOCK); }
}

void term_wake(void) {
	pthread_once(&once, init);
	if (wake_fd[1] >= 0 && write(wake_fd[1], "w", 1) < 0) {}
}

/* Take one buffered line into buf. 1 if a line was available. */
static int take_line(char *buf, size_t n) {
	char *nl = memchr(ibuf, '\n', ilen);
	if (!nl) return 0;
	size_t l = (size_t)(nl - ibuf), c = l < n - 1 ? l : n - 1;
	memcpy(buf, ibuf, c);
	buf[c] = 0;
	if (c && buf[c - 1] == '\r') buf[c - 1] = 0;
	memmove(ibuf, nl + 1, ilen - l - 1);
	ilen -= l + 1;
	return 1;
}

static int ask(const char *label, char *buf, size_t n, int wakeable) {
	pthread_once(&once, init);
	/* typed after the label (no typeahead) and on a terminal: its echo can be re-wrapped */
	struct pollfd pend = { .fd = 0, .events = POLLIN };
	int fresh = !ilen && isatty(0) && isatty(1) && poll(&pend, 1, 0) == 0;
	fputs(label, stdout);
	fflush(stdout);
	buf[0] = 0;
	for (;;) {
		if (take_line(buf, n)) { if (fresh) rewrap(label, buf); return 0; }
		struct pollfd p[2] = { { .fd = 0, .events = POLLIN }, { .fd = wake_fd[0], .events = POLLIN } };
		if (poll(p, wakeable && wake_fd[0] >= 0 ? 2 : 1, -1) < 0) continue;
		if (wakeable && (p[1].revents & POLLIN)) {
			char d[64];
			while (read(wake_fd[0], d, sizeof d) > 0) {}
			buf[0] = 0;
			return 1;
		}
		if (!(p[0].revents & (POLLIN | POLLHUP))) continue;
		if (ilen == sizeof ibuf) ilen = 0; /* an absurd line: drop it */
		ssize_t r = read(0, ibuf + ilen, sizeof ibuf - ilen);
		if (r <= 0) {
			if (ilen) { ibuf[ilen++] = '\n'; continue; } /* last line without newline */
			return -1;
		}
		ilen += (size_t)r;
	}
}

int term_ask(const char *label, char *buf, size_t n) { return ask(label, buf, n, 0) < 0 ? -1 : 0; }
int term_ask_wakeable(const char *label, char *buf, size_t n) { return ask(label, buf, n, 1); }

int term_has_images(void) {
	if (!isatty(1)) return 0;
	const char *tp = getenv("TERM_PROGRAM"), *t = getenv("TERM");
	return (tp && (!strcasecmp(tp, "ghostty") || !strcasecmp(tp, "kitty"))) || (t && (strstr(t, "kitty") || strstr(t, "ghostty")));
}
