#include "img.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "mcp.h"
#include "proc.h"
#include "sha256.h"
#include "store.h"
#include "term.h"

int img_is_image_path(const char *p) {
	const char *e = p ? strrchr(p, '.') : NULL;
	static const char *const OK[] = { ".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", NULL };
	for (int i = 0; e && OK[i]; i++) if (!strcasecmp(e, OK[i])) return 1;
	return 0;
}

int img_size(const char *path, int *w, int *h) {
	char spec[1100], *out = NULL;
	snprintf(spec, sizeof spec, "%s[0]", path);
	char *argv[] = { "magick", "identify", "-format", "%w %h", spec, NULL };
	int rc = proc_run(argv, &out, 0);
	int ok = rc == 0 && out && sscanf(out, "%d %d", w, h) == 2;
	free(out);
	return ok ? 0 : -1;
}

int img_to_png(const char *src, const char *dst, int max) {
	char in[1100], fit[32], outp[1100];
	const char *e = strrchr(src, '.');
	if (e && !strcasecmp(e, ".svg")) { /* librsvg: browser-grade text and embedded images */
		char *rs[] = { "rsvg-convert", "-b", "white", "-o", (char *)dst, (char *)src, NULL };
		if (proc_run(rs, NULL, 0)) return -1;
		snprintf(fit, sizeof fit, "%dx%d>", max, max);
		char *mg[] = { "magick", (char *)dst, "-resize", fit, (char *)dst, NULL };
		return proc_run(mg, NULL, 0) == 0 ? 0 : -1;
	}
	snprintf(in, sizeof in, "%s[0]", src);
	snprintf(fit, sizeof fit, "%dx%d>", max, max);
	snprintf(outp, sizeof outp, "png:%s", dst);
	char *argv[] = { "magick", "-background", "white", in, "-flatten", "-resize", fit, outp, NULL };
	return proc_run(argv, NULL, 0) == 0 ? 0 : -1;
}

/* Send a PNG over the kitty graphics protocol, in 4096-byte base64 chunks. */
static int kitty_png(const char *png) {
	FILE *f = fopen(png, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	unsigned char *buf = malloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	char *b = mcp_base64(buf, got);
	free(buf);
	size_t bl = strlen(b);
	for (size_t off = 0; off < bl; off += 4096) {
		size_t c = bl - off < 4096 ? bl - off : 4096;
		printf("\033_G%s%s;%.*s\033\\", off ? "" : "a=T,f=100,", off + c < bl ? "m=1" : "m=0", (int)c, b + off);
	}
	free(b);
	printf("\n");
	return 0;
}

void img_show(const char *path, const char *caption) {
	if (term_has_images()) {
		char png[1100];
		store_path(png, sizeof png, "tmp/show.png");
		if (!img_to_png(path, png, 1200)) kitty_png(png);
	}
	printf("\033[2m  %s%s%s\033[0m\n", caption && *caption ? caption : "", caption && *caption ? " — " : "", path);
	fflush(stdout);
}

/* Display math: pdflatex → pdftoppm → trimmed PNG, cached by the formula's
 * hash under tmp/math. The resolution puts 12pt text at about one terminal
 * row, so typeset math sits at the size of the prose around it. */
int img_show_tex(const char *tex) {
	if (!term_has_images() || !tex) return -1;
	struct winsize ws;
	int cell = !ioctl(1, TIOCGWINSZ, &ws) && ws.ws_row && ws.ws_ypixel ? ws.ws_ypixel / ws.ws_row : 34;
	int dpi = cell * 6;
	char key[200], h[65], rel[120], png[1100];
	snprintf(key, sizeof key, "%d:", dpi);
	size_t kl = strlen(key), tl = strlen(tex);
	char *all = malloc(kl + tl + 1);
	memcpy(all, key, kl);
	memcpy(all + kl, tex, tl + 1);
	sha256_hex(all, kl + tl, h);
	free(all);
	snprintf(rel, sizeof rel, "tmp/math/%.16s.png", h);
	store_path(png, sizeof png, rel);
	if (access(png, R_OK)) {
		char dir[1100], src[1100], pdf[1100], stem[1100], r[16];
		snprintf(dir, sizeof dir, "%.*s", (int)(strrchr(png, '/') - png), png);
		snprintf(src, sizeof src, "%s/%.16s.tex", dir, h);
		snprintf(pdf, sizeof pdf, "%s/%.16s.pdf", dir, h);
		snprintf(stem, sizeof stem, "%s/%.16s", dir, h);
		/* aligned rows (& or \\) need an align environment; a \begin brings its own */
		const char *env = strstr(tex, "\\begin") ? NULL : strchr(tex, '&') || strstr(tex, "\\\\") ? "align*" : NULL;
		FILE *f = fopen(src, "w");
		if (!f) return -1;
		fprintf(f, "\\documentclass[12pt]{article}\n\\usepackage{amsmath,amssymb}\n\\pagestyle{empty}\n\\begin{document}\n");
		if (env) fprintf(f, "\\begin{%s}\n%s\n\\end{%s}\n", env, tex, env);
		else fprintf(f, "\\[\n%s\n\\]\n", tex);
		fprintf(f, "\\end{document}\n");
		fclose(f);
		char *tx[] = { "pdflatex", "-interaction=batchmode", "-halt-on-error", "-output-directory", dir, src, NULL };
		snprintf(r, sizeof r, "%d", dpi);
		char *pp[] = { "pdftoppm", "-png", "-singlefile", "-r", r, pdf, stem, NULL };
		char *mg[] = { "magick", png, "-trim", "+repage", "-bordercolor", "white", "-border", "12", png, NULL };
		int bad = proc_run(tx, NULL, 0) || proc_run(pp, NULL, 0) || proc_run(mg, NULL, 0);
		const char *ext[] = { "tex", "pdf", "aux", "log", NULL };
		for (int i = 0; ext[i]; i++) { char x[1100]; snprintf(x, sizeof x, "%s.%s", stem, ext[i]); unlink(x); }
		if (bad) { unlink(png); return -1; }
	}
	printf("  ");
	int rc = kitty_png(png);
	fflush(stdout);
	return rc;
}
