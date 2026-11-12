#include "jobs.h"
#include <pthread.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "agent.h"
#include "chrome.h"
#include "claude.h"
#include "img.h"
#include "proc.h"
#include "session.h"
#include "store.h"
#include "term.h"

#define SLOTS 3

typedef enum { J_RESEARCH, J_PDF, J_DEMO, J_VISUAL } kind_t;
static const char *KIND[] = { "research", "pdf", "demo", "visual" };

typedef struct job {
	char id[24], dir[1100];
	kind_t kind;
	cJSON *input;
	int chan;
	volatile pid_t pid;
	volatile int aborted;
	char *html, *svg, *rendered;  /* demo / visual working state */
	char image[1100];             /* visual: the learner's picture (PNG copy), or "" */
	int iw, ih;
	char pdf[1100];               /* pdf: absolute path */
	char *final;                  /* the child's final message */
} job_t;

typedef struct delivery { char id[24]; kind_t kind; char *text, *artifact, *title; int failed; struct delivery *next; } delivery_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t slot_cv = PTHREAD_COND_INITIALIZER;
static int slot_busy[SLOTS];
static job_t *running[64];
static int nrunning;
static delivery_t *done_head, *done_tail;

static const char *S_(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsString(v) ? v->valuestring : NULL; }
static int blank(const char *s) { if (!s) return 1; for (; *s; s++) if (*s != ' ' && *s != '\n' && *s != '\t') return 0; return 1; }
static char *fmt(const char *f, const char *a, const char *b) {
	size_t n = strlen(f) + (a ? strlen(a) : 0) + (b ? strlen(b) : 0) + 1;
	char *s = malloc(n);
	snprintf(s, n, f, a ? a : "", b ? b : "");
	return s;
}
static char *err(int *is_error, const char *f, const char *a) { *is_error = 1; return fmt(f, a, NULL); }

/* A learner path, relative to where dio was launched; absolute result in out. */
static int resolve(const char *p, char *out, size_t n) {
	if (!p || !*p) return -1;
	if (p[0] == '/') snprintf(out, n, "%s", p);
	else if (p[0] == '~' && p[1] == '/') snprintf(out, n, "%s%s", getenv("HOME") ? getenv("HOME") : "", p + 1);
	else snprintf(out, n, "%s/%s", claude_launch_dir(), p);
	return access(out, R_OK);
}

static void dim(const char *a, const char *b) {
	term_lock();
	printf("\033[2m· %s%s\033[0m\n", a, b ? b : "");
	fflush(stdout);
	term_unlock();
}

/* ── steer texts (the original's) ──────────────────────────────────────── */

static char *steer(const delivery_t *d, const char *notes) {
	const char *n = notes && *notes ? notes : "(none)";
	if (d->failed) {
		if (d->kind == J_RESEARCH || d->kind == J_PDF)
			return fmt(d->kind == J_RESEARCH ? "%s\n\nContinue with what you can establish and flag the uncertainty to the learner."
				: "%s\n\nContinue with what you can establish from the session and flag the gap to the learner.", d->text, NULL);
		if (d->kind == J_DEMO) {
			char *a = fmt("The demo \"%s\" could not be built: %s", d->title, n);
			char *b = fmt("%s. Fall back to explaining in prose; if the brief was the problem, re-call demo with a tighter one.", a, NULL);
			free(a);
			return b;
		}
		return fmt("The figure could not be made: %s. Fall back to prose or mermaid, or re-call visual with a tighter brief.", n, NULL);
	}
	if (d->kind == J_RESEARCH) return fmt("Research complete — the researcher's full brief follows. Weave it into the lesson and cite its sources when you use its findings:\n\n%s", d->text, NULL);
	if (d->kind == J_PDF) return fmt("PDF reading complete — the reader's brief on \"%s\" follows, with page citations. Weave it into the lesson and cite pages when you use it:\n\n%s", d->title, d->text);
	if (d->kind == J_DEMO) {
		char *a = fmt("The demo \"%s\" is built, headlessly tested, and opened for the learner. Builder notes: %s", d->title, n);
		char *b = fmt("%s.\nTeach with it — reference its controls by name and point the learner at what to try.", a, NULL);
		free(a);
		return b;
	}
	return fmt(d->title && *d->title ? "The annotated image is drawn, verified by inspection, and shown in the lesson. Maker notes: %s.\nReference its marks as you teach."
		: "The figure is drawn, verified by inspection, and shown in the lesson. Maker notes: %s.\nReference its elements as you teach.", n, NULL);
}

static const char *abort_text(kind_t k) {
	static const char *T[] = {
		"Research aborted — the learner aborted the research job, so no brief is coming. Acknowledge it in one line and continue teaching without it (or offer to re-dispatch it).",
		"The PDF reading job was aborted by the learner — no brief is coming. Acknowledge it in one line and continue without it (a re-dispatch is fine if the learner wants it).",
		"The demo job was aborted by the learner — no demo is coming. Acknowledge it in one line and teach the topic in prose instead (a re-dispatch is fine if the learner wants it).",
		"The figure job was aborted by the learner — no figure is coming. Acknowledge it in one line and teach without it (a re-dispatch is fine if the learner wants it).",
	};
	return T[k];
}

/* ── the sub-agents' own tools (ctx = the job) ─────────────────────────── */

static job_t *J(void) { return mcp_call_ctx(); }

static void save(job_t *j, const char *name, const char *text) {
	char p[1200];
	snprintf(p, sizeof p, "%s/%s", j->dir, name);
	store_write_atomic(p, text);
}

static char *t_write_demo(const cJSON *a, int *is_error) {
	const char *h = S_(a, "html");
	if (blank(h)) return err(is_error, "ERROR: empty document — write the full HTML.%s", NULL);
	job_t *j = J();
	free(j->html);
	j->html = strdup(h);
	save(j, "demo.html", h);
	char n[32];
	snprintf(n, sizeof n, "%zu", strlen(h));
	return fmt("Saved (%s bytes). Now call run_demo to test it — never deliver untested.", n, NULL);
}

static char *t_run_demo(const cJSON *a, int *is_error) {
	job_t *j = J();
	if (!j->html) return err(is_error, "ERROR: nothing written yet — call write_demo first.%s", NULL);
	char p[1200];
	snprintf(p, sizeof p, "%s/demo.html", j->dir);
	int ok;
	char *r = chrome_test(p, S_(a, "probe"), &ok);
	char *out = fmt(ok ? "%s If you are satisfied, finish with your notes; otherwise keep improving and re-test." : "%s\nFix the document (write_demo) and re-test (run_demo).", r, NULL);
	free(r);
	return out;
}

static char *t_write_svg(const cJSON *a, int *is_error) {
	const char *s = S_(a, "svg");
	if (blank(s) || !strstr(s, "<svg")) return err(is_error, "ERROR: write the complete <svg>…</svg> document.%s", NULL);
	job_t *j = J();
	free(j->svg);
	j->svg = strdup(s);
	return strdup("Written. Call render_svg to look at it.");
}

static char *t_edit_svg(const cJSON *a, int *is_error) {
	job_t *j = J();
	const char *o = S_(a, "old_text"), *n = S_(a, "new_text");
	if (!j->svg) return err(is_error, "ERROR: no SVG yet — call write_svg first.%s", NULL);
	if (blank(o) || !n) return err(is_error, "ERROR: old_text and new_text are required.%s", NULL);
	char *at = strstr(j->svg, o);
	if (!at) return err(is_error, "ERROR: old_text does not appear in the current SVG — copy it exactly, or write_svg the whole document.%s", NULL);
	if (strstr(at + 1, o)) return err(is_error, "ERROR: old_text appears more than once — include more surrounding text so it matches exactly once.%s", NULL);
	size_t ol = strlen(o), nl = strlen(n), sl = strlen(j->svg);
	char *s = malloc(sl - ol + nl + 1);
	memcpy(s, j->svg, (size_t)(at - j->svg));
	memcpy(s + (at - j->svg), n, nl);
	strcpy(s + (at - j->svg) + nl, at + ol);
	free(j->svg);
	j->svg = s;
	return strdup("Applied. Call render_svg to look at the result.");
}

/* The SVG with the learner-image placeholder replaced by the picture itself. */
static char *materialize(job_t *j) {
	if (!j->image[0] || !strstr(j->svg, "\"learner-image\"")) return strdup(j->svg);
	FILE *f = fopen(j->image, "rb");
	if (!f) return strdup(j->svg);
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	unsigned char *buf = malloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	char *b = mcp_base64(buf, got);
	free(buf);
	char *uri = fmt("\"data:image/png;base64,%s\"", b, NULL);
	free(b);
	size_t cap = strlen(j->svg) + strlen(uri) * 2 + 1, len = 0;
	char *out = malloc(cap);
	for (const char *s = j->svg, *hit; ; s = hit + 15) {
		hit = strstr(s, "\"learner-image\"");
		size_t l = hit ? (size_t)(hit - s) : strlen(s);
		while (len + l + strlen(uri) + 1 > cap) out = realloc(out, cap *= 2);
		memcpy(out + len, s, l); len += l;
		if (!hit) break;
		memcpy(out + len, uri, strlen(uri)); len += strlen(uri);
	}
	out[len] = 0;
	free(uri);
	return out;
}

/* Rasterize the current SVG to <dir>/figure.png. 0 ok. */
static int render(job_t *j) {
	char svgp[1200], png[1200];
	snprintf(svgp, sizeof svgp, "%s/figure.svg", j->dir);
	snprintf(png, sizeof png, "%s/figure.png", j->dir);
	char *m = materialize(j);
	store_write_atomic(svgp, m);
	free(m);
	unlink(png);
	return img_to_png(svgp, png, 1400) || access(png, R_OK) ? -1 : 0;
}

static char *t_render_svg(const cJSON *a, int *is_error) {
	(void)a;
	job_t *j = J();
	if (!j->svg) return err(is_error, "ERROR: no SVG yet — call write_svg first.%s", NULL);
	if (render(j)) return err(is_error, "ERROR: the SVG did not render — check that it is well-formed XML with width, height and viewBox.%s", NULL);
	char png[1200];
	snprintf(png, sizeof png, "%s/figure.png", j->dir);
	mcp_attach_image(png);
	free(j->rendered);
	j->rendered = strdup(j->svg);
	return strdup("Here is the current render of your SVG — inspect it critically (geometry, labels, clipping, readability):");
}

/* ── pdf tools (poppler) ───────────────────────────────────────────────── */

static int page_count(const char *pdf) {
	char *out = NULL, *argv[] = { "pdfinfo", (char *)pdf, NULL };
	int n = 0;
	if (proc_run(argv, &out, 0) == 0 && out) { char *p = strstr(out, "Pages:"); if (p) n = atoi(p + 6); }
	free(out);
	return n;
}

static char *page_text(const char *pdf, int page) {
	char f[16], *out = NULL;
	snprintf(f, sizeof f, "%d", page);
	char *argv[] = { "pdftotext", "-layout", "-f", f, "-l", f, (char *)pdf, "-", NULL };
	proc_run(argv, &out, 0);
	return out ? out : strdup("");
}

/* Math-flavored codepoint fraction (the original's ranges). */
static double math_density(const char *s) {
	long math = 0, n = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; ) {
		unsigned c = *p;
		int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
		unsigned cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
		for (int k = 1; k < len && p[k]; k++) cp = cp << 6 | (p[k] & 0x3F);
		p += len;
		n++;
		if ((cp >= 0x2200 && cp <= 0x22ff) || (cp >= 0x2100 && cp <= 0x214f) || (cp >= 0x2190 && cp <= 0x21ff) || (cp >= 0x27c0 && cp <= 0x27ef) ||
			(cp >= 0x2980 && cp <= 0x29ff) || (cp >= 0x2a00 && cp <= 0x2aff) || (cp >= 0x0370 && cp <= 0x03ff) || (cp >= 0x1d400 && cp <= 0x1d7ff) ||
			cp == 0xB1 || cp == 0xD7 || cp == 0xF7 || cp == 0xAC)
			math++;
	}
	return n ? (double)math / n : 0;
}

/* 'all' | '1-5' | '1,3,7' → sorted page flags in pick[1..total]. Count or -1. */
static int page_spec(const char *spec, int total, char *pick) {
	memset(pick, 0, (size_t)total + 1);
	if (!spec) return -1;
	if (!strcasecmp(spec, "all")) { memset(pick + 1, 1, (size_t)total); return total; }
	int count = 0;
	for (const char *p = spec; *p; ) {
		while (*p == ' ' || *p == ',') p++;
		if (!*p) break;
		char *e;
		long a = strtol(p, &e, 10), b = a;
		if (e == p) return -1;
		p = e;
		if (*p == '-') { b = strtol(p + 1, &e, 10); if (e == p + 1) return -1; p = e; }
		if (a < 1 || b < a || b > total) return -1;
		for (long i = a; i <= b; i++) if (!pick[i]) { pick[i] = 1; count++; }
	}
	return count;
}

typedef struct { char *p; size_t n, cap; } buf_t;
static void put(buf_t *b, const char *s) {
	size_t l = strlen(s);
	if (b->n + l + 1 > b->cap) { b->cap = (b->n + l + 1) * 2; b->p = realloc(b->p, b->cap); }
	memcpy(b->p + b->n, s, l + 1);
	b->n += l;
}

static char *t_pdf_info(const cJSON *a, int *is_error) {
	(void)a; (void)is_error;
	job_t *j = J();
	int n = page_count(j->pdf);
	if (n <= 0) return err(is_error, "ERROR: the PDF could not be read (pdfinfo found no pages).%s", NULL);
	int *imgs = calloc((size_t)n + 1, sizeof(int));
	char *out = NULL, *argv[] = { "pdfimages", "-list", j->pdf, NULL };
	if (proc_run(argv, &out, 0) == 0 && out) {
		int ln = 0;
		for (char *l = strtok(out, "\n"); l; l = strtok(NULL, "\n"), ln++) { int pg; if (ln >= 2 && sscanf(l, "%d", &pg) == 1 && pg >= 1 && pg <= n) imgs[pg]++; }
	}
	free(out);
	buf_t b = {0};
	char line[256];
	long total = 0;
	buf_t rows = {0};
	put(&rows, "");
	for (int i = 1; i <= n; i++) {
		char *t = page_text(j->pdf, i);
		long len = (long)strlen(t);
		total += len;
		snprintf(line, sizeof line, "  p.%d: text_length %ld, image_count %d, math_density %.4f\n", i, len, imgs[i], math_density(t));
		put(&rows, line);
		free(t);
	}
	snprintf(line, sizeof line, "File: %s\nPages: %d\nMean text length: %ld\nPer page:\n", j->pdf, n, total / n);
	put(&b, line);
	put(&b, rows.p);
	free(rows.p); free(imgs);
	return b.p;
}

static char *t_pdf_extract(const cJSON *a, int *is_error) {
	job_t *j = J();
	int n = page_count(j->pdf);
	char *pick = malloc((size_t)n + 2);
	if (page_spec(S_(a, "pages"), n, pick) <= 0) { free(pick); return err(is_error, "ERROR: bad page spec — use 'all', '1-5', '1,3,7' within the document's pages.%s", NULL); }
	buf_t b = {0};
	put(&b, "");
	char head[64];
	for (int i = 1; i <= n; i++) {
		if (!pick[i]) continue;
		if (b.n > 24000) { put(&b, "\n[output cap reached — narrow the page spec]\n"); break; }
		snprintf(head, sizeof head, "%s--- page %d ---\n", b.n ? "\n" : "", i);
		put(&b, head);
		char *t = page_text(j->pdf, i);
		put(&b, t);
		free(t);
	}
	free(pick);
	return b.p;
}

static char *t_pdf_search(const cJSON *a, int *is_error) {
	job_t *j = J();
	const char *q = S_(a, "query");
	if (blank(q)) return err(is_error, "ERROR: query is required.%s", NULL);
	const cJSON *cl = cJSON_GetObjectItem(a, "context_lines");
	int ctx = cJSON_IsNumber(cl) ? cl->valueint : 3, literal = cJSON_IsTrue(cJSON_GetObjectItem(a, "literal"));
	if (ctx < 0) ctx = 0;
	if (ctx > 8) ctx = 8;
	char pat[1024] = "";
	if (literal) { for (const char *c = q; *c && strlen(pat) < sizeof pat - 3; c++) { if (strchr(".^$*+?()[]{}|\\", *c)) strcat(pat, "\\"); strncat(pat, c, 1); } }
	else snprintf(pat, sizeof pat, "%s", q);
	regex_t re;
	if (regcomp(&re, pat, REG_EXTENDED | REG_ICASE | REG_NOSUB)) return err(is_error, "ERROR: invalid regex — fix it or pass literal:true.%s", NULL);
	int n = page_count(j->pdf), hits = 0;
	buf_t b = {0};
	put(&b, "");
	for (int i = 1; i <= n && hits < 50; i++) {
		char *t = page_text(j->pdf, i);
		char *lines[4096];
		int nl = 0;
		for (char *l = strtok(t, "\n"); l && nl < 4096; l = strtok(NULL, "\n")) lines[nl++] = l;
		for (int k = 0; k < nl && hits < 50; k++) {
			if (regexec(&re, lines[k], 0, NULL, 0)) continue;
			char head[64];
			snprintf(head, sizeof head, "\np.%d, line %d:\n", i, k + 1);
			put(&b, head);
			for (int c = k - ctx < 0 ? 0 : k - ctx; c <= k + ctx && c < nl; c++) { put(&b, c == k ? "> " : "  "); put(&b, lines[c]); put(&b, "\n"); }
			hits++;
		}
		free(t);
	}
	regfree(&re);
	if (!hits) { free(b.p); return strdup("No hits."); }
	char head[64];
	snprintf(head, sizeof head, "%d hit%s%s:", hits, hits == 1 ? "" : "s", hits >= 50 ? " (capped)" : "");
	char *out = fmt("%s%s", head, b.p);
	free(b.p);
	return out;
}

static char *t_pdf_render(const cJSON *a, int *is_error) {
	job_t *j = J();
	int n = page_count(j->pdf);
	char *pick = malloc((size_t)n + 2);
	int count = page_spec(S_(a, "pages"), n, pick);
	if (count <= 0) { free(pick); return err(is_error, "ERROR: bad page spec — use '1-3' or '2,5,8' within the document's pages.%s", NULL); }
	if (count > 6) { free(pick); return err(is_error, "ERROR: at most 6 pages per pdf_render call — render selectively.%s", NULL); }
	const cJSON *d = cJSON_GetObjectItem(a, "dpi");
	int dpi = cJSON_IsNumber(d) ? d->valueint : 150;
	if (dpi < 72) dpi = 72;
	if (dpi > 300) dpi = 300;
	char list[256] = "", r[16];
	snprintf(r, sizeof r, "%d", dpi);
	for (int i = 1; i <= n; i++) {
		if (!pick[i]) continue;
		char f[16], prefix[1200], png[1220];
		snprintf(f, sizeof f, "%d", i);
		snprintf(prefix, sizeof prefix, "%s/page-%d", j->dir, i);
		char *argv[] = { "pdftoppm", "-f", f, "-l", f, "-r", r, "-png", "-singlefile", j->pdf, prefix, NULL };
		if (proc_run(argv, NULL, 0)) continue;
		snprintf(png, sizeof png, "%s.png", prefix);
		mcp_attach_image(png);
		snprintf(list + strlen(list), sizeof list - strlen(list), "%s%d", list[0] ? ", " : "", i);
	}
	free(pick);
	if (!list[0]) return err(is_error, "ERROR: rendering failed — lower the DPI or narrow the pages.%s", NULL);
	return fmt("Rendered pages %s at %s dpi — the images follow.", list, r);
}

static const tool_t DEMO_TOOLS[] = {
	{ "write_demo", "Write the complete self-contained HTML document for the demo (overwrites any previous version). Always follow with run_demo to test it headlessly.",
	  "{\"type\":\"object\",\"properties\":{\"html\":{\"type\":\"string\",\"description\":\"The complete HTML document: <!doctype html>…</html>, all JS inline.\"}},\"required\":[\"html\"]}", t_write_demo },
	{ "run_demo", "Load the current demo in a headless browser, let it settle ~2s, and report console errors, uncaught errors and failed requests. Optionally pass a probe expression to assert on page state.",
	  "{\"type\":\"object\",\"properties\":{\"probe\":{\"type\":\"string\",\"description\":\"Optional JS expression evaluated in the page after settling, e.g. \\\"document.querySelectorAll('input').length\\\".\"}}}", t_run_demo },
};
static const tool_t VISUAL_TOOLS[] = {
	{ "write_svg", "Write the complete SVG source for the figure (overwrites any previous version).",
	  "{\"type\":\"object\",\"properties\":{\"svg\":{\"type\":\"string\",\"description\":\"Complete <svg xmlns=\\\"…\\\">…</svg> markup.\"}},\"required\":[\"svg\"]}", t_write_svg },
	{ "edit_svg", "Replace one exact spot in the current SVG: old_text must appear EXACTLY ONCE and is replaced by new_text. Call write_svg first. For larger changes, write_svg the whole document.",
	  "{\"type\":\"object\",\"properties\":{\"old_text\":{\"type\":\"string\",\"description\":\"The exact text to replace — must match exactly once in the current SVG.\"},\"new_text\":{\"type\":\"string\",\"description\":\"The replacement text.\"}},\"required\":[\"old_text\",\"new_text\"]}", t_edit_svg },
	{ "render_svg", "Render the current SVG to a PNG and show it to you for inspection. No arguments.", "{\"type\":\"object\",\"properties\":{}}", t_render_svg },
};
static const tool_t PDF_TOOLS[] = {
	{ "pdf_info", "Metadata + per-page triage signals for the PDF (page count, text length, image count, math density). ALWAYS call this first.", "{\"type\":\"object\",\"properties\":{}}", t_pdf_info },
	{ "pdf_extract", "Extract text from pages (spec: 'all', '1-5', '1,3,7', '3'). Use pdf_info first; narrow the spec for big documents.",
	  "{\"type\":\"object\",\"properties\":{\"pages\":{\"type\":\"string\",\"description\":\"Page spec, e.g. 'all', '1-5', '1,3,7'\"}},\"required\":[\"pages\"]}", t_pdf_extract },
	{ "pdf_render", "Render pages to images and show them to you for visual reading (equations, diagrams, layout). DPI: 100 text, 150 default, 200 dense small print. Max 6 pages per call.",
	  "{\"type\":\"object\",\"properties\":{\"pages\":{\"type\":\"string\",\"description\":\"Page spec, e.g. '1-3' or '2,5,8'\"},\"dpi\":{\"type\":\"integer\",\"minimum\":72,\"maximum\":300,\"description\":\"Render DPI (100 text / 150 default / 200 dense)\"}},\"required\":[\"pages\"]}", t_pdf_render },
	{ "pdf_search", "Search the PDF's text by keyword or regex (case-insensitive); returns hits with context lines. Use for long documents before reading anything at length.",
	  "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Keyword or regex\"},\"context_lines\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":8,\"description\":\"Context lines around each hit (default 3)\"},"
	  "\"literal\":{\"type\":\"boolean\",\"description\":\"true = treat the query literally (no regex; default false)\"}},\"required\":[\"query\"]}", t_pdf_search },
};

/* ── the runner ────────────────────────────────────────────────────────── */

static void on_job_event(const cJSON *ev, void *ctx) {
	job_t *j = ctx;
	claude_announce(j->chan, ev);
	const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "type"));
	if (type && !strcmp(type, "result")) {
		const char *r = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "result"));
		if (r) { free(j->final); j->final = strdup(r); }
	}
}

static void deliver_later(job_t *j, delivery_t *d) {
	pthread_mutex_lock(&mu);
	for (int i = 0; i < nrunning; i++) if (running[i] == j) { running[i] = running[--nrunning]; break; }
	if (!j->aborted) {
		d->next = NULL;
		if (done_tail) done_tail->next = d; else done_head = d;
		done_tail = d;
	} else { free(d->text); free(d->artifact); free(d->title); free(d); d = NULL; }
	pthread_mutex_unlock(&mu);
	if (d) term_wake();
}

static void *job_main(void *arg) {
	job_t *j = arg;
	pthread_mutex_lock(&mu);
	int slot = -1;
	while (!j->aborted) {
		for (int i = 0; i < SLOTS && slot < 0; i++) if (!slot_busy[i]) slot = i;
		if (slot >= 0) break;
		pthread_cond_wait(&slot_cv, &mu);
	}
	if (slot >= 0) slot_busy[slot] = 1;
	pthread_mutex_unlock(&mu);
	delivery_t *d = calloc(1, sizeof *d);
	snprintf(d->id, sizeof d->id, "%s", j->id);
	d->kind = j->kind;
	if (slot < 0) { deliver_later(j, d); return NULL; }
	j->chan = MCP_CHAN_JOB0 + slot;
	mcp_set_ctx(j->chan, j);

	const cJSON *in = j->input;
	agent_kind_t ak = j->kind == J_RESEARCH ? AGENT_RESEARCHER : j->kind == J_PDF ? AGENT_PDF_READER : j->kind == J_DEMO ? AGENT_DEMO_BUILDER
		: j->image[0] ? AGENT_VISUAL_ANNOTATOR : AGENT_VISUAL_MAKER;
	const tool_t *tools = NULL;
	int nt = 0, turns = 12;
	const char *builtin = NULL;
	buf_t p = {0};
	put(&p, "");
	char line[256];
	switch (j->kind) {
	case J_RESEARCH:
		builtin = "WebSearch,WebFetch";
		put(&p, "Research question: "); put(&p, S_(in, "question"));
		if (S_(in, "context")) { put(&p, "\n\nContext (why the teacher needs it): "); put(&p, S_(in, "context")); }
		break;
	case J_PDF:
		tools = PDF_TOOLS; nt = 4; turns = 14;
		put(&p, "PDF: "); put(&p, j->pdf); put(&p, "\n\nQuestion: "); put(&p, S_(in, "question"));
		break;
	case J_DEMO:
		tools = DEMO_TOOLS; nt = 2; turns = 30;
		put(&p, "Demo title: "); put(&p, S_(in, "title"));
		put(&p, "\nBrief (all you know about the lesson — this comes from the teacher, verbatim):\n"); put(&p, S_(in, "brief"));
		put(&p, "\n\nBuild the demo, test it with run_demo, iterate until PASSED, then finish with your notes.");
		break;
	case J_VISUAL:
		tools = VISUAL_TOOLS; nt = 3; turns = 20;
		put(&p, "Brief (all you know about the lesson — this comes from the teacher, verbatim):\n"); put(&p, S_(in, "brief"));
		if (j->image[0]) {
			snprintf(line, sizeof line, "\n\nThe learner's image: %d×%d pixels — place it with <image href=\"learner-image\" … width=\"%d\" height=\"%d\"/>. It is attached to your first render.", j->iw, j->ih, j->iw, j->ih);
			put(&p, line);
		}
		break;
	}
	const char *pf = agent_prompt_file(ak);
	claude_job_t cj = { .prompt_file = pf, .input = p.p, .errname = "job", .tools = tools, .ntools = nt, .chan = j->chan,
		.background = 1, .max_turns = turns, .builtin = builtin, .on_event = on_job_event, .ctx = j, .pid = &j->pid };
	if (pf) claude_run(&cj);
	free(p.p);
	const char *notes = j->final ? j->final : "";
	switch (j->kind) {
	case J_RESEARCH:
	case J_PDF:
		if (blank(notes)) { d->failed = 1; d->text = strdup(j->kind == J_RESEARCH ? "Research failed: the researcher returned no brief." : "PDF failed: the reader returned no brief."); }
		else d->text = strdup(notes);
		d->title = strdup(j->kind == J_PDF ? S_(in, "path") : S_(in, "question"));
		break;
	case J_DEMO: {
		d->title = strdup(S_(in, "title"));
		char path[1200];
		snprintf(path, sizeof path, "%s/demo.html", j->dir);
		int ok = 0;
		char *v = j->html ? chrome_test(path, NULL, &ok) : NULL; /* the final document, verified again */
		if (!j->html) { d->failed = 1; d->text = fmt("FAILED: the builder never wrote a demo.%s%s", blank(notes) ? "" : " ", notes); }
		else if (!ok) { d->failed = 1; d->text = fmt("FAILED: the demo never passed headless verification (%s)", v, NULL); }
		else { d->text = strdup(notes); d->artifact = strdup(path); }
		free(v);
		break;
	}
	case J_VISUAL: {
		d->title = strdup(j->image[0] ? "annotated" : "");
		char png[1200];
		snprintf(png, sizeof png, "%s/figure.png", j->dir);
		if (!j->svg) { d->failed = 1; d->text = fmt("FAILED: the maker never wrote an SVG.%s%s", blank(notes) ? "" : " ", notes); }
		else if (render(j)) { d->failed = 1; d->text = strdup("FAILED: the final SVG does not render."); }
		else {
			int looked = j->rendered && !strcmp(j->rendered, j->svg);
			d->text = fmt("%s%s", notes, looked ? "" : " (Note: the maker did not inspect a render of its final SVG.)");
			d->artifact = strdup(png);
		}
		break;
	}
	}
	pthread_mutex_lock(&mu);
	slot_busy[slot] = 0;
	pthread_cond_broadcast(&slot_cv);
	pthread_mutex_unlock(&mu);
	mcp_set_ctx(j->chan, NULL);
	deliver_later(j, d);
	return NULL;
}

/* Create the job record and its thread. */
static job_t *spawn(kind_t k, const cJSON *input) {
	job_t *j = calloc(1, sizeof *j);
	unsigned char r[5] = {0};
	FILE *f = fopen("/dev/urandom", "r");
	if (f) { if (fread(r, 1, 5, f) != 5) {} fclose(f); }
	snprintf(j->id, sizeof j->id, "j_%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4]);
	j->kind = k;
	j->input = cJSON_Duplicate(input, 1);
	char rel[64];
	snprintf(rel, sizeof rel, "jobs/%s/job.json", j->id);
	store_path(j->dir, sizeof j->dir, rel);
	cJSON *rec = cJSON_CreateObject();
	cJSON_AddStringToObject(rec, "id", j->id);
	cJSON_AddStringToObject(rec, "kind", KIND[k]);
	cJSON_AddStringToObject(rec, "chat", S->id);
	cJSON_AddItemToObject(rec, "input", cJSON_Duplicate(input, 1));
	store_write_json(j->dir, rec);
	cJSON_Delete(rec);
	*strrchr(j->dir, '/') = 0;
	return j;
}

static char *start(job_t *j, const char *ack) {
	pthread_mutex_lock(&mu);
	if (nrunning < 64) running[nrunning++] = j;
	pthread_mutex_unlock(&mu);
	pthread_t t;
	pthread_create(&t, NULL, job_main, j);
	pthread_detach(t);
	char head[96];
	snprintf(head, sizeof head, "Dispatched (job %s, %s). ", j->id, KIND[j->kind]);
	return fmt("%s%s", head, ack);
}

/* ── the dispatching tools ─────────────────────────────────────────────── */

#define FIRE "This call is fire-and-forget: continue your turn and keep teaching — do not wait, poll, or mention waiting. "

static char *t_research(const cJSON *a, int *is_error) {
	if (blank(S_(a, "question"))) return err(is_error, "research rejected: `question` is required — one specific question.%s", NULL);
	dim("research: ", S_(a, "question"));
	return start(spawn(J_RESEARCH, a), FIRE "The researched brief arrives as a new message when ready and the lesson resumes from it; cite its sources when you use them.");
}

static char *t_demo(const cJSON *a, int *is_error) {
	if (blank(S_(a, "title")) || blank(S_(a, "brief"))) return err(is_error, "demo rejected: `title` and `brief` are both required.%s", NULL);
	const char *why = chrome_unavailable();
	if (why) return err(is_error, "demo unavailable: %s. Teach it in prose instead.", why);
	dim("building demo: ", S_(a, "title"));
	return start(spawn(J_DEMO, a), FIRE "The tested demo arrives as a new message when ready and is opened for the learner; teach with it then.");
}

static char *t_visual(const cJSON *a, int *is_error) {
	if (blank(S_(a, "brief"))) return err(is_error, "visual rejected: `brief` is required — exactly what the figure must show.%s", NULL);
	char path[1100] = "";
	int w = 0, h = 0;
	if (S_(a, "image")) {
		if (resolve(S_(a, "image"), path, sizeof path)) return err(is_error, "visual rejected: image \"%s\" is not a readable file — check the path (relative to where the learner launched dio).", S_(a, "image"));
		if (!img_is_image_path(path) || img_size(path, &w, &h)) return err(is_error, "visual rejected: \"%s\" is not an image (jpg, png, gif, webp, bmp).", S_(a, "image"));
	}
	job_t *j = spawn(J_VISUAL, a);
	if (path[0]) {
		snprintf(j->image, sizeof j->image, "%s/learner.png", j->dir);
		img_to_png(path, j->image, 100000);
		img_size(j->image, &j->iw, &j->ih);
	}
	dim(path[0] ? "annotating image: " : "drawing figure: ", S_(a, "brief"));
	return start(j, FIRE "The verified figure arrives as a new message when ready and is shown in the lesson; reference it then.");
}

static char *t_pdf(const cJSON *a, int *is_error) {
	char path[1100];
	if (blank(S_(a, "question"))) return err(is_error, "pdf rejected: `question` is required — what you need from the document.%s", NULL);
	if (resolve(S_(a, "path"), path, sizeof path)) return err(is_error, "pdf rejected: \"%s\" is not a readable file — check the path (relative to where the learner launched dio).", S_(a, "path"));
	if (page_count(path) <= 0) return err(is_error, "pdf rejected: \"%s\" is not a readable PDF.", S_(a, "path"));
	job_t *j = spawn(J_PDF, a);
	snprintf(j->pdf, sizeof j->pdf, "%s", path);
	dim("reading pdf: ", S_(a, "path"));
	return start(j, FIRE "The document brief arrives as a new message when ready; weave it into the lesson and cite its pages.");
}

static char *t_show_image(const cJSON *a, int *is_error) {
	char path[1100];
	if (resolve(S_(a, "path"), path, sizeof path)) return err(is_error, "show_image rejected: \"%s\" is not a readable file — check the path.", S_(a, "path"));
	if (!img_is_image_path(path)) return err(is_error, "show_image rejected: \"%s\" is not an image (jpg, png, gif, webp, bmp).", S_(a, "path"));
	term_lock();
	img_show(path, S_(a, "caption"));
	term_unlock();
	return fmt("Shown to the learner: %s%s", S_(a, "path"), S_(a, "caption") ? " (with caption)" : "");
}

#define T_RESEARCH { "research", \
	"Dispatch a background web-research agent: fact verification, current data, or niche specifics you are not certain about. The call is ASYNC and fire-and-forget — " \
	"it resolves immediately and your turn continues; the research brief arrives later as a new message and the lesson resumes from it. Cite the brief's sources when you " \
	"use them. NEVER use it for things you know cold, internal reasoning, or math.", \
	"{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\",\"description\":\"The single research question to answer. Make it specific — the researcher sees nothing else of the lesson.\"}," \
	"\"context\":{\"type\":\"string\",\"description\":\"Why you need this / what the lesson needs it for — focuses the brief. One or two sentences.\"}},\"required\":[\"question\"]}", t_research }
#define T_PDF { "pdf", \
	"Dispatch a background sub-agent that READS a learner-provided PDF on your behalf (triage, targeted extraction, visual page reading for equations and diagrams) and " \
	"returns a brief with page citations. Use it when the learner gives you a PDF path and you need its CONTENT. ASYNC and fire-and-forget — the brief arrives later as a " \
	"new message. For quick looks at plain text or image files, use Read directly instead.", \
	"{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to the learner's PDF (relative or absolute), exactly as they gave it.\"}," \
	"\"question\":{\"type\":\"string\",\"description\":\"What you need from the document — the specific content, explanation, or answer to extract and brief. The reader sees NOTHING else of the lesson.\"}},\"required\":[\"path\",\"question\"]}", t_pdf }

static const tool_t TEACHER_JOBS[] = {
	T_RESEARCH,
	{ "demo",
	  "Dispatch a background sub-agent that BUILDS and headlessly TESTS an interactive teaching instrument — a single-file HTML demo (parameter sweep, simulation, animation, "
	  "sampling) opened for the learner in a browser. Use it when the concept is DYNAMIC. ASYNC and fire-and-forget — the tested demo arrives later as a new message. "
	  "Write a precise brief: concept, controls, interactions, style.",
	  "{\"type\":\"object\",\"properties\":{\"title\":{\"type\":\"string\",\"description\":\"Short title for the demo card, e.g. 'Damped harmonic oscillator'.\"},"
	  "\"brief\":{\"type\":\"string\",\"description\":\"Precise brief for the builder: the concept, the parameters/controls and what each does, the interactions, and the visual style. The builder sees NOTHING else of the lesson — include everything it needs, including necessary formulas and constants.\"}},\"required\":[\"title\",\"brief\"]}",
	  t_demo },
	{ "visual",
	  "Dispatch a background sub-agent that hand-authors ONE precise SVG figure, renders it, LOOKS at the render, and iterates until correct. Use it for exact spatial pictures "
	  "auto-layout diagrams can't do: coordinate geometry, number lines, vectors, function plots, physical layouts. For flows/relations/taxonomies write mermaid inline instead. "
	  "Pass `image` (a learner image file) to have the maker annotate that picture instead. ASYNC and fire-and-forget — the verified figure arrives later as a new message.",
	  "{\"type\":\"object\",\"properties\":{\"brief\":{\"type\":\"string\",\"description\":\"Precise brief for ONE static figure: exactly what it must show — coordinates, labels, proportions, orientation. The maker sees NOTHING else of the lesson. With an image: which features to mark and what each label says.\"},"
	  "\"image\":{\"type\":\"string\",\"description\":\"Path to one of the learner's image files (jpg, png, gif, webp, bmp) to annotate.\"}},\"required\":[\"brief\"]}",
	  t_visual },
	T_PDF,
	{ "show_image", "Put one of the learner's local image files into the lesson as-is, instantly, with an optional caption. Its result only confirms what was shown — to see the picture yourself, Read it.",
	  "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to the image (relative to where the learner launched dio, or absolute).\"},\"caption\":{\"type\":\"string\"}},\"required\":[\"path\"]}",
	  t_show_image },
};
static const tool_t PRACTICE_JOBS[] = { T_RESEARCH, T_PDF };

const tool_t *jobs_teacher_tools(int *n) { *n = (int)(sizeof TEACHER_JOBS / sizeof TEACHER_JOBS[0]); return TEACHER_JOBS; }
const tool_t *jobs_practice_tools(int *n) { *n = 2; return PRACTICE_JOBS; }

/* ── delivery (main thread) ────────────────────────────────────────────── */

int jobs_pending(void) {
	pthread_mutex_lock(&mu);
	int p = done_head != NULL;
	pthread_mutex_unlock(&mu);
	return p;
}

static void append_job_message(const char *id, kind_t k, const char *body) {
	char head[64];
	snprintf(head, sizeof head, "[job %s · %s] ", id, KIND[k]);
	char *msg = fmt("%s%s", head, body);
	session_append_user_text(S, msg);
	free(msg);
}

static void show(const delivery_t *d) {
	if (d->failed) { printf("\033[2m· %s job failed\033[0m\n", KIND[d->kind]); return; }
	if (d->kind == J_VISUAL) img_show(d->artifact, "figure");
	else if (d->kind == J_DEMO) {
		printf("\033[1m▸ demo:\033[0m %s \033[2m— %s\033[0m\n", d->title, d->artifact);
		if (!getenv("DIOTIMA_NO_OPEN")) { char *argv[] = { "open", d->artifact, NULL }; proc_run(argv, NULL, 0); }
	} else printf("\033[2m· %s ready\033[0m\n", KIND[d->kind]);
	fflush(stdout);
}

int jobs_deliver(void) {
	pthread_mutex_lock(&mu);
	delivery_t *d = done_head;
	done_head = done_tail = NULL;
	pthread_mutex_unlock(&mu);
	int n = 0;
	term_lock();
	while (d) {
		delivery_t *next = d->next;
		char *body = steer(d, d->text);
		/* failures already carry their reason in steer(); successes carry notes */
		append_job_message(d->id, d->kind, body);
		show(d);
		free(body); free(d->text); free(d->artifact); free(d->title); free(d);
		d = next;
		n++;
	}
	term_unlock();
	return n;
}

/* Dispatched job ids (from "Dispatched (job <id>, <kind>)" results) not yet delivered. */
static int outstanding(char ids[][24], kind_t *kinds, int max) {
	int n = 0;
	const cJSON *m, *b;
	pthread_mutex_lock(&S->mu);
	cJSON_ArrayForEach(m, S->messages)
		cJSON_ArrayForEach(b, cJSON_GetObjectItem(m, "content")) {
			const char *t = S_(b, "text"), *type = S_(b, "type");
			if (!t || !type) continue;
			char id[24], kind[16];
			if (!strcmp(type, "tool_result") && sscanf(t, "Dispatched (job %23[^,], %15[^)])", id, kind) == 2 && n < max) {
				for (int k = 0; k < 4; k++) if (!strcmp(kind, KIND[k])) { snprintf(ids[n], 24, "%s", id); kinds[n++] = (kind_t)k; }
			} else if (!strcmp(type, "text") && sscanf(t, "[job %23s ", id) == 1) {
				for (int i = 0; i < n; i++) if (!strcmp(ids[i], id)) { memmove(ids + i, ids + i + 1, (size_t)(n - i - 1) * 24); memmove(kinds + i, kinds + i + 1, (size_t)(n - i - 1) * sizeof *kinds); n--; break; }
			}
		}
	pthread_mutex_unlock(&S->mu);
	return n;
}

static void abort_outstanding(void) {
	char ids[64][24];
	kind_t kinds[64];
	int n = outstanding(ids, kinds, 64);
	for (int i = 0; i < n; i++) append_job_message(ids[i], kinds[i], abort_text(kinds[i]));
	if (n) { printf("\033[2m· %d job%s aborted\033[0m\n", n, n == 1 ? "" : "s"); fflush(stdout); }
}

void jobs_abort_all(void) {
	pthread_mutex_lock(&mu);
	for (int i = 0; i < nrunning; i++) { running[i]->aborted = 1; if (running[i]->pid > 0) kill(running[i]->pid, SIGTERM); }
	pthread_cond_broadcast(&slot_cv);
	pthread_mutex_unlock(&mu);
	jobs_deliver();
	abort_outstanding();
}

void jobs_recover(void) { abort_outstanding(); }

const char *jobs_latest_demo(void) {
	static char path[1200];
	const char *found = NULL;
	const cJSON *m, *b;
	pthread_mutex_lock(&S->mu);
	cJSON_ArrayForEach(m, S->messages)
		cJSON_ArrayForEach(b, cJSON_GetObjectItem(m, "content")) {
			const char *t = S_(b, "text");
			char id[24];
			if (t && sscanf(t, "[job %23s · demo] The demo", id) == 1 && strstr(t, "is built")) {
				char rel[64];
				snprintf(rel, sizeof rel, "jobs/%s/demo.html", id);
				store_path(path, sizeof path, rel);
				found = path;
			}
		}
	pthread_mutex_unlock(&S->mu);
	return found;
}
