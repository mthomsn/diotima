#include "prose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "math.h"

#define MAX_WIDTH 88
#define BAR "\033[0m\033[2m│\033[0m "
#define BAR_BLANK "\033[0m\033[2m│\033[0m"
enum { BOLD = 1, ITAL = 2, CODE = 4, HEAD = 8, STRONG = 16 }; /* BOLD: the block's base; STRONG: markdown ** */

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
	sb_t tbl;                        /* a markdown table's lines, held until it ends (atoms wrapped in \1…\2) */
	int tline, intable, replay;      /* inside a table line; a table is held; re-feeding held text */
	const char *pre1, *pre2, *pre_blank; /* first line's prefix, later lines', a blank line's */
	int w1, w2;
} prose_t;

static prose_t P = { .pre1 = BAR, .pre2 = BAR, .pre_blank = BAR_BLANK, .w1 = 2, .w2 = 2 };

static void put(const char *s) { prose_out(s, strlen(s)); }

static void codes(int st, char *out) {
	strcpy(out, "\033[0m");
	if (st & (BOLD | HEAD | STRONG)) strcat(out, "\033[1m");
	if ((st & STRONG) && (st & (BOLD | HEAD))) strcat(out, "\033[4m"); /* ** inside bold text: underline, so it still shows */
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
	if (n >= 2 && ((P.style & STRONG) ? can_close : can_open)) { set_style(P.style ^ STRONG); n -= 2; }
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

static int at_line_start(void) { return !P.replay && !P.started && !P.word.n && !P.marked && !P.stars; }
static void table_flush(void);

static void text_char(char c) {
	if (P.skip) { if (c == '\n') { P.skip = 0; reset_line(); } return; }
	if (P.fence) { fence_char(c); return; }
	if (c == '\r') return;
	if (P.tline) { sb_add(&P.tbl, &c, 1); if (c == '\n') P.tline = 0, P.lead = 0; return; }
	if (at_line_start()) {
		if (c == '|') { P.tline = P.intable = 1; sb_add(&P.tbl, "|", 1); return; }
		if (P.intable && c != ' ' && c != '\t') table_flush();
	}
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
	if (P.tline) { sb_add(&P.tbl, "\1", 1); for (size_t i = 0; i < n; i++) if (s[i] != '\n') sb_add(&P.tbl, s + i, 1); sb_add(&P.tbl, "\2", 1); return; }
	if (P.intable && at_line_start()) table_flush();
	if (P.stars) resolve('x');
	for (size_t i = 0; i < n; i++) if (s[i] != '\n') add_byte((unsigned char)s[i]);
	sb_add(&P.raw, "x", 1);
}

/* ---- tables: held until the first line that isn't one, then drawn as a grid ---- */

/* Held text back through the stream (atoms unwrapped), with table detection off. */
static void replay(const char *s, size_t n) {
	for (size_t i = 0; i < n; i++) {
		if (s[i] != 1) { text_char(s[i]); continue; }
		size_t j = i + 1;
		while (j < n && s[j] != 2) j++;
		prose_atom(s + i + 1, j - i - 1);
		i = j;
	}
}

/* Columns a rendered line takes: escapes skipped, combining marks free. */
static int vwidth(const char *s) {
	int w = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == 033) { while (p[1] && *p != 'm') p++; continue; }
		if (*p < 0x80) { w++; continue; }
		if (*p < 0xC0) continue;
		int k = *p >= 0xF0 ? 3 : *p >= 0xE0 ? 2 : 1;
		unsigned cp = *p & (0x3Fu >> k);
		for (int i = 0; i < k && (p[1] & 0xC0) == 0x80; i++) cp = cp << 6 | (*++p & 0x3F);
		w += !zero_width(cp);
	}
	return w;
}

static sb_t cell_cap;
static void cell_sink(const char *s, size_t n) { sb_add(&cell_cap, s, n); }

/* One cell's markdown rendered at width (0 = unwrapped), split into lines (caller frees). */
static int render_cell(const char *src, int width, int bold, char ***lines) {
	prose_t saved = P;
	void (*out)(const char *, size_t) = prose_out;
	memset(&P, 0, sizeof P);
	P.pre1 = P.pre2 = P.pre_blank = "";
	P.base = P.style = bold ? BOLD : 0;
	P.width = width;
	P.replay = 1;
	cell_cap.n = 0;
	sb_add(&cell_cap, "", 0);
	prose_out = cell_sink;
	replay(src, strlen(src));
	prose_end();
	free(P.word.p); free(P.raw.p); free(P.fence_line.p); free(P.tbl.p);
	P = saved;
	prose_out = out;
	int n = 0;
	*lines = NULL;
	for (char *a = cell_cap.p; *a; ) {
		char *e = strchr(a, '\n');
		size_t l = e ? (size_t)(e - a) : strlen(a);
		*lines = realloc(*lines, sizeof **lines * (size_t)(n + 1));
		(*lines)[n++] = strndup(a, l);
		a += l + (e != NULL);
	}
	if (!n) { *lines = malloc(sizeof **lines); (*lines)[n++] = strdup(""); }
	return n;
}

static int widest(char **l, int n) { int w = 0; for (int i = 0; i < n; i++) if (vwidth(l[i]) > w) w = vwidth(l[i]); return w; }
static void free_lines(char **l, int n) { for (int i = 0; i < n; i++) free(l[i]); free(l); }

/* Split one table line into trimmed cells (<br> becomes a line break; \| a literal bar). */
static int split_row(const char *s, char ***cells) {
	int n = 0;
	sb_t c = {0};
	*cells = NULL;
	s += strspn(s, " \t");
	if (*s == '|') s++;
	for (int atom = 0;; s++) {
		if (*s == 1) atom = 1;
		else if (*s == 2) atom = 0;
		if (!atom && *s == '\\' && s[1] == '|') { sb_add(&c, "|", 1); s++; continue; }
		if (!atom && (!strncasecmp(s, "<br>", 4) || !strncasecmp(s, "<br/>", 5) || !strncasecmp(s, "<br />", 6))) {
			sb_add(&c, "\n", 1); s = strchr(s, '>'); continue;
		}
		if (*s && (atom || *s != '|')) { sb_add(&c, s, 1); continue; }
		const char *t = c.p ? c.p : "";
		size_t a = strspn(t, " \t"), e = c.n;
		while (e > a && (t[e - 1] == ' ' || t[e - 1] == '\t')) e--;
		if (*s || e > a) { *cells = realloc(*cells, sizeof **cells * (size_t)(n + 1)); (*cells)[n++] = strndup(t + a, e - a); }
		c.n = 0;
		if (!*s) break;
	}
	free(c.p);
	return n;
}

/* |---|:--:|--:| : 0 when not one; else per-column alignment ('l', 'c', 'r') into al. */
static int separator(char **cells, int n, char *al) {
	if (!n) return 0;
	for (int i = 0; i < n; i++) {
		const char *t = cells[i];
		size_t l = strlen(t), d = strspn(t + (t[0] == ':'), "-");
		if (!d || (t[0] == ':') + d + (l && t[l - 1] == ':' && l > 1) != l) return 0;
		if (i < 16) al[i] = t[0] == ':' && t[l - 1] == ':' && l > 1 ? 'c' : t[l - 1] == ':' ? 'r' : 'l';
	}
	return 1;
}

static void rule(const char *l, const char *m, const char *r, const int *w, int nc) {
	prefix();
	put("\033[2m");
	put(l);
	for (int i = 0; i < nc; i++) { for (int k = 0; k < w[i] + 2; k++) put("─"); put(i < nc - 1 ? m : r); }
	put("\033[0m\n");
}

static void table_flush(void) {
	sb_t held = P.tbl;
	P.tbl = (sb_t){0};
	P.intable = P.tline = 0;
	/* rows */
	int nr = 0, nc = 0;
	char ***row = NULL, al[16];
	int *rn = NULL;
	memset(al, 'l', sizeof al);
	for (char *a = held.p; a && *a; ) {
		char *e = strchr(a, '\n');
		if (e) *e = 0;
		row = realloc(row, sizeof *row * (size_t)(nr + 1));
		rn = realloc(rn, sizeof *rn * (size_t)(nr + 1));
		rn[nr] = split_row(a, &row[nr]);
		nr++;
		if (!e) break;
		*e = '\n';
		a = e + 1;
	}
	int ok = nr >= 2 && rn[0] > 0 && separator(row[1], rn[1], al);
	if (!ok) { /* not a table after all: the lines as plain text */
		P.replay = 1;
		replay(held.p, held.n);
		P.replay = 0;
	} else {
		for (int r = 0; r < nr; r++) if (r != 1 && rn[r] > nc) nc = rn[r];
		int avail = P.width > 0 ? P.width - P.w2 - (3 * nc + 1) : 0;
		int *nat = calloc((size_t)nc, sizeof *nat), *mn = calloc((size_t)nc, sizeof *mn), *w = calloc((size_t)nc, sizeof *w);
		for (int r = 0; r < nr; r++) {
			if (r == 1) continue;
			for (int c = 0; c < rn[r]; c++) {
				char **l;
				int k = render_cell(row[r][c], 0, !r, &l), x = widest(l, k);
				if (x > nat[c]) nat[c] = x;
				free_lines(l, k);
				k = render_cell(row[r][c], 1, !r, &l), x = widest(l, k);
				if (x > mn[c]) mn[c] = x;
				free_lines(l, k);
			}
		}
		/* natural widths if they fit; else every column its longest word, the rest shared by need */
		int sn = 0, sm = 0;
		for (int c = 0; c < nc; c++) { if (nat[c] < 1) nat[c] = 1; if (mn[c] < 1) mn[c] = 1; sn += nat[c]; sm += mn[c]; w[c] = nat[c]; }
		if (avail > 0 && sn > avail) {
			int spare = avail > sm ? avail - sm : 0, need = sn - sm, given = 0;
			for (int c = 0; c < nc; c++) { w[c] = mn[c] + (need ? (int)((long)spare * (nat[c] - mn[c]) / need) : 0); given += w[c] - mn[c]; }
			for (int c = 0; given < spare && c < nc; c = (c + 1) % nc) if (w[c] < nat[c]) w[c]++, given++;
		}
		rule("┌", "┬", "┐", w, nc);
		for (int r = 0; r < nr; r++) {
			if (r == 1) continue;
			char **cl[nc];
			int ck[nc], h = 1;
			for (int c = 0; c < nc; c++) {
				ck[c] = render_cell(c < rn[r] ? row[r][c] : "", w[c], !r, &cl[c]);
				if (ck[c] > h) h = ck[c];
			}
			for (int k = 0; k < h; k++) {
				prefix();
				put("\033[2m│\033[0m");
				for (int c = 0; c < nc; c++) {
					const char *t = k < ck[c] ? cl[c][k] : "";
					int pad = w[c] - vwidth(t), lp = 0;
					if (pad < 0) pad = 0;
					char a = c < 16 ? al[c] : 'l';
					if (a == 'r') lp = pad; else if (a == 'c') lp = pad / 2;
					put(" "); spaces(lp); put(t); spaces(pad - lp); put(" \033[2m│\033[0m");
				}
				put("\n");
			}
			for (int c = 0; c < nc; c++) free_lines(cl[c], ck[c]);
			int last = r == (nr > 2 ? nr - 1 : 0);
			rule(last ? "└" : "├", last ? "┴" : "┼", last ? "┘" : "┤", w, nc);
		}
		free(nat); free(mn); free(w);
	}
	for (int r = 0; r < nr; r++) free_lines(row[r], rn[r]);
	free(row); free(rn); free(held.p);
	P.started = P.col = P.marked = 0;
	P.blank = 0;
}

void prose_end(void) {
	if (P.tline) { sb_add(&P.tbl, "\n", 1); P.tline = 0; }
	if (P.intable) table_flush();
	if (P.stars) resolve(0);
	if (P.fence && P.fence_line.n) fence_char('\n');
	place();
	if (P.started) put("\033[0m\n");
	reset_line();
	P.fence = P.skip = 0;
	P.fence_line.n = 0;
	clear_word();
}

int prose_col(void) { return P.started ? P.linew + P.col : 0; }

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
	free(P.word.p); free(P.raw.p); free(P.fence_line.p); free(P.tbl.p);
	P = saved;
}
