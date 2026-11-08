#include "agent.h"
#include <dirent.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "store.h"

#ifndef PROMPTS_DIR
#define PROMPTS_DIR "prompts"
#endif

typedef struct { char *p; size_t n; } sbuf;

static void add(sbuf *b, const char *s, size_t l) {
	b->p = realloc(b->p, b->n + l + 1);
	memcpy(b->p + b->n, s, l);
	b->n += l;
	b->p[b->n] = 0;
}

static void add_trimmed(sbuf *b, char *txt) {
	if (!txt) return;
	char *s = txt;
	while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
	size_t l = strlen(s);
	while (l && (s[l - 1] == ' ' || s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == '\t')) l--;
	if (!l) return;
	if (b->n) add(b, "\n\n---\n\n", 7);
	add(b, s, l);
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Fragments: ^\d+-[\w.-]+\.md$ sorted by filename. */
static void add_dir(sbuf *b, const char *dir) {
	DIR *d = opendir(dir);
	if (!d) return;
	regex_t re;
	regcomp(&re, "^[0-9]+-[A-Za-z0-9_.-]+\\.md$", REG_EXTENDED | REG_NOSUB);
	char *names[256];
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < 256) if (!regexec(&re, e->d_name, 0, NULL, 0)) names[n++] = strdup(e->d_name);
	closedir(d);
	regfree(&re);
	qsort(names, (size_t)n, sizeof *names, cmp);
	for (int i = 0; i < n; i++) {
		char path[1200];
		snprintf(path, sizeof path, "%s/%s", dir, names[i]);
		char *t = store_read_file(path);
		add_trimmed(b, t);
		free(t);
		free(names[i]);
	}
}

const char *agent_name(agent_kind_t k) {
	static const char *N[] = { "prober", "teacher", "practice", "extractor", "auditor",
		"researcher", "pdf-reader", "demo-builder", "visual-maker", "visual-annotator" };
	return N[k];
}

static const char *NOTE =
	"Your tools are exposed as mcp__diotima__<name>; call them by those names. Only the tools in your list exist — "
	"if a tool the instructions mention is absent, proceed without it.";

const char *agent_prompt_file(agent_kind_t k) {
	static char out[1200];
	sbuf b = {0};
	char path[1200];
	const char *root = getenv("DIOTIMA_PROMPTS");
	if (!root) root = PROMPTS_DIR;
	int stage = k >= AGENT_EXTRACTOR; /* sub-agents: their own instructions only */
	if (k != AGENT_PRACTICE && !stage) {
		snprintf(path, sizeof path, "%s/system-prompt.md", root);
		char *t = store_read_file(path);
		add_trimmed(&b, t);
		free(t);
	}
	snprintf(path, sizeof path, "%s/%s", root, k == AGENT_VISUAL_ANNOTATOR ? "visual-maker" : agent_name(k));
	add_dir(&b, path);
	if (k == AGENT_VISUAL_ANNOTATOR) { snprintf(path, sizeof path, "%s/visual-annotate", root); add_dir(&b, path); }
	snprintf(path, sizeof path, "%s/shared", root);
	if (!stage) add_dir(&b, path);
	add_trimmed(&b, strdup(NOTE));
	if (k == AGENT_RESEARCHER)
		add_trimmed(&b, strdup("Here web_search is the WebSearch tool and web_fetch is the WebFetch tool. web_fetch_pdf, pdf_extract and "
			"pdf_search are not available: read what WebFetch returns for a PDF URL, and cite pages only when it shows them."));
	if (!b.n) return NULL;
	char rel[64];
	snprintf(rel, sizeof rel, "tmp/prompt-%s.md", agent_name(k));
	if (store_path(out, sizeof out, rel) || store_write_atomic(out, b.p)) { free(b.p); return NULL; }
	free(b.p);
	return out;
}
