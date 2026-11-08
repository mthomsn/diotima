#include "term.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

pthread_mutex_t term_mu = PTHREAD_MUTEX_INITIALIZER;

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
	fputs(label, stdout);
	fflush(stdout);
	buf[0] = 0;
	for (;;) {
		if (take_line(buf, n)) return 0;
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
