#include "prose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "math.h"

#define MAX_WIDTH 88
#define BAR "\033[0m\033[2m│\033[0m "
#define BAR_BLANK "\033[0m\033[2m│\033[0m"
enum { BOLD = 1, ITAL = 2, CODE = 4, HEAD = 8 };

static void to_stdout(const char *s, size_t n) { fwrite(s, 1, n, stdout); fflush(stdout); }
int prose_width = -1;
void (*prose_out)(const char *s, size_t n) = to_stdout;

typedef struct { char *p; size_t n, cap; } sb_t;
static void sb_add(sb_t *b, const char *s, size_t n) {
	if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); }
	memcpy(b->p + b->n, s, n);
	b->p[b->n += n] = 0;
}

/* One stream's state. The teacher's lives across a turn; a block swaps in its own
 * so a tool's card never disturbs a half-streamed teacher line. */
typedef struct {
	sb_t word, raw, fence_line;      /* the word being built (rendered bytes, escapes included); its markdown source */
	int wordw, wstyle, lastc;        /* visible width; style when the word began; last text byte */
	int style, base, stars, space;   /* current style; style every line starts in; a pending run of '*'; a space owed */
	int started, col, lead, indent;  /* this line has its prefix; columns after it; leading spaces; hanging indent */
	int linew;                       /* this line's prefix width */
	int marked, fence, skip, gap, blank, width, firstdone;
	int cp_left;
	unsigned cp;
	const char *pre1, *pre2, *pre_blank; /* first line's prefix, later lines', a blank line's */
	int w1, w2;
} prose_t;

static prose_t P = { .pre1 = BAR, .pre2 = BAR, .pre_blank = BAR_BLANK, .w1 = 2, .w2 = 2 };

static void put(const char *s) { prose_out(s, strlen(s)); }

static void codes(int st, char *out) {
	strcpy(out, "\033[0m");
	if (st & (BOLD | HEAD)) strcat(out, "\033[1m");
	if (st & ITAL) strcat(out, "\033[3m");
	if (st & CODE) strcat(out, "\033[36m");
}

static int zero_width(unsigned c) {
	return (c >= 0x300 && c <= 0x36F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x20D0 && c <= 0x20FF) ||
		(c >= 0xFE20 && c <= 0xFE2F) || (c >= 0x200B && c <= 0x200D);
}

/* Columns a prefix takes: escapes skipped, UTF-8 counted by character. */
static int visible(const char *s) {
	int w = 0;
	for (; *s; s++) {
		if (*s == 033) { while (s[1] && *s != 'm') s++; continue; }
		if (((unsigned char)*s & 0xC0) != 0x80) w++;
	}
	return w;
}

static int term_width(void) {
	struct winsize ws;
	if (prose_width >= 0) return prose_width;
	return isatty(1) && !ioctl(1, TIOCGWINSZ, &ws) && ws.ws_col ? (ws.ws_col < MAX_WIDTH ? ws.ws_col : MAX_WIDTH) : 0;
}

static void add_byte(unsigned char c) {
	if (!P.word.n) P.wstyle = P.style;
	sb_add(&P.word, (const char *)&c, 1);
	if (c < 0x80) { P.wordw++; P.cp_left = 0; }
	else if (c >= 0xC0) { P.cp_left = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1; P.cp = c & (0x3Fu >> P.cp_left); }
	else if (P.cp_left) { P.cp = P.cp << 6 | (c & 0x3F); if (!--P.cp_left) P.wordw += !zero_width(P.cp); }
	P.lastc = c;
}

static void set_style(int st) {
	char e[32];
	if (!P.word.n) P.wstyle = P.style;
	P.style = st;
	codes(st, e);
	sb_add(&P.word, e, strlen(e));
}

/* Start a line with its prefix (the first-line form once). */
static void prefix(void) {
	if (P.gap) { put("\n"); P.gap = 0; }
	put(P.firstdone ? P.pre2 : P.pre1);
	P.linew = P.firstdone ? P.w2 : P.w1;
	P.firstdone = 1;
	P.started = 1;
	P.blank = 0;
}

/* The prefix reset the terminal: bring back the style this word starts in. */
static void restyle(void) { if (P.wstyle) { char e[32]; codes(P.wstyle, e); put(e); } }

static void spaces(int n) { while (n-- > 0) put(" "); }

static void clear_word(void) { P.word.n = P.raw.n = 0; P.wordw = 0; }

/* A finished word: read line-start markdown off it, then wrap and print it. */
static void place(void) {
	if (!P.word.n) return;
	if (!P.marked) {
		P.marked = 1;
		P.indent = P.lead;
		size_t h = strspn(P.raw.p, "#");
		if (!strncmp(P.raw.p, "```", 3)) { P.fence = P.skip = 1; P.style = P.base; clear_word(); return; }
		if (h >= 1 && h <= 6 && h == P.raw.n) { P.style |= HEAD; clear_word(); return; }
		if (P.raw.n == 1 && strchr("-*+", P.raw.p[0])) { clear_word(); add_byte(0xE2); add_byte(0x80); add_byte(0xA2); P.indent = P.lead + 2; } /* • */
		else {
			size_t d = strspn(P.raw.p, "0123456789");
			if (d && d + 1 == P.raw.n && strchr(".)", P.raw.p[d])) P.indent = P.lead + (int)P.raw.n + 1;
		}
	}
	int need = P.wordw + (P.started && P.space);
	if (P.started && P.width > 0 && P.col + need > P.width - P.linew && P.col > P.indent) {
		put("\033[0m\n");
		prefix();
		spaces(P.col = P.indent);
		restyle();
	} else if (!P.started) {
		prefix();
		spaces(P.col = P.lead);
		restyle();
	} else if (P.space) { put(" "); P.col++; }
	prose_out(P.word.p, P.word.n);
	P.col += P.wordw;
	clear_word();
	P.space = 0;
}

static void reset_line(void) {
	P.started = P.col = P.lead = P.indent = P.marked = P.stars = P.space = 0;
	P.style = P.base;
}

static void end_line(void) {
	if (P.started) put("\033[0m\n");
	else if (!P.gap && !P.blank && P.firstdone) { put(P.pre_blank); put("\n"); P.blank = 1; } /* a paragraph break keeps the bar unbroken */
	reset_line();
}

/* A run of '*' ended before nx (0 = end of text): bold/italic toggles, or literal stars. */
static void resolve(int nx) {
	int n = P.stars, sp = !nx || nx == ' ' || nx == '\t' || nx == '\n';
	P.stars = 0;
	int can_open = !sp && (!P.wordw || strchr("([{\"'", P.lastc)), can_close = P.wordw > 0;
	if (n >= 2 && ((P.style & BOLD) ? can_close : can_open)) { set_style(P.style ^ BOLD); n -= 2; }
	if (n == 1 && ((P.style & ITAL) ? can_close : can_open)) { set_style(P.style ^ ITAL); n = 0; }
	while (n-- > 0) add_byte('*');
}

static void fence_char(char c) {
	if (c != '\n') { sb_add(&P.fence_line, &c, 1); return; }
	const char *s = P.fence_line.p ? P.fence_line.p : "";
	if (!strncmp(s + strspn(s, " "), "```", 3)) P.fence = 0;
	else { prefix(); put("\033[36m"); prose_out(s, P.fence_line.n); put("\033[0m\n"); }
	P.fence_line.n = 0;
	P.started = 0;
}

static void text_char(char c) {
	if (P.skip) { if (c == '\n') { P.skip = 0; reset_line(); } return; }
	if (P.fence) { fence_char(c); return; }
	if (c == '\r') return;
	if (c == '*' && !(P.style & CODE)) { P.stars++; sb_add(&P.raw, "*", 1); return; }
	if (P.stars) resolve(c);
	if (c == '`') { sb_add(&P.raw, "`", 1); set_style(P.style ^ CODE); return; }
	if (c == ' ' || c == '\t') {
		if (!P.started && !P.word.n && !P.marked) P.lead += c == '\t' ? 4 : 1;
		else { place(); if (P.started) P.space = 1; }
		return;
	}
	if (c == '\n') { place(); if (P.skip) { P.skip = 0; reset_line(); } else end_line(); return; }
	sb_add(&P.raw, &c, 1);
	add_byte((unsigned char)c);
}

void prose_text(const char *s, size_t n) { for (size_t i = 0; i < n; i++) text_char(s[i]); }

void prose_atom(const char *s, size_t n) {
	if (P.skip) return;
	if (P.fence) { for (size_t i = 0; i < n; i++) if (s[i] != '\n') sb_add(&P.fence_line, s + i, 1); return; }
	if (P.stars) resolve('x');
	for (size_t i = 0; i < n; i++) if (s[i] != '\n') add_byte((unsigned char)s[i]);
	sb_add(&P.raw, "x", 1);
}

void prose_end(void) {
	if (P.stars) resolve(0);
	if (P.fence && P.fence_line.n) fence_char('\n');
	place();
	if (P.started) put("\033[0m\n");
	reset_line();
	P.fence = P.skip = 0;
	P.fence_line.n = 0;
	clear_word();
}

void prose_begin(void) {
	P.gap = 1;
	P.blank = 0;
	P.firstdone = 0;
	P.width = term_width();
}

void prose_block(const char *text, const char *first, const char *rest, int bold) {
	prose_t saved = P;
	memset(&P, 0, sizeof P);
	P.pre1 = first ? first : BAR;
	P.pre2 = first ? (rest ? rest : first) : BAR;
	P.pre_blank = first ? "" : BAR_BLANK;
	P.w1 = visible(P.pre1);
	P.w2 = visible(P.pre2);
	P.base = P.style = bold ? BOLD : 0;
	P.width = term_width();
	math_stream_t m = { .out = prose_text, .atom = prose_atom, .bol = 1 };
	math_feed(&m, text ? text : "");
	math_flush(&m);
	prose_end();
	if (!P.firstdone) { put(P.pre1); put("\033[0m\n"); } /* an empty field still shows its prefix */
	free(P.word.p); free(P.raw.p); free(P.fence_line.p);
	P = saved;
}
