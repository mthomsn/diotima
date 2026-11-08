#include "math.h"
#include "plan.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *str(const cJSON *o, const char *k) {
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int blank(const char *s) {
	if (!s) return 1;
	for (; *s; s++) if (*s != ' ' && *s != '\t' && *s != '\n') return 0;
	return 1;
}

static int slug_ok(const char *s) {
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return 0;
	return 1;
}

static int index_of(const char **ids, int n, const char *id) {
	for (int i = 0; i < n; i++) if (!strcmp(ids[i], id)) return i;
	return -1;
}

/* Walk forward from `to`; reaching `from` means the new edge closes a loop. */
static int reaches(int n, const int *ef, const int *et, int ne, int start, int goal) {
	int *stack = malloc(sizeof(int) * (size_t)(ne + 2)), sp = 0;
	char *seen = calloc((size_t)n, 1);
	int found = 0;
	stack[sp++] = start;
	while (sp > 0 && !found) {
		int cur = stack[--sp];
		if (cur == goal) { found = 1; break; }
		if (seen[cur]) continue;
		seen[cur] = 1;
		for (int i = 0; i < ne; i++)
			if (ef[i] == cur && sp < ne + 1) stack[sp++] = et[i];
	}
	free(stack);
	free(seen);
	return found;
}

int plan_validate(const cJSON *nodes, const cJSON *edges, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); return -1; } while (0)
	if (!cJSON_IsArray(nodes) || !cJSON_IsArray(edges)) FAIL("`nodes` and `edges` must both be arrays.");
	int n = cJSON_GetArraySize(nodes), ne = cJSON_GetArraySize(edges), k = 0;
	const char **ids = calloc((size_t)n + 1, sizeof *ids);
	const cJSON *nd;
	cJSON_ArrayForEach(nd, nodes) {
		const char *id = str(nd, "id"), *title = str(nd, "title");
		if (blank(id)) { free(ids); FAIL("Every node needs a non-empty `id` (a stable slug like 'chain-rule-intuition')."); }
		if (!slug_ok(id)) { free(ids); FAIL("Node id \"%s\" must be a lowercase slug (a-z, 0-9, hyphens/underscores).", id); }
		if (blank(title)) { free(ids); FAIL("Node \"%s\" needs a non-empty `title`.", id); }
		if (index_of(ids, k, id) >= 0) { free(ids); FAIL("Duplicate node id \"%s\" — node ids must be unique.", id); }
		ids[k++] = id;
	}
	int *ef = calloc((size_t)ne + 1, sizeof(int)), *et = calloc((size_t)ne + 1, sizeof(int)), acc = 0, rc = 0;
	const cJSON *e;
	cJSON_ArrayForEach(e, edges) {
		const char *f = str(e, "from"), *t = str(e, "to");
		int fi = f ? index_of(ids, k, f) : -1, ti = t ? index_of(ids, k, t) : -1;
		if (fi < 0) { snprintf(err, errn, "Edge `from` references unknown node id \"%s\" — it is not in the tree.", f ? f : ""); rc = -1; break; }
		if (ti < 0) { snprintf(err, errn, "Edge `to` references unknown node id \"%s\" — it is not in the tree.", t ? t : ""); rc = -1; break; }
		if (fi == ti) { snprintf(err, errn, "Edge \"%s\" → \"%s\" is a self-loop — a node can't require itself.", f, t); rc = -1; break; }
		int dup = 0;
		for (int i = 0; i < acc; i++) if (ef[i] == fi && et[i] == ti) dup = 1;
		if (dup) { snprintf(err, errn, "Edge \"%s\" → \"%s\" already exists — duplicate edges are not allowed.", f, t); rc = -1; break; }
		if (reaches(k, ef, et, acc, ti, fi)) {
			snprintf(err, errn, "Edge \"%s\" → \"%s\" would create a cycle: \"%s\" already leads (directly or transitively) back to \"%s\". "
				"\"from\" must be understood before \"to\" — the tree is a DAG.", f, t, t, f);
			rc = -1; break;
		}
		ef[acc] = fi; et[acc] = ti; acc++;
	}
	free(ids); free(ef); free(et);
	return rc;
#undef FAIL
}

cJSON *plan_create(const cJSON *nodes, const cJSON *edges, char *err, size_t errn) {
	if (plan_validate(nodes, edges, err, errn)) return NULL;
	cJSON *t = cJSON_CreateObject();
	cJSON_AddItemToObject(t, "nodes", cJSON_Duplicate(nodes, 1));
	cJSON_AddItemToObject(t, "edges", cJSON_Duplicate(edges, 1));
	cJSON_AddNumberToObject(t, "version", 1);
	return t;
}

int plan_has_node(const cJSON *tree, const char *id) {
	const cJSON *nd;
	cJSON_ArrayForEach(nd, cJSON_GetObjectItemCaseSensitive(tree, "nodes")) {
		const char *s = str(nd, "id");
		if (s && !strcmp(s, id)) return 1;
	}
	return 0;
}

/* ── mermaid authoring subset → tree ───────────────────────────────────────
 * graph|flowchart TD|TB|LR|RL|BT header; %% comments; `id`, `id[title]`,
 * `id("title")`, `id(title)` endpoints; -->, ==>, -.-> requires-edges
 * (chains expand to pairs). Ids are the one strict part. Every problem is
 * collected (line-numbered) so the teacher can fix them all in one retry. */

typedef struct { char *p; size_t n; } pbuf;
static void problem(pbuf *b, const char *fmt, const char *a, int line) {
	char tmp[1024];
	int l = line ? snprintf(tmp, sizeof tmp, "line %d: ", line) : 0;
	snprintf(tmp + l, sizeof tmp - (size_t)l, fmt, a);
	size_t tl = strlen(tmp);
	b->p = realloc(b->p, b->n + tl + 2);
	if (b->n) b->p[b->n++] = '\n';
	memcpy(b->p + b->n, tmp, tl + 1);
	b->n += tl;
}

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t' || *s == '\r') s++;
	size_t l = strlen(s);
	while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r')) s[--l] = 0;
	return s;
}

static int strict_slug(const char *s) {
	int prev_sep = 1;
	if (!*s) return 0;
	for (; *s; s++) {
		int alnum = (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9');
		if (alnum) prev_sep = 0;
		else if ((*s == '-' || *s == '_') && !prev_sep) prev_sep = 1;
		else return 0;
	}
	return !prev_sep;
}

/* Parse one endpoint in place: id + optional label. 0 ok. */
static int endpoint(char *t, char **id, char **label) {
	t = trim(t);
	size_t i = strcspn(t, " \t()[]");
	if (!i) return -1;
	*id = t;
	char *r = t + i;
	while (*r == ' ' || *r == '\t') r++;
	char save = *r, *end = r + strlen(r) - 1;
	t[i] = 0; /* may be r itself: only r + 1 onwards is read below */
	*label = NULL;
	if (!save) return 0;
	if (save == '[' && *end == ']') { *end = 0; if (strpbrk(r + 1, "[]")) return -1; *label = r + 1; }
	else if (save == '(' && *end == ')') { *end = 0; if (strpbrk(r + 1, "()")) return -1; *label = r + 1; }
	else return -1;
	char *l = trim(*label);
	size_t ll = strlen(l);
	if (ll >= 2 && ((l[0] == '"' && l[ll - 1] == '"') || (l[0] == '\'' && l[ll - 1] == '\''))) { l[ll - 1] = 0; l = trim(l + 1); }
	*label = l;
	return 0;
}

static const char *arrow(const char *s, size_t *len) {
	const char *best = NULL;
	static const char *A[] = { "-->", "==>", "-.->" };
	for (int k = 0; k < 3; k++) {
		const char *p = strstr(s, A[k]);
		if (p && (!best || p < best)) { best = p; *len = strlen(A[k]); }
	}
	return best;
}

static cJSON *find_node(cJSON *nodes, const char *id) {
	cJSON *n;
	cJSON_ArrayForEach(n, nodes) if (!strcmp(str(n, "id"), id)) return n;
	return NULL;
}

static void reg(cJSON *nodes, const char *id, const char *label, pbuf *pb, int line) {
	if (!strict_slug(id)) {
		problem(pb, "node id `%s` must be a lowercase slug (a-z, 0-9, joined by hyphens or underscores — e.g. `chain-rule-intuition`); ids are permanent and checks reference them.", id, line);
		return;
	}
	cJSON *n = find_node(nodes, id);
	int has = label && *label;
	if (!n) {
		n = cJSON_CreateObject();
		cJSON_AddStringToObject(n, "id", id);
		cJSON_AddStringToObject(n, "title", has ? label : id);
		cJSON_AddItemToArray(nodes, n);
	} else if (has && !strcmp(str(n, "title"), id)) {
		cJSON_ReplaceItemInObject(n, "title", cJSON_CreateString(label));
	}
}

cJSON *plan_from_mermaid(const char *src, char *err, size_t errn) {
	pbuf pb = {0};
	char *copy = strdup(src ? src : "");
	cJSON *nodes = cJSON_CreateArray(), *edges = cJSON_CreateArray();
	int lineno = 0, header = 0;
	char *cursor = copy, *ln;
	while ((ln = strsep(&cursor, "\n"))) { /* strsep keeps empty lines: numbering stays exact */
		lineno++;
		char *line = trim(ln);
		if (!*line || !strncmp(line, "%%", 2)) continue;
		if (!header) {
			header = 1;
			char kw[16] = "", dir[8] = "", extra[4] = "";
			int got = sscanf(line, "%15s %7s %3s", kw, dir, extra);
			for (char *c = kw; *c; c++) *c = (char)(*c | 32);
			for (char *c = dir; *c; c++) *c = (char)(*c | 32);
			int ok = got == 2 && (!strcmp(kw, "graph") || !strcmp(kw, "flowchart")) &&
				(!strcmp(dir, "td") || !strcmp(dir, "tb") || !strcmp(dir, "lr") || !strcmp(dir, "rl") || !strcmp(dir, "bt"));
			if (!ok) problem(&pb, "expected the header `graph TD` (or flowchart TD/TB) first, got `%s`.", line, lineno);
			continue;
		}
		char full[1024];
		snprintf(full, sizeof full, "%s", line);
		size_t al;
		if (arrow(line, &al)) {
			char *segs[64], *ids[64], *labels[64];
			int ns = 0, bad = 0;
			char *p = line;
			for (const char *a; ns < 63 && (a = arrow(p, &al)); p = (char *)a + al) { *(char *)a = 0; segs[ns++] = p; }
			segs[ns++] = p;
			for (int k = 0; k < ns && !bad; k++) bad = endpoint(segs[k], &ids[k], &labels[k]);
			if (bad) { problem(&pb, "can't read the edge `%s` — expected `a[title] --> b[title]` (chains like `a --> b --> c` are fine).", full, lineno); continue; }
			for (int k = 0; k < ns; k++) reg(nodes, ids[k], labels[k], &pb, lineno);
			for (int k = 0; k + 1 < ns; k++) {
				if (!strcmp(ids[k], ids[k + 1])) { problem(&pb, "self-loop `%s` — a node can't require itself.", ids[k], lineno); continue; }
				if (!strict_slug(ids[k]) || !strict_slug(ids[k + 1])) continue;
				int dup = 0;
				const cJSON *e;
				cJSON_ArrayForEach(e, edges) if (!strcmp(str(e, "from"), ids[k]) && !strcmp(str(e, "to"), ids[k + 1])) dup = 1;
				if (dup) continue;
				cJSON *e2 = cJSON_CreateObject();
				cJSON_AddStringToObject(e2, "from", ids[k]);
				cJSON_AddStringToObject(e2, "to", ids[k + 1]);
				cJSON_AddItemToArray(edges, e2);
			}
		} else {
			char *id, *label;
			if (endpoint(line, &id, &label)) {
				problem(&pb, "can't parse `%s` — ids are the strict part (lowercase slugs: a-z, 0-9, hyphens, underscores); node lines (`slug[title]`) and requires-edges (`a --> b`) are the accepted forms; subgraphs, styles, and classes are not part of the subset.", full, lineno);
				continue;
			}
			reg(nodes, id, label, &pb, lineno);
		}
	}
	free(copy);
	if (!header) problem(&pb, "%sThe plan is empty — expected a `graph TD` block with nodes and `-->` requires-edges.", "", 0);
	else if (!pb.n && cJSON_GetArraySize(nodes) == 0) problem(&pb, "%sNo nodes found — the plan needs at least one.", "", 0);
	cJSON *tree = NULL;
	if (pb.n) snprintf(err, errn, "%s", pb.p);
	else tree = plan_create(nodes, edges, err, errn);
	free(pb.p);
	cJSON_Delete(nodes); cJSON_Delete(edges);
	return tree;
}

/* ── terminal view: topological list indented by longest-path depth ─────── */

static int in_list(const cJSON *arr, const char *id) {
	const cJSON *v;
	cJSON_ArrayForEach(v, arr) if (cJSON_IsString(v) && !strcmp(v->valuestring, id)) return 1;
	return 0;
}

char *plan_render(const cJSON *tree, const char *current, const cJSON *visited) {
	const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(tree, "nodes"), *edges = cJSON_GetObjectItemCaseSensitive(tree, "edges");
	int n = cJSON_GetArraySize(nodes), ne = cJSON_GetArraySize(edges);
	const char **ids = calloc((size_t)n + 1, sizeof *ids), **titles = calloc((size_t)n + 1, sizeof *titles);
	int *ef = calloc((size_t)ne + 1, sizeof(int)), *et = calloc((size_t)ne + 1, sizeof(int));
	int *indeg = calloc((size_t)n + 1, sizeof(int)), *depth = calloc((size_t)n + 1, sizeof(int)), *done = calloc((size_t)n + 1, sizeof(int));
	int k = 0, w = 0;
	const cJSON *x;
	cJSON_ArrayForEach(x, nodes) { ids[k] = str(x, "id"); titles[k] = math_text(str(x, "title")); if ((int)strlen(ids[k]) > w) w = (int)strlen(ids[k]); k++; }
	k = 0;
	cJSON_ArrayForEach(x, edges) { ef[k] = index_of(ids, n, str(x, "from")); et[k] = index_of(ids, n, str(x, "to")); indeg[et[k]]++; k++; }
	pbuf out = {0};
	/* Kahn's algorithm, always taking the earliest-declared ready node */
	for (int placed = 0; placed < n; placed++) {
		int pick = -1;
		for (int i = 0; i < n && pick < 0; i++) if (!done[i] && indeg[i] == 0) pick = i;
		if (pick < 0) break; /* unreachable for a validated tree */
		done[pick] = 1;
		for (int e = 0; e < ne; e++) if (ef[e] == pick) { indeg[et[e]]--; if (depth[pick] + 1 > depth[et[e]]) depth[et[e]] = depth[pick] + 1; }
		int cur = current && !strcmp(current, ids[pick]), vis = !cur && in_list(visited, ids[pick]);
		char line[2048];
		int l = snprintf(line, sizeof line, "%*s%s %-*s %s", 2 + 2 * depth[pick], "",
			cur ? "\033[1;36m●\033[0m" : vis ? "\033[32m✓\033[0m" : "\033[2m○\033[0m", w, ids[pick], titles[pick]);
		int npre = 0;
		for (int e = 0; e < ne; e++) if (et[e] == pick) npre++;
		if (npre >= 2) {
			l += snprintf(line + l, sizeof line - (size_t)l, " \033[2m←");
			int first = 1;
			for (int e = 0; e < ne && l < (int)sizeof line - 64; e++)
				if (et[e] == pick) { l += snprintf(line + l, sizeof line - (size_t)l, "%s %s", first ? "" : ",", ids[ef[e]]); first = 0; }
			l += snprintf(line + l, sizeof line - (size_t)l, "\033[0m");
		}
		problem(&out, "%s", line, 0);
	}
	for (int i = 0; i < n; i++) free((char *)titles[i]);
	free(ids); free(titles); free(ef); free(et); free(indeg); free(depth); free(done);
	if (!out.p) return strdup("");
	return out.p;
}
