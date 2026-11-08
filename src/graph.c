#include "math.h"
#include "graph.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "store.h"

static const char *S(const cJSON *o, const char *k) {
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}
static int in_arr(const cJSON *arr, const char *s) {
	const cJSON *v;
	cJSON_ArrayForEach(v, arr) if (cJSON_IsString(v) && !strcmp(v->valuestring, s)) return 1;
	return 0;
}
static int same_point(const cJSON *a, const cJSON *b) {
	return !strcmp(S(a, "chatId") ?: "", S(b, "chatId") ?: "") && !strcmp(S(a, "messageId") ?: "", S(b, "messageId") ?: "");
}
static void union_points(cJSON *into, const cJSON *from) {
	const cJSON *p, *q;
	cJSON_ArrayForEach(p, from) {
		int have = 0;
		cJSON_ArrayForEach(q, into) if (same_point(p, q)) have = 1;
		if (!have) cJSON_AddItemToArray(into, cJSON_Duplicate(p, 1));
	}
}
static cJSON *find_id(const cJSON *arr, const char *id) {
	cJSON *n;
	cJSON_ArrayForEach(n, arr) if (S(n, "id") && !strcmp(S(n, "id"), id)) return n;
	return NULL;
}
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Merge key: sorted premises joined by '+', then '>' conclusion. */
static char *chain_key(const cJSON *premises, const char *conclusion) {
	int n = cJSON_GetArraySize(premises);
	const char **p = calloc((size_t)n + 1, sizeof *p);
	size_t len = strlen(conclusion) + 2;
	for (int i = 0; i < n; i++) { p[i] = cJSON_GetArrayItem(premises, i)->valuestring; len += strlen(p[i]) + 1; }
	qsort(p, (size_t)n, sizeof *p, cmpstr);
	char *k = malloc(len);
	k[0] = 0;
	for (int i = 0; i < n; i++) { if (i) strcat(k, "+"); strcat(k, p[i]); }
	strcat(k, ">");
	strcat(k, conclusion);
	free(p);
	return k;
}

static void path(char *out, size_t n, const char *name) {
	char rel[64];
	snprintf(rel, sizeof rel, "graph/%s", name);
	store_path(out, n, rel);
}

/* ── reads ─────────────────────────────────────────────────────────────── */

graph_t graph_fold(const cJSON *node_lines, const cJSON *chain_lines) {
	graph_t g = { cJSON_CreateArray(), cJSON_CreateArray() };
	const cJSON *l;
	cJSON_ArrayForEach(l, node_lines) {
		if (!S(l, "id")) continue;
		cJSON *have = find_id(g.nodes, S(l, "id"));
		if (!have) {
			cJSON *n = cJSON_Duplicate(l, 1);
			if (!cJSON_IsArray(cJSON_GetObjectItem(n, "aliases"))) { cJSON_DeleteItemFromObject(n, "aliases"); cJSON_AddArrayToObject(n, "aliases"); }
			if (!cJSON_IsArray(cJSON_GetObjectItem(n, "provenance"))) { cJSON_DeleteItemFromObject(n, "provenance"); cJSON_AddArrayToObject(n, "provenance"); }
			cJSON_AddItemToArray(g.nodes, n);
			continue;
		}
		cJSON *aliases = cJSON_GetObjectItem(have, "aliases");
		const char *st = S(l, "statement");
		if (st && *st && !in_arr(aliases, st)) cJSON_AddItemToArray(aliases, cJSON_CreateString(st));
		const cJSON *a;
		cJSON_ArrayForEach(a, cJSON_GetObjectItem(l, "aliases"))
			if (cJSON_IsString(a) && *a->valuestring && !in_arr(aliases, a->valuestring)) cJSON_AddItemToArray(aliases, cJSON_CreateString(a->valuestring));
		union_points(cJSON_GetObjectItem(have, "provenance"), cJSON_GetObjectItem(l, "provenance"));
	}
	cJSON *keys = cJSON_CreateArray();
	cJSON_ArrayForEach(l, chain_lines) {
		if (!S(l, "conclusion")) continue;
		char *k = chain_key(cJSON_GetObjectItem(l, "premises"), S(l, "conclusion"));
		int i = 0, at = -1;
		const cJSON *kk;
		cJSON_ArrayForEach(kk, keys) { if (!strcmp(kk->valuestring, k)) at = i; i++; }
		if (at >= 0) union_points(cJSON_GetObjectItem(cJSON_GetArrayItem(g.chains, at), "provenance"), cJSON_GetObjectItem(l, "provenance"));
		else { cJSON_AddItemToArray(g.chains, cJSON_Duplicate(l, 1)); cJSON_AddItemToArray(keys, cJSON_CreateString(k)); }
		free(k);
	}
	cJSON_Delete(keys);
	return g;
}

static cJSON *read_ledger(const char *name) {
	char p[1100];
	path(p, sizeof p, name);
	return store_read_jsonl(p);
}

graph_t graph_load(void) {
	cJSON *n = read_ledger("nodes.jsonl"), *c = read_ledger("chains.jsonl");
	graph_t g = graph_fold(n, c);
	cJSON_Delete(n); cJSON_Delete(c);
	return g;
}

cJSON *graph_coverage(void) { return read_ledger("coverage.jsonl"); }

void graph_free(graph_t *g) { cJSON_Delete(g->nodes); cJSON_Delete(g->chains); g->nodes = g->chains = NULL; }

/* ── simplification ────────────────────────────────────────────────────── */

typedef struct { const char **s; int n, cap; } strtab;
static int intern(strtab *t, const char *s) {
	for (int i = 0; i < t->n; i++) if (!strcmp(t->s[i], s)) return i;
	if (t->n == t->cap) t->s = realloc(t->s, sizeof *t->s * (size_t)(t->cap = t->cap ? t->cap * 2 : 64));
	t->s[t->n] = s;
	return t->n++;
}

typedef struct { int nids, nlines; int *concl, **prem, *nprem; char *memo, *done, *visiting; } anc_t;

/* Ancestry bitset of id (every premise reachable behind it), memoized, cycle-guarded. */
static const char *ancestry(anc_t *a, int id) {
	char *out = a->memo + (size_t)id * (size_t)a->nids;
	if (a->done[id] || a->visiting[id]) return out;
	a->visiting[id] = 1;
	for (int l = 0; l < a->nlines; l++) {
		if (a->concl[l] != id) continue;
		for (int k = 0; k < a->nprem[l]; k++) {
			int p = a->prem[l][k];
			out[p] = 1;
			const char *up = ancestry(a, p);
			for (int x = 0; x < a->nids; x++) if (up[x]) out[x] = 1;
		}
	}
	a->visiting[id] = 0;
	a->done[id] = 1;
	return out;
}

typedef struct { int line; char *key; int np; const char *created; } cand_t;
static int cand_cmp(const void *x, const void *y) {
	const cand_t *a = x, *b = y;
	if (a->np != b->np) return a->np - b->np;
	int c = strcmp(a->created ?: "", b->created ?: "");
	return c ? c : strcmp(a->key, b->key);
}

cJSON *graph_simplify(const cJSON *lines) {
	int nl = cJSON_GetArraySize(lines);
	if (nl < 2) return cJSON_Duplicate(lines, 1);
	strtab ids = {0};
	anc_t a = { .nlines = nl };
	a.concl = calloc((size_t)nl, sizeof(int)); a.nprem = calloc((size_t)nl, sizeof(int)); a.prem = calloc((size_t)nl, sizeof(int *));
	char **keys = calloc((size_t)nl, sizeof(char *));
	for (int l = 0; l < nl; l++) {
		const cJSON *ln = cJSON_GetArrayItem(lines, l), *pr = cJSON_GetObjectItem(ln, "premises");
		a.concl[l] = intern(&ids, S(ln, "conclusion"));
		a.nprem[l] = cJSON_GetArraySize(pr);
		a.prem[l] = calloc((size_t)a.nprem[l] + 1, sizeof(int));
		for (int k = 0; k < a.nprem[l]; k++) a.prem[l][k] = intern(&ids, cJSON_GetArrayItem(pr, k)->valuestring);
		keys[l] = chain_key(pr, S(ln, "conclusion"));
	}
	a.nids = ids.n;
	a.memo = calloc((size_t)a.nids * (size_t)a.nids, 1); a.done = calloc((size_t)a.nids, 1); a.visiting = calloc((size_t)a.nids, 1);
	char *drop = calloc((size_t)nl, 1), *covered = malloc((size_t)a.nids), *cset = malloc((size_t)a.nids);
	cand_t *c = calloc((size_t)nl, sizeof *c);
	int *kept = calloc((size_t)nl, sizeof(int));
	for (int id = 0; id < a.nids; id++) {
		int nc = 0;
		for (int l = 0; l < nl; l++) {
			if (a.concl[l] != id) continue;
			int dup = 0; /* one candidate per key: its first line represents it */
			for (int j = 0; j < nc; j++) if (!strcmp(c[j].key, keys[l])) dup = 1;
			if (!dup) c[nc++] = (cand_t){ l, keys[l], a.nprem[l], S(cJSON_GetArrayItem(lines, l), "createdAt") };
		}
		qsort(c, (size_t)nc, sizeof *c, cand_cmp);
		int nk = 0;
		for (int j = 0; j < nc; j++) {
			int cl = c[j].line, redundant = 0;
			memset(cset, 0, (size_t)a.nids);
			for (int k = 0; k < a.nprem[cl]; k++) cset[a.prem[cl][k]] = 1;
			for (int q = 0; q < nk && !redundant; q++) {
				int kl = kept[q], subset = 1;
				for (int k = 0; k < a.nprem[kl]; k++) if (!cset[a.prem[kl][k]]) subset = 0;
				if (subset) { redundant = 1; break; }
				memset(covered, 0, (size_t)a.nids);
				for (int k = 0; k < a.nprem[kl]; k++) {
					int p = a.prem[kl][k];
					covered[p] = 1;
					const char *up = ancestry(&a, p);
					for (int x = 0; x < a.nids; x++) if (up[x]) covered[x] = 1;
				}
				int all = 1;
				for (int k = 0; k < a.nprem[cl]; k++) if (!covered[a.prem[cl][k]]) all = 0;
				redundant = all;
			}
			if (redundant) { for (int l = 0; l < nl; l++) if (!strcmp(keys[l], c[j].key)) drop[l] = 1; }
			else kept[nk++] = cl;
		}
	}
	cJSON *out = cJSON_CreateArray();
	for (int l = 0; l < nl; l++) if (!drop[l]) cJSON_AddItemToArray(out, cJSON_Duplicate(cJSON_GetArrayItem(lines, l), 1));
	for (int l = 0; l < nl; l++) { free(a.prem[l]); free(keys[l]); }
	free(a.prem); free(a.concl); free(a.nprem); free(keys); free(a.memo); free(a.done); free(a.visiting);
	free(drop); free(covered); free(cset); free(c); free(kept); free(ids.s);
	return out;
}

/* ── the same-source digest ────────────────────────────────────────────── */

typedef struct { char *p; size_t n, cap; } buf_t;
static void put(buf_t *b, const char *s) {
	size_t l = strlen(s);
	if (b->n + l + 1 > b->cap) { b->cap = (b->n + l + 1) * 2; b->p = realloc(b->p, b->cap); }
	memcpy(b->p + b->n, s, l + 1);
	b->n += l;
}
static void put_flat(buf_t *b, const char *s) { /* whitespace runs → one space */
	char tmp[2];
	tmp[1] = 0;
	int sp = 0;
	for (; s && *s; s++) {
		if (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r') { if (!sp) put(b, " "); sp = 1; continue; }
		sp = 0; tmp[0] = *s; put(b, tmp);
	}
}
static int from_source(const cJSON *o, const char *src) {
	const cJSON *p;
	cJSON_ArrayForEach(p, cJSON_GetObjectItem(o, "provenance")) if (S(p, "chatId") && !strcmp(S(p, "chatId"), src)) return 1;
	return 0;
}
static const char *name_of(const graph_t *g, const char *id) { const cJSON *n = find_id(g->nodes, id); return n && S(n, "name") ? S(n, "name") : id; }

char *graph_digest(const graph_t *g, const char *src) {
	buf_t b = {0};
	put(&b, "");
	int any = 0;
	const cJSON *n, *c, *p;
	cJSON_ArrayForEach(n, g->nodes) if (from_source(n, src)) any = 1;
	cJSON_ArrayForEach(c, g->chains) if (from_source(c, src)) any = 1;
	if (!any) return b.p;
	put(&b, "THE GRAPH SO FAR — everything already extracted from this source (reference these ids verbatim):\n");
	cJSON_ArrayForEach(n, g->nodes) {
		if (!from_source(n, src)) continue;
		put(&b, "  "); put(&b, S(n, "id")); put(&b, " | "); put(&b, S(n, "name") ?: ""); put(&b, " | "); put_flat(&b, S(n, "statement")); put(&b, "\n");
	}
	int head = 0;
	cJSON_ArrayForEach(c, g->chains) {
		if (!from_source(c, src)) continue;
		if (!head++) put(&b, "DERIVATIONS ALREADY RECORDED (build on these; do not re-derive them):\n");
		int k = 0;
		put(&b, "  [");
		cJSON_ArrayForEach(p, cJSON_GetObjectItem(c, "premises")) { if (k++) put(&b, " + "); put(&b, p->valuestring); }
		put(&b, " ⇒ "); put(&b, S(c, "conclusion")); put(&b, "]  ");
		k = 0;
		cJSON_ArrayForEach(p, cJSON_GetObjectItem(c, "premises")) { if (k++) put(&b, " ∧ "); put(&b, name_of(g, p->valuestring)); }
		put(&b, " ⇒ "); put(&b, name_of(g, S(c, "conclusion"))); put(&b, "\n");
	}
	if (b.n && b.p[b.n - 1] == '\n') b.p[--b.n] = 0;
	return b.p;
}

/* ── candidate retrieval (BM25; ranks, never decides identity) ─────────── */

static const char *STOP = " a an the and or of to in on at by for from with as is are was were be been being "
	"it its this that these those which who whose their them they we our you your "
	"can could may might must will would should has have had does do did "
	"such than then when where each every any all also into through ";

/* Lowercased word list (ASCII alnum + any UTF-8 byte), stop words removed. */
static cJSON *words(const char *s) {
	cJSON *out = cJSON_CreateArray();
	char w[256];
	int l = 0;
	for (const unsigned char *p = (const unsigned char *)(s ? s : ""); ; p++) {
		int c = *p, isw = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c >= 0x80;
		if (isw) { if (l < 250) w[l++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); continue; }
		if (l) {
			w[l] = 0;
			char pad[260];
			snprintf(pad, sizeof pad, " %s ", w);
			if (!strstr(STOP, pad)) cJSON_AddItemToArray(out, cJSON_CreateString(w));
			l = 0;
		}
		if (!c) break;
	}
	return out;
}
static int count_of(const cJSON *ws, const char *t) { int n = 0; const cJSON *w; cJSON_ArrayForEach(w, ws) if (!strcmp(w->valuestring, t)) n++; return n; }

typedef struct { const cJSON *node; cJSON *texts; cJSON *aliasOf; } ix_t; /* texts: arrays of words; aliasOf: alias string or null per text */
typedef struct { const cJSON *node; double score; const char *alias; } hit_t;
static int hit_cmp(const void *x, const void *y) {
	const hit_t *a = x, *b = y;
	if (a->score != b->score) return a->score < b->score ? 1 : -1;
	return strcmp(S(a->node, "id"), S(b->node, "id"));
}

static int join_text(char *out, size_t n, const char *a, const char *b) { return snprintf(out, n, "%s %s", a ? a : "", b ? b : ""); }

cJSON *graph_candidates(const cJSON *nodes, const cJSON *props, const cJSON *reqs, char *err, size_t errn) {
	const cJSON *r, *q;
	int nr = 0;
	cJSON_ArrayForEach(r, reqs) {
		const char *id = S(r, "id");
		const cJSON *off = cJSON_GetObjectItem(r, "offset");
		if (!id || !find_id(props, id)) { snprintf(err, errn, "Unknown proposition id \"%s\". Use an id from the proposed findings.", id ? id : ""); return NULL; }
		for (q = reqs->child; q != r; q = q->next) if (!strcmp(S(q, "id") ?: "", id)) { snprintf(err, errn, "Repeated proposition id \"%s\". Combine its hints in one request.", id); return NULL; }
		if (off && (!cJSON_IsNumber(off) || off->valuedouble < 0 || off->valuedouble != (int)off->valuedouble)) { snprintf(err, errn, "offset must be a nonnegative integer."); return NULL; }
		nr++;
	}
	int nn = cJSON_GetArraySize(nodes);
	ix_t *ix = calloc((size_t)nn + 1, sizeof *ix);
	cJSON *df = cJSON_CreateObject();
	double total = 0;
	int ntexts = 0, i = 0;
	const cJSON *n;
	static char tbuf[1 << 16];
	cJSON_ArrayForEach(n, nodes) {
		ix[i].node = n;
		ix[i].texts = cJSON_CreateArray();
		ix[i].aliasOf = cJSON_CreateArray();
		join_text(tbuf, sizeof tbuf, S(n, "name"), S(n, "statement"));
		cJSON_AddItemToArray(ix[i].texts, words(tbuf));
		cJSON_AddItemToArray(ix[i].aliasOf, cJSON_CreateNull());
		cJSON *seen = cJSON_CreateArray();
		const cJSON *a;
		cJSON_ArrayForEach(a, cJSON_GetObjectItem(n, "aliases")) {
			if (!cJSON_IsString(a) || in_arr(seen, a->valuestring) || (S(n, "statement") && !strcmp(a->valuestring, S(n, "statement")))) continue;
			cJSON_AddItemToArray(seen, cJSON_CreateString(a->valuestring));
			join_text(tbuf, sizeof tbuf, S(n, "name"), a->valuestring);
			cJSON_AddItemToArray(ix[i].texts, words(tbuf));
			cJSON_AddItemToArray(ix[i].aliasOf, cJSON_CreateString(a->valuestring));
		}
		cJSON_Delete(seen);
		cJSON *terms = cJSON_CreateArray();
		const cJSON *t, *w;
		cJSON_ArrayForEach(t, ix[i].texts) {
			total += cJSON_GetArraySize(t); ntexts++;
			cJSON_ArrayForEach(w, t) if (!in_arr(terms, w->valuestring)) cJSON_AddItemToArray(terms, cJSON_CreateString(w->valuestring));
		}
		cJSON_ArrayForEach(w, terms) {
			cJSON *d = cJSON_GetObjectItemCaseSensitive(df, w->valuestring);
			if (d) cJSON_SetNumberValue(d, d->valuedouble + 1); else cJSON_AddNumberToObject(df, w->valuestring, 1);
		}
		cJSON_Delete(terms);
		i++;
	}
	double avg = ntexts ? total / ntexts : 1;
	if (avg <= 0) avg = 1;
	cJSON *out = cJSON_CreateObject(), *matches = cJSON_AddArrayToObject(out, "matches"), *cands = cJSON_AddArrayToObject(out, "candidates");
	hit_t *hits = calloc((size_t)nn + 1, sizeof *hits);
	cJSON_ArrayForEach(r, reqs) {
		const cJSON *p = find_id(props, S(r, "id")), *a, *h;
		buf_t qb = {0};
		put(&qb, S(p, "name") ?: ""); put(&qb, " "); put(&qb, S(p, "statement") ?: "");
		cJSON_ArrayForEach(a, cJSON_GetObjectItem(p, "aliases")) if (cJSON_IsString(a)) { put(&qb, " "); put(&qb, a->valuestring); }
		cJSON_ArrayForEach(h, cJSON_GetObjectItem(r, "hints")) if (cJSON_IsString(h)) { put(&qb, " "); put(&qb, h->valuestring); }
		cJSON *qw = words(qb.p), *query = cJSON_CreateArray();
		const cJSON *w;
		cJSON_ArrayForEach(w, qw) if (!in_arr(query, w->valuestring)) cJSON_AddItemToArray(query, cJSON_CreateString(w->valuestring));
		cJSON_Delete(qw); free(qb.p);
		int nh = 0;
		for (int k = 0; k < nn; k++) {
			double best = 0;
			const char *alias = NULL;
			int ti = 0;
			const cJSON *t;
			cJSON_ArrayForEach(t, ix[k].texts) {
				double sc = 0;
				int len = cJSON_GetArraySize(t);
				cJSON_ArrayForEach(w, query) {
					int tf = count_of(t, w->valuestring);
					if (!tf) continue;
					double d = cJSON_GetObjectItemCaseSensitive(df, w->valuestring)->valuedouble;
					double idf = log(1 + (nn - d + 0.5) / (d + 0.5));
					sc += idf * (tf * 2.2) / (tf + 1.2 * (0.25 + 0.75 * len / avg));
				}
				if (sc > best) {
					best = sc;
					const cJSON *ao = cJSON_GetArrayItem(ix[k].aliasOf, ti);
					alias = NULL;
					if (cJSON_IsString(ao)) {
						cJSON *aw = words(ao->valuestring);
						const cJSON *x;
						cJSON_ArrayForEach(x, aw) if (in_arr(query, x->valuestring)) alias = ao->valuestring;
						cJSON_Delete(aw);
					}
				}
				ti++;
			}
			if (best > 0) hits[nh++] = (hit_t){ ix[k].node, best, alias };
		}
		cJSON_Delete(query);
		qsort(hits, (size_t)nh, sizeof *hits, hit_cmp);
		const cJSON *off = cJSON_GetObjectItem(r, "offset");
		int o = off ? off->valueint : 0, end = o + 5 < nh ? o + 5 : nh;
		cJSON *m = cJSON_CreateObject(), *ids = cJSON_CreateArray();
		cJSON_AddStringToObject(m, "propositionId", S(r, "id"));
		for (int k = o; k < end; k++) {
			const char *id = S(hits[k].node, "id");
			cJSON_AddItemToArray(ids, cJSON_CreateString(id));
			cJSON *c = find_id(cands, id);
			if (!c) {
				c = cJSON_CreateObject();
				cJSON_AddStringToObject(c, "id", id);
				cJSON_AddStringToObject(c, "name", S(hits[k].node, "name") ?: "");
				cJSON_AddStringToObject(c, "statement", S(hits[k].node, "statement") ?: "");
				cJSON_AddArrayToObject(c, "matchedAliases");
				cJSON_AddItemToArray(cands, c);
			}
			cJSON *ma = cJSON_GetObjectItem(c, "matchedAliases");
			if (hits[k].alias && !in_arr(ma, hits[k].alias)) cJSON_AddItemToArray(ma, cJSON_CreateString(hits[k].alias));
		}
		cJSON_AddItemToObject(m, "candidateIds", ids);
		if (end < nh) cJSON_AddNumberToObject(m, "nextOffset", end); else cJSON_AddNullToObject(m, "nextOffset");
		cJSON_AddItemToArray(matches, m);
	}
	for (int k = 0; k < nn; k++) { cJSON_Delete(ix[k].texts); cJSON_Delete(ix[k].aliasOf); }
	free(ix); free(hits); cJSON_Delete(df);
	(void)nr;
	return out;
}

/* ── the merger ────────────────────────────────────────────────────────── */

static void new_id(char *out, size_t n, const char *prefix) {
	unsigned char r[6] = {0};
	FILE *f = fopen("/dev/urandom", "r");
	if (f) { if (fread(r, 1, 6, f) != 6) {} fclose(f); }
	snprintf(out, n, "%s%02x%02x%02x%02x%02x%02x", prefix, r[0], r[1], r[2], r[3], r[4], r[5]);
}

static void now_iso(char *out, size_t n) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	gmtime_r(&ts.tv_sec, &tm);
	char d[32];
	strftime(d, sizeof d, "%Y-%m-%dT%H:%M:%S", &tm);
	snprintf(out, n, "%s.%03ldZ", d, ts.tv_nsec / 1000000);
}

static cJSON *point(const char *chat, const char *mid) {
	cJSON *arr = cJSON_CreateArray(), *p = cJSON_CreateObject();
	cJSON_AddStringToObject(p, "chatId", chat);
	cJSON_AddStringToObject(p, "messageId", mid ? mid : "");
	cJSON_AddItemToArray(arr, p);
	return arr;
}

static void append(const char *ledger, const cJSON *row) {
	char p[1100], *line = cJSON_PrintUnformatted(row);
	path(p, sizeof p, ledger);
	store_append_line(p, line);
	free(line);
}

int graph_merge(const char *chat, const cJSON *findings, const cJSON *links, const slot_t *slots, int nslots, merge_stats *st) {
	memset(st, 0, sizeof *st);
	char now[40];
	now_iso(now, sizeof now);
	cJSON *lines = read_ledger("nodes.jsonl"), *existing = cJSON_CreateArray(), *map = cJSON_CreateObject();
	const cJSON *l, *p;
	cJSON_ArrayForEach(l, lines) if (S(l, "id") && !in_arr(existing, S(l, "id"))) cJSON_AddItemToArray(existing, cJSON_CreateString(S(l, "id")));
	cJSON_Delete(lines);
	/* 1. propositions: link-append onto an existing node, or create */
	cJSON_ArrayForEach(p, cJSON_GetObjectItem(findings, "propositions")) {
		const char *pid = S(p, "id");
		if (!pid || cJSON_GetObjectItemCaseSensitive(map, pid)) continue;
		const char *link = NULL;
		const cJSON *k;
		cJSON_ArrayForEach(k, links) if (S(k, "id") && !strcmp(S(k, "id"), pid)) link = S(k, "linkTo");
		char id[32];
		if (link && in_arr(existing, link)) { snprintf(id, sizeof id, "%s", link); st->linked++; }
		else { new_id(id, sizeof id, "n_"); st->nodes++; cJSON_AddItemToArray(existing, cJSON_CreateString(id)); }
		cJSON *row = cJSON_CreateObject();
		cJSON_AddStringToObject(row, "id", id);
		cJSON_AddStringToObject(row, "name", S(p, "name") ?: "");
		cJSON_AddStringToObject(row, "statement", S(p, "statement") ?: "");
		const cJSON *al = cJSON_GetObjectItem(p, "aliases");
		cJSON_AddItemToObject(row, "aliases", cJSON_IsArray(al) ? cJSON_Duplicate(al, 1) : cJSON_CreateArray());
		cJSON_AddStringToObject(row, "createdAt", now);
		cJSON_AddItemToObject(row, "provenance", point(chat, S(p, "messageId")));
		append("nodes.jsonl", row);
		cJSON_Delete(row);
		cJSON_AddStringToObject(map, pid, id);
	}
	/* 2. chains: endpoints resolve through this job's map or name an existing node */
	cJSON *chains = read_ledger("chains.jsonl"), *keyIds = cJSON_CreateObject(), *asserted = cJSON_CreateArray();
	cJSON_ArrayForEach(l, chains) {
		char *k = chain_key(cJSON_GetObjectItem(l, "premises"), S(l, "conclusion") ?: "");
		if (!cJSON_GetObjectItemCaseSensitive(keyIds, k)) cJSON_AddStringToObject(keyIds, k, S(l, "id") ?: "");
		if (from_source(l, chat) && !in_arr(asserted, k)) cJSON_AddItemToArray(asserted, cJSON_CreateString(k));
		free(k);
	}
	cJSON_Delete(chains);
	const cJSON *c;
	cJSON_ArrayForEach(c, cJSON_GetObjectItem(findings, "chains")) {
#define RESOLVE(e) (cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(map, e)) ?: (in_arr(existing, e) ? e : NULL))
		const char *concl = S(c, "conclusion") ? RESOLVE(S(c, "conclusion")) : NULL;
		if (!concl) continue;
		cJSON *prem = cJSON_CreateArray();
		const cJSON *x;
		cJSON_ArrayForEach(x, cJSON_GetObjectItem(c, "premises")) {
			const char *r = cJSON_IsString(x) ? RESOLVE(x->valuestring) : NULL;
			if (r && strcmp(r, concl) && !in_arr(prem, r)) cJSON_AddItemToArray(prem, cJSON_CreateString(r));
		}
#undef RESOLVE
		char *k = chain_key(prem, concl);
		if (cJSON_GetArraySize(prem) == 0 || in_arr(asserted, k)) { free(k); cJSON_Delete(prem); continue; }
		char id[32];
		const char *known = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(keyIds, k));
		if (known && *known) snprintf(id, sizeof id, "%s", known);
		else { new_id(id, sizeof id, "arg_"); cJSON_AddStringToObject(keyIds, k, id); }
		cJSON *row = cJSON_CreateObject();
		cJSON_AddStringToObject(row, "id", id);
		cJSON_AddItemToObject(row, "premises", prem);
		cJSON_AddStringToObject(row, "conclusion", concl);
		cJSON_AddStringToObject(row, "createdAt", now);
		cJSON_AddItemToObject(row, "provenance", point(chat, ""));
		append("chains.jsonl", row);
		cJSON_Delete(row);
		cJSON_AddItemToArray(asserted, cJSON_CreateString(k));
		free(k);
		st->chains++;
	}
	cJSON_Delete(keyIds); cJSON_Delete(asserted); cJSON_Delete(existing); cJSON_Delete(map);
	/* 2b. simplification over the whole chain ledger (an atomic rewrite) */
	chains = read_ledger("chains.jsonl");
	cJSON *kept = graph_simplify(chains);
	int rc = 0;
	if (cJSON_GetArraySize(kept) != cJSON_GetArraySize(chains)) {
		st->dropped = cJSON_GetArraySize(chains) - cJSON_GetArraySize(kept);
		buf_t b = {0};
		put(&b, "");
		cJSON_ArrayForEach(l, kept) { char *s = cJSON_PrintUnformatted(l); put(&b, s); put(&b, "\n"); free(s); }
		char pth[1100];
		path(pth, sizeof pth, "chains.jsonl");
		rc = store_write_atomic(pth, b.p);
		free(b.p);
	}
	cJSON_Delete(chains); cJSON_Delete(kept);
	/* 3. only now is the batch covered */
	for (int i = 0; i < nslots; i++) {
		cJSON *row = cJSON_CreateObject();
		cJSON_AddStringToObject(row, "chatId", chat);
		cJSON_AddStringToObject(row, "fp", slots[i].fp);
		cJSON_AddNumberToObject(row, "tokens", slots[i].tokens);
		cJSON_AddStringToObject(row, "foldedAt", now);
		append("coverage.jsonl", row);
		cJSON_Delete(row);
	}
	return rc;
}

/* ── the terminal view ─────────────────────────────────────────────────── */

static const char *birth(const cJSON *n) { return S(cJSON_GetArrayItem(cJSON_GetObjectItem(n, "provenance"), 0), "chatId"); }

static void session_title(const char *chat, char *out, size_t n) {
	char rel[128], p[1100];
	snprintf(rel, sizeof rel, "chats/%s.jsonl", chat);
	out[0] = 0;
	if (store_path(p, sizeof p, rel)) return;
	FILE *f = fopen(p, "r");
	if (!f) return;
	char *line = NULL;
	size_t cap = 0;
	if (getline(&line, &cap, f) > 0) {
		cJSON *m = cJSON_Parse(line);
		const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(m, "content"), 0), "text"));
		if (t) snprintf(out, n, "%.60s", t);
		cJSON_Delete(m);
	}
	free(line);
	fclose(f);
}

/* Display cells of a UTF-8 string, skipping ANSI escapes (every codepoint one
 * cell, combining accents from typeset math none). */
static int cells(const char *s) {
	int n = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == 033) { while (*p && *p != 'm') p++; if (!*p) break; continue; }
		if (*p == 0xCC || *p == 0xCD || (p[0] == 0xE2 && p[1] == 0x83)) continue; /* U+0300–036F, U+20C0–20FF */
		if ((*p & 0xC0) != 0x80) n++;
	}
	return n;
}

/* A proposition name or session title, its math shown as Unicode. */
static void put_math(buf_t *b, const char *s) {
	char *t = math_text(s);
	put(b, t);
	free(t);
}

/* One session color per index (256-color palette), cycling. */
static const int PALETTE[] = { 39, 42, 214, 203, 141, 44, 220, 205 };
#define NPAL (int)(sizeof PALETTE / sizeof PALETTE[0])

typedef struct { int color, width; const cJSON *sessions; char (*titles)[128]; } view_t;

static int session_index(const view_t *v, const char *sid) {
	int i = 0;
	const cJSON *x;
	cJSON_ArrayForEach(x, v->sessions) { if (!strcmp(x->valuestring, sid)) return i; i++; }
	return -1;
}

static void put_dot(buf_t *b, const view_t *v, int si) {
	char e[24];
	snprintf(e, sizeof e, "\033[38;5;%dm●\033[0m", PALETTE[si % NPAL]);
	put(b, v->color ? e : "●");
}

/* "A ∧ B ●" — premises from other sessions carry that session's mark. */
static void put_premises(buf_t *b, const graph_t *g, const view_t *v, const cJSON *chain, const char *here) {
	const cJSON *p;
	int k = 0;
	cJSON_ArrayForEach(p, cJSON_GetObjectItem(chain, "premises")) {
		if (k++) put(b, " ∧ ");
		put_math(b, name_of(g, p->valuestring));
		const cJSON *pn = find_id(g->nodes, p->valuestring);
		const char *pb = pn ? birth(pn) : NULL;
		if (!pb || !strcmp(pb, here)) continue;
		int si = session_index(v, pb);
		if (v->color) { put(b, " "); put_dot(b, v, si); }
		else {
			/* the session's title, cut at a word boundary */
			const char *full = v->titles[si][0] ? v->titles[si] : pb;
			char t[64];
			int cut = (int)strlen(full);
			if (cut > 22) { cut = 22; while (cut > 8 && full[cut] != ' ') cut--; }
			snprintf(t, sizeof t, " ↗ %.*s%s", cut, full, cut < (int)strlen(full) ? "…" : "");
			put(b, t);
		}
	}
}

char *graph_render(const graph_t *g, int color, int width) {
	buf_t b = {0};
	put(&b, "");
	cJSON *sessions = cJSON_CreateArray();
	const cJSON *n, *c;
	cJSON_ArrayForEach(n, g->nodes) { const char *s = birth(n); if (s && !in_arr(sessions, s)) cJSON_AddItemToArray(sessions, cJSON_CreateString(s)); }
	int ns = cJSON_GetArraySize(sessions);
	char (*titles)[128] = calloc((size_t)ns + 1, sizeof *titles);
	for (int i = 0; i < ns; i++) {
		session_title(cJSON_GetArrayItem(sessions, i)->valuestring, titles[i], sizeof titles[i]);
		char *t = math_text(titles[i]);
		snprintf(titles[i], sizeof titles[i], "%s", t);
		free(t);
	}
	view_t v = { color, width > 20 ? width : 100, sessions, titles };
	const char *B = color ? "\033[1m" : "", *D = color ? "\033[2m" : "", *R = color ? "\033[0m" : "";
	for (int si = 0; si < ns; si++) {
		const char *sid = cJSON_GetArrayItem(sessions, si)->valuestring;
		int ideas = 0, derivs = 0;
		cJSON_ArrayForEach(n, g->nodes) {
			if (!birth(n) || strcmp(birth(n), sid)) continue;
			ideas++;
			cJSON_ArrayForEach(c, g->chains) if (!strcmp(S(c, "conclusion") ?: "", S(n, "id"))) derivs++;
		}
		char head[512];
		if (b.n) put(&b, "\n");
		put_dot(&b, &v, si);
		snprintf(head, sizeof head, " %s%s%s  %s%d idea%s · %d derivation%s  (%s)%s\n", B, titles[si][0] ? titles[si] : sid, R,
			D, ideas, ideas == 1 ? "" : "s", derivs, derivs == 1 ? "" : "s", sid, R);
		put(&b, head);
		int seen = 0;
		cJSON_ArrayForEach(n, g->nodes) {
			if (!birth(n) || strcmp(birth(n), sid)) continue;
			int last = ++seen == ideas, nd = 0;
			const cJSON *only = NULL;
			cJSON_ArrayForEach(c, g->chains) if (!strcmp(S(c, "conclusion") ?: "", S(n, "id"))) { nd++; only = c; }
			buf_t line = {0};
			put(&line, last ? "  └─ " : "  ├─ ");
			put_math(&line, S(n, "name") ?: S(n, "id"));
			if (nd == 1) { /* one derivation: inline when it fits */
				buf_t try = {0};
				put(&try, line.p); put(&try, " "); put(&try, D); put(&try, "⇐"); put(&try, R); put(&try, " ");
				put_premises(&try, g, &v, only, sid);
				if (cells(try.p) <= v.width) { free(line.p); line = try; nd = 0; }
				else free(try.p);
			}
			put(&b, line.p); put(&b, "\n");
			free(line.p);
			if (!nd) continue;
			cJSON_ArrayForEach(c, g->chains) {
				if (strcmp(S(c, "conclusion") ?: "", S(n, "id"))) continue;
				put(&b, last ? "       " : "  │    ");
				put(&b, D); put(&b, "⇐"); put(&b, R); put(&b, " ");
				put_premises(&b, g, &v, c, sid);
				put(&b, "\n");
			}
		}
	}
	free(titles);
	cJSON_Delete(sessions);
	if (!b.n) put(&b, "The knowledge graph is empty.\n");
	return b.p;
}
