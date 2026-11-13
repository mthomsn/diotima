#include "tools.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "store.h"
#include "phase.h"
#include "plan.h"
#include "graph.h"
#include "math.h"
#include "prose.h"
#include "jobs.h"
#include "session.h"
#include "term.h"

static const char *S_(const cJSON *o, const char *k) {
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}
static const char *optval(const cJSON *opt) { const char *v = S_(opt, "value"); return v ? v : S_(opt, "label"); }
static int blank(const char *s) { return !s || !*s; }

/* ── authoring contracts (the call is returned to the model to fix) ────── */

/* Append one problem to a "; "-joined list. */
static void addp(char *msg, size_t n, const char *fmt, ...) {
	size_t l = strlen(msg);
	if (l && l + 2 < n) { strcat(msg, "; "); l += 2; }
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg + l, n - l, fmt, ap);
	va_end(ap);
}

static char *rejected(const char *tool, const char *msg) {
	size_t n = strlen(msg) + 96;
	char *out = malloc(n);
	snprintf(out, n, "%s rejected: %s. Fix the call and send it again.", tool, msg);
	return out;
}

static void quiz_authoring(const cJSON *a, char *msg, size_t mn) {
	const cJSON *opts = cJSON_GetObjectItem(a, "options"), *ca = cJSON_GetObjectItem(a, "correctAnswer"), *o;
	int multi = cJSON_IsTrue(cJSON_GetObjectItem(a, "multiSelect"));
	if (blank(S_(a, "question"))) { addp(msg, mn, "`question` is empty"); return; }
	if (blank(S_(a, "explanation"))) { addp(msg, mn, "`explanation` is required"); return; }
	if (!cJSON_IsArray(opts) || cJSON_GetArraySize(opts) < 2) { addp(msg, mn, "`options` must be an array of 2+ option objects"); return; }
	cJSON_ArrayForEach(o, opts) {
		if (blank(S_(o, "label"))) { addp(msg, mn, "every option needs a non-empty `label`"); return; }
		for (const cJSON *p = o->next; p; p = p->next)
			if (!strcmp(optval(o), optval(p) ? optval(p) : "")) { addp(msg, mn, "option values must be distinct (\"%s\" repeats)", optval(o)); return; }
	}
	int n = cJSON_IsArray(ca) ? cJSON_GetArraySize(ca) : cJSON_IsString(ca) ? 1 : 0;
	if (!n) { addp(msg, mn, "`correctAnswer` must be an option value (or an array of them for multiSelect)"); return; }
	if (cJSON_IsArray(ca) && !multi) { addp(msg, mn, "an array `correctAnswer` requires multiSelect:true"); return; }
	for (int i = 0; i < n; i++) {
		const char *v = cJSON_IsArray(ca) ? cJSON_GetStringValue(cJSON_GetArrayItem(ca, i)) : ca->valuestring;
		int ok = 0;
		cJSON_ArrayForEach(o, opts) if (v && !strcmp(v, optval(o))) ok = 1;
		if (!ok) { addp(msg, mn, "correctAnswer \"%s\" matches no option value", v ? v : "?"); return; }
	}
}

char *quiz_problems(const cJSON *a) {
	char msg[512] = "";
	quiz_authoring(a, msg, sizeof msg);
	return msg[0] ? rejected("quiz", msg) : NULL;
}

/* Teach-side contract: `type` is predict|check, `nodeId` names a node of the live plan. */
static void teach_problems(const cJSON *a, char *msg, size_t mn) {
	const char *type = S_(a, "type"), *node = S_(a, "nodeId");
	if (!type || (strcmp(type, "predict") && strcmp(type, "check")))
		addp(msg, mn, "`type` must be the bare word predict (posed before you teach the step) or check (after its teaching)%s%s%s",
			type ? ", got \"" : "", type ? type : "", type ? "\"" : "");
	pthread_mutex_lock(&S->mu);
	cJSON *tree = phase_plan(S->messages);
	pthread_mutex_unlock(&S->mu);
	if (!tree) addp(msg, mn, "no plan map exists yet — call plan_init before posing quizzes");
	else if (blank(node)) addp(msg, mn, "`nodeId` is required — the plan node this question is about (off-map checks use ask_user_question)");
	else if (!plan_has_node(tree, node)) {
		char ids[300] = "";
		int k = 0;
		const cJSON *nd;
		cJSON_ArrayForEach(nd, cJSON_GetObjectItem(tree, "nodes")) {
			if (k == 5) { strncat(ids, ", …", sizeof ids - strlen(ids) - 1); break; }
			if (k++) strncat(ids, ", ", sizeof ids - strlen(ids) - 1);
			strncat(ids, S_(nd, "id"), sizeof ids - strlen(ids) - 1);
		}
		addp(msg, mn, "nodeId \"%s\" matches no node in this session's plan map — copy one of your plan_init slugs exactly (%s)", node, ids);
	}
	cJSON_Delete(tree);
}

static int is_check(const cJSON *a) { const char *t = S_(a, "type"); return t && !strcmp(t, "check"); }

/* ── learner interaction ───────────────────────────────────────────────── */

static void count_open(int d) {
	pthread_mutex_lock(&S->mu);
	S->open_questions += d;
	pthread_mutex_unlock(&S->mu);
}

static char *eof_result(int *is_error) {
	S->quit = 1;
	*is_error = 1;
	return strdup("The learner closed the session. End your turn.");
}

/* Reads indices like "2" or "1,3"; returns count or -1 on bad input. 0 = "I don't know". */
static int parse_choice(const char *s, int nopts, int multi, int *out) {
	int n = 0;
	while (*s) {
		while (*s == ' ' || *s == ',') s++;
		if (!*s) break;
		char *end;
		long v = strtol(s, &end, 10);
		if (end == s || v < 0 || v > nopts) return -1;
		if (v == 0) return n == 0 && !*end ? 0 : -1;
		for (int i = 0; i < n; i++) if (out[i] == v) return -1;
		if (n >= nopts) return -1;
		out[n++] = (int)v;
		s = end;
	}
	if (n == 0 || (n > 1 && !multi)) return -1;
	return n;
}

/* A numbered option, wrapped with its continuation lines under the text. */
static void put_option(const char *num, const char *text) {
	char pre[32], rest[32];
	snprintf(pre, sizeof pre, "  %s) ", num);
	snprintf(rest, sizeof rest, "%*s", (int)strlen(pre), "");
	prose_block(text, pre, rest, 0);
}

/* Question prompts: "/quit" (or Ctrl-D) ends the session from inside a question too. */
static int q_ask(const char *label, char *buf, size_t n) {
	int r = term_ask(label, buf, n);
	if (!r && !strcmp(buf, "/quit")) { buf[0] = 0; return -1; }
	return r;
}

/* Checks arrive collapsed: the learner opens one when done reading, and may
 * say what he thought while reading. Caller holds term_mu. -1 on EOF. */
static int check_gate(char *reading, size_t n) {
	char line[256];
	if (q_ask("\n\033[1m▸ Check your understanding\033[0m — press enter when ready ", line, sizeof line)) return -1;
	return q_ask("thinking while reading (optional, enter to skip)> ", reading, n);
}

/* Asked right after the question is shown, so he can think out loud while
 * working it out rather than recounting afterwards. -1 on EOF. */
static int ask_thinking(char *buf, size_t n) {
	return q_ask("thinking as you work it out (optional, enter to skip)> ", buf, n);
}

static void show_question(const cJSON *a) {
	printf("\n");
	prose_block(S_(a, "question"), "\033[1m? ", "  ", 1);
	if (S_(a, "details")) prose_block(S_(a, "details"), "  ", NULL, 0);
}

#define EOF_OUT(...) do { __VA_ARGS__; term_unlock(); count_open(-1); return eof_result(is_error); } while (0)

static char *run_quiz(const cJSON *a, int *is_error, int teach) {
	char msg[1024] = "";
	quiz_authoring(a, msg, sizeof msg);
	if (teach) teach_problems(a, msg, sizeof msg);
	if (msg[0]) { *is_error = 1; return rejected("quiz", msg); }
	const cJSON *opts = cJSON_GetObjectItem(a, "options"), *ca = cJSON_GetObjectItem(a, "correctAnswer");
	int multi = cJSON_IsTrue(cJSON_GetObjectItem(a, "multiSelect")), n = cJSON_GetArraySize(opts);
	char line[2048], thinking[2048] = "", reading[2048] = "";
	count_open(1);
	term_lock();
	if (teach && is_check(a) && check_gate(reading, sizeof reading)) EOF_OUT();
	show_question(a);
	for (int i = 0; i < n; i++) { char k[12]; snprintf(k, sizeof k, "%d", i + 1); put_option(k, S_(cJSON_GetArrayItem(opts, i), "label")); }
	printf("  0) I don't know\n");
	if (ask_thinking(thinking, sizeof thinking)) EOF_OUT();
	int picks[64], np;
	for (;;) {
		if (q_ask(multi ? "answer (e.g. 1,3)> " : "answer> ", line, sizeof line)) EOF_OUT();
		np = parse_choice(line, n < 64 ? n : 63, multi, picks);
		if (np >= 0) break;
		printf("  pick %s of 1-%d, or 0\n", multi ? "one or more" : "one", n);
	}
	int dont = np == 0, conf = -1;
	if (!dont) {
		if (q_ask("confidence that you're right, 0-100 [50]> ", line, sizeof line)) EOF_OUT();
		conf = line[0] ? atoi(line) : 50;
		if (conf < 0) conf = 0;
		if (conf > 100) conf = 100;
	}

	cJSON *picked = cJSON_CreateArray();
	for (int i = 0; i < np; i++) cJSON_AddItemToArray(picked, cJSON_CreateString(optval(cJSON_GetArrayItem(opts, picks[i] - 1))));
	int correct = 0;
	if (!dont) {
		int want = cJSON_IsArray(ca) ? cJSON_GetArraySize(ca) : 1;
		correct = want == np;
		for (int i = 0; correct && i < np; i++) {
			const char *pv = cJSON_GetArrayItem(picked, i)->valuestring;
			int found = 0;
			for (int j = 0; j < want; j++) {
				const char *cv = cJSON_IsArray(ca) ? cJSON_GetArrayItem(ca, j)->valuestring : ca->valuestring;
				if (!strcmp(pv, cv)) found = 1;
			}
			correct = found;
		}
	}
	printf("%s\n", dont ? "\033[33m– no guess recorded\033[0m" : correct ? "\033[32m✓ correct\033[0m" : "\033[31m✗ not quite\033[0m");
	if (!correct) {
		const cJSON *o;
		size_t cap = 64, len = 0;
		char *right = malloc(cap);
		len = (size_t)snprintf(right, cap, "correct: ");
		cJSON_ArrayForEach(o, opts) {
			const char *v = optval(o), *lb = S_(o, "label") ? S_(o, "label") : "";
			int hit = cJSON_IsArray(ca) ? 0 : !strcmp(v, ca->valuestring);
			for (int j = 0; cJSON_IsArray(ca) && j < cJSON_GetArraySize(ca); j++) if (!strcmp(v, cJSON_GetArrayItem(ca, j)->valuestring)) hit = 1;
			if (!hit) continue;
			size_t l = strlen(lb);
			while (len + l + 3 > cap) right = realloc(right, cap *= 2);
			len += (size_t)sprintf(right + len, "%s%s", len > 9 ? "; " : "", lb);
		}
		prose_block(right, "  ", NULL, 0);
		free(right);
	}
	prose_block(S_(a, "explanation"), NULL, NULL, 0);
	term_unlock();
	count_open(-1);

	cJSON *r = cJSON_CreateObject();
	cJSON_AddStringToObject(r, "kind", "quiz");
	cJSON_AddStringToObject(r, "verdict", dont ? "dontKnow" : correct ? "correct" : "wrong");
	cJSON_AddItemToObject(r, "picked", picked);
	cJSON_AddItemToObject(r, "correctAnswer", cJSON_Duplicate(ca, 1));
	if (!dont) cJSON_AddNumberToObject(r, "confidence", conf);
	if (thinking[0]) cJSON_AddStringToObject(r, "thinking", thinking);
	if (reading[0]) cJSON_AddStringToObject(r, "thinkingWhileReading", reading);
	char *s = cJSON_PrintUnformatted(r);
	cJSON_Delete(r);
	return s;
}

/* Free recall: the commitment (correctAnswer) is fixed before he sees the
 * question, revealed on submit; the model grades his words in its reply. */
static char *run_explain(const cJSON *a, int *is_error, int teach) {
	char msg[1024] = "";
	if (blank(S_(a, "question"))) addp(msg, sizeof msg, "`question` is empty");
	if (blank(S_(a, "correctAnswer"))) addp(msg, sizeof msg, "correctAnswer is required — commit to the full correct answer BEFORE posing the question");
	if (teach) teach_problems(a, msg, sizeof msg);
	if (msg[0]) { *is_error = 1; return rejected("explain", msg); }
	char answer[4096], thinking[2048] = "", reading[2048] = "";
	count_open(1);
	term_lock();
	if (teach && is_check(a) && check_gate(reading, sizeof reading)) EOF_OUT();
	show_question(a);
	if (ask_thinking(thinking, sizeof thinking)) EOF_OUT();
	if (q_ask("answer in your own words (enter = I don't know)> ", answer, sizeof answer)) EOF_OUT();
	printf("\033[2m  committed answer:\033[0m\n");
	prose_block(S_(a, "correctAnswer"), NULL, NULL, 0);
	term_unlock();
	count_open(-1);

	cJSON *r = cJSON_CreateObject();
	cJSON_AddStringToObject(r, "kind", "explain");
	if (answer[0]) cJSON_AddStringToObject(r, "text", answer);
	else cJSON_AddTrueToObject(r, "dontKnow");
	if (thinking[0]) cJSON_AddStringToObject(r, "thinking", thinking);
	if (reading[0]) cJSON_AddStringToObject(r, "thinkingWhileReading", reading);
	char *s = cJSON_PrintUnformatted(r);
	cJSON_Delete(r);
	return s;
}

static char *tool_quiz(const cJSON *a, int *e) { return run_quiz(a, e, 0); }
static char *tool_quiz_teach(const cJSON *a, int *e) { return run_quiz(a, e, 1); }
static char *tool_explain(const cJSON *a, int *e) { return run_explain(a, e, 0); }
static char *tool_explain_teach(const cJSON *a, int *e) { return run_explain(a, e, 1); }

static char *tool_ask(const cJSON *a, int *is_error) {
	const cJSON *opts = cJSON_GetObjectItem(a, "options");
	int n = cJSON_IsArray(opts) ? cJSON_GetArraySize(opts) : 0, multi = cJSON_IsTrue(cJSON_GetObjectItem(a, "multiSelect"));
	if (blank(S_(a, "question"))) { *is_error = 1; return strdup("ask_user_question rejected: `question` is empty."); }
	if (n == 1) { *is_error = 1; return strdup("ask_user_question rejected: `options` needs 2+ entries (or omit it for free text)."); }
	count_open(1);
	term_lock();
	show_question(a);
	for (int i = 0; i < n; i++) {
		const cJSON *o = cJSON_GetArrayItem(opts, i);
		const char *lb = S_(o, "label") ? S_(o, "label") : "", *ds = S_(o, "description");
		char k[12], *t = malloc(strlen(lb) + (ds ? strlen(ds) : 0) + 8);
		snprintf(k, sizeof k, "%d", i + 1);
		sprintf(t, ds ? "%s — %s" : "%s", lb, ds);
		put_option(k, t);
		free(t);
	}
	if (n) printf("  o) Other (type your own)\n");
	char line[2048], thinking[2048] = "";
	cJSON *r = cJSON_CreateObject();
	cJSON_AddStringToObject(r, "kind", "ask");
	if (ask_thinking(thinking, sizeof thinking)) EOF_OUT(cJSON_Delete(r));
	for (;;) {
		if (q_ask(n ? (multi ? "answer (e.g. 1,3 or o)> " : "answer> ") : "answer> ", line, sizeof line)) EOF_OUT(cJSON_Delete(r));
		if (!n) { cJSON_AddStringToObject(r, "answer", line); break; }
		if (!strcmp(line, "o") || !strcmp(line, "O")) {
			if (q_ask("your answer> ", line, sizeof line)) EOF_OUT(cJSON_Delete(r));
			cJSON_AddStringToObject(r, "other", line);
			break;
		}
		int picks[64], np = parse_choice(line, n < 64 ? n : 63, multi, picks);
		if (np > 0) {
			cJSON *sel = cJSON_AddArrayToObject(r, "selected");
			for (int i = 0; i < np; i++) cJSON_AddItemToArray(sel, cJSON_CreateString(S_(cJSON_GetArrayItem(opts, picks[i] - 1), "label")));
			break;
		}
		printf("  pick 1-%d or o\n", n);
	}
	term_unlock();
	count_open(-1);
	if (thinking[0]) cJSON_AddStringToObject(r, "thinking", thinking);
	char *s = cJSON_PrintUnformatted(r);
	cJSON_Delete(r);
	return s;
}

static char *tool_probe_done(const cJSON *a, int *is_error) {
	const cJSON *as = cJSON_GetObjectItem(a, "assessment"), *strands = cJSON_GetObjectItem(as, "strands"), *st;
	const char *bad = NULL;
	if (!cJSON_IsObject(as)) bad = "`assessment` must be an object {strands:[{strand,assessment}], summary?}, not a string";
	else if (!cJSON_IsArray(strands) || cJSON_GetArraySize(strands) == 0) bad = "`assessment.strands` must be a non-empty array";
	else cJSON_ArrayForEach(st, strands) if (!S_(st, "strand") || !S_(st, "assessment")) { bad = "each strand needs string `strand` and `assessment`"; break; }
	if (bad) { *is_error = 1; char *m = malloc(300); snprintf(m, 300, "probe_done rejected: %s.", bad); return m; }
	pthread_mutex_lock(&S->mu);
	int open = S->open_questions;
	pthread_mutex_unlock(&S->mu);
	if (open > 0) { *is_error = 1; return strdup("probe_done refused: resolve the open question first."); }
	pthread_mutex_lock(&S->mu);
	/* the current call is in the transcript without a result, so it doesn't count */
	int done = phase_probe_concluded(S->messages);
	if (!done) S->handoff = 1;
	pthread_mutex_unlock(&S->mu);
	if (done) { *is_error = 1; return strdup("probe_done refused: the probe already concluded."); }
	return strdup("Probe concluded. Assessment recorded; the lesson planner takes over now. Write a one-sentence closing and end your turn.");
}

char *tools_plan_view(const cJSON *tree) {
	pthread_mutex_lock(&S->mu);
	cJSON *vis = NULL;
	const char *c = phase_position(S->messages, tree, &vis);
	char cur[128] = "";
	if (c) snprintf(cur, sizeof cur, "%s", c);
	pthread_mutex_unlock(&S->mu);
	char *view = plan_render(tree, cur[0] ? cur : NULL, vis);
	cJSON_Delete(vis);
	return view;
}

/* Author-once: the map is written (or rewritten wholesale) here; where the
 * lesson stands is derived from nodeId tags, never declared. */
static char *tool_plan_init(const cJSON *a, int *is_error) {
	char err[4096];
	cJSON *tree = plan_from_mermaid(S_(a, "mermaid"), err, sizeof err);
	if (!tree) {
		*is_error = 1;
		size_t n = strlen(err) + 96;
		char *m = malloc(n);
		snprintf(m, n, "plan_init rejected — fix every problem and call again:\n%s", err);
		return m;
	}
	char *view = tools_plan_view(tree);
	term_lock();
	printf("\n\033[1mplan\033[0m\n%s\n", view);
	fflush(stdout);
	term_unlock();
	free(view);
	cJSON *r = cJSON_CreateObject(), *ids = cJSON_AddArrayToObject(r, "nodeIds");
	const cJSON *nd;
	cJSON_ArrayForEach(nd, cJSON_GetObjectItem(tree, "nodes")) cJSON_AddItemToArray(ids, cJSON_CreateString(S_(nd, "id")));
	cJSON_AddTrueToObject(r, "ok");
	cJSON_AddStringToObject(r, "reminder", "These are the plan's node ids — the only valid nodeId values for every quiz and explain call from here on. Reference them exactly.");
	cJSON_Delete(tree);
	char *s = cJSON_PrintUnformatted(r);
	cJSON_Delete(r);
	return s;
}

/* ── map readers (prober, practice): the graph's organization, not its prose ── */

static const char *birth_of(const cJSON *n) { return S_(cJSON_GetArrayItem(cJSON_GetObjectItem(n, "provenance"), 0), "chatId"); }

/* First user line of a chat, as its name. */
char *tools_session_index(const graph_t *g, cJSON **ids_out) {
	/* sessions in birth order of their first node; listed most recent first */
	cJSON *ids = cJSON_CreateArray(), *count = cJSON_CreateObject(), *born = cJSON_CreateObject();
	const cJSON *n;
	cJSON_ArrayForEach(n, g->nodes) {
		const char *b = birth_of(n);
		if (!b) continue;
		cJSON *c = cJSON_GetObjectItemCaseSensitive(count, b);
		if (c) { cJSON_SetNumberValue(c, c->valuedouble + 1); continue; }
		cJSON_AddNumberToObject(count, b, 1);
		cJSON_AddStringToObject(born, b, S_(n, "createdAt") ? S_(n, "createdAt") : "unknown date");
		cJSON_InsertItemInArray(ids, 0, cJSON_CreateString(b));
	}
	size_t cap = 256, len = 0;
	char *out = malloc(cap);
	out[0] = 0;
	int i = 0;
	const cJSON *id;
	cJSON_ArrayForEach(id, ids) {
		char title[81], line[512];
		session_title(id->valuestring, title, sizeof title);
		int c = (int)cJSON_GetObjectItemCaseSensitive(count, id->valuestring)->valuedouble;
		int l = snprintf(line, sizeof line, "%d. %s (chat, %d proposition%s, born %.10s) — id: %s\n", ++i, title[0] ? title : id->valuestring,
			c, c == 1 ? "" : "s", S_(born, id->valuestring), id->valuestring);
		while (len + (size_t)l + 1 > cap) out = realloc(out, cap *= 2);
		memcpy(out + len, line, (size_t)l + 1);
		len += (size_t)l;
	}
	cJSON_Delete(count); cJSON_Delete(born);
	if (ids_out) *ids_out = ids; else cJSON_Delete(ids);
	return out;
}

static char *tool_list_sessions(const cJSON *a, int *is_error) {
	(void)a; (void)is_error;
	graph_t g = graph_load();
	cJSON *ids;
	char *idx = tools_session_index(&g, &ids);
	int n = cJSON_GetArraySize(ids);
	cJSON_Delete(ids);
	graph_free(&g);
	if (!n) { free(idx); return strdup("The knowledge graph is empty — no session has contributed propositions yet."); }
	size_t len = strlen(idx) + 96;
	char *out = malloc(len);
	snprintf(out, len, "%d session(s) in the knowledge graph, most recent first:\n%s", n, idx);
	free(idx);
	return out;
}

static char *tool_list_propositions(const cJSON *a, int *is_error) {
	const char *sess = S_(a, "session");
	if (blank(sess)) { *is_error = 1; return strdup("list_propositions rejected: `session` is required — an id exactly as list_sessions reported it."); }
	graph_t g = graph_load();
	size_t cap = 256, len = 0;
	char *body = calloc(1, cap);
	int k = 0;
	const cJSON *n;
	cJSON_ArrayForEach(n, g.nodes) {
		if (!birth_of(n) || strcmp(birth_of(n), sess)) continue;
		const char *nm = S_(n, "name") ? S_(n, "name") : "";
		while (len + strlen(nm) + 4 > cap) body = realloc(body, cap *= 2);
		len += (size_t)sprintf(body + len, "- %s\n", nm);
		k++;
	}
	graph_free(&g);
	if (!k) {
		free(body);
		size_t m = strlen(sess) + 96;
		char *out = malloc(m);
		snprintf(out, m, "No propositions born in session \"%s\". Call list_sessions for the current ids.", sess);
		return out;
	}
	char *out = malloc(len + 64);
	snprintf(out, len + 64, "%d proposition(s) covered in this session:\n%s", k, body);
	free(body);
	return out;
}

static char *tool_find_candidates(const cJSON *a, int *is_error) {
	const char *q = S_(a, "query");
	if (blank(q)) { *is_error = 1; return strdup("find_candidates rejected: `query` is required — the idea to look for, in plain words."); }
	graph_t g = graph_load();
	cJSON *props = cJSON_CreateArray(), *p = cJSON_CreateObject(), *reqs = cJSON_Parse("[{\"id\":\"q\"}]");
	cJSON_AddStringToObject(p, "id", "q");
	cJSON_AddStringToObject(p, "name", q);
	cJSON_AddStringToObject(p, "statement", "");
	cJSON_AddArrayToObject(p, "aliases");
	cJSON_AddItemToArray(props, p);
	char err[256];
	cJSON *r = graph_candidates(g.nodes, props, reqs, err, sizeof err);
	size_t cap = 256, len = 0;
	char *out = calloc(1, cap);
	const cJSON *c;
	cJSON_ArrayForEach(c, cJSON_GetObjectItem(r, "candidates")) {
		const cJSON *n = NULL, *x;
		cJSON_ArrayForEach(x, g.nodes) if (!strcmp(S_(x, "id"), S_(c, "id"))) n = x;
		char line[512];
		int l = snprintf(line, sizeof line, "- [%s] %s — session %s\n", S_(c, "id"), S_(c, "name"), n && birth_of(n) ? birth_of(n) : "?");
		while (len + (size_t)l + 1 > cap) out = realloc(out, cap *= 2);
		memcpy(out + len, line, (size_t)l + 1);
		len += (size_t)l;
	}
	cJSON_Delete(r); cJSON_Delete(props); cJSON_Delete(reqs); graph_free(&g);
	if (!len) { free(out); return strdup("No proposition in the knowledge graph matches that query."); }
	return out;
}

/* ── practice: pose with a committed solution, conclude to reveal it ───── */

static const char *CONCLUDE_ACK =
	"Problem concluded. Say your closing words about the learner's work if useful, then END YOUR TURN — no further tool calls. "
	"The next problem is NEVER posed here: it waits for the learner's explicit go-ahead (a *Next problem* message, or their own words asking for the next one). "
	"Between problems, answer their questions about the concluded one.";

const char *tools_conclude_ack(void) { return CONCLUDE_ACK; }

static int one_of(const char *v, const char *const *set) { for (; v && *set; set++) if (!strcmp(v, *set)) return 1; return 0; }

/* id exact, else case-insensitive name; NULL when nothing matches */
static const char *resolve_target(const cJSON *nodes, const char *t) {
	const cJSON *n;
	cJSON_ArrayForEach(n, nodes) if (!strcmp(S_(n, "id"), t)) return S_(n, "id");
	cJSON_ArrayForEach(n, nodes) if (S_(n, "name") && !strcasecmp(S_(n, "name"), t)) return S_(n, "id");
	return NULL;
}

static const cJSON *active_problem_locked(void) { return phase_active_problem(S->messages); }

void tools_show_solution(const cJSON *problem) {
	printf("\033[2m  model solution:\033[0m\n");
	prose_block(S_(problem, "modelSolution"), NULL, NULL, 0);
}

static char *tool_problem(const cJSON *a, int *is_error) {
	char msg[1024] = "";
	static const char *const MODES[] = { "recall", "discrimination", "composition", NULL };
	const cJSON *target = cJSON_GetObjectItem(a, "target"), *t;
	if (blank(S_(a, "statement"))) addp(msg, sizeof msg, "`statement` is required — the problem as the learner sees it");
	if (!one_of(S_(a, "mode"), MODES)) addp(msg, sizeof msg, "`mode` must be the bare word recall, discrimination, or composition");
	if (blank(S_(a, "modelSolution"))) addp(msg, sizeof msg, "`modelSolution` is required — your full worked solution, committed BEFORE the learner sees the problem");
	if (target && (!cJSON_IsArray(target) || cJSON_GetArraySize(target) == 0)) addp(msg, sizeof msg, "`target` must be a non-empty array of proposition ids — or omit it when the material is not in the map");
	else if (target) {
		graph_t g = graph_load();
		char unknown[300] = "";
		cJSON_ArrayForEach(t, target)
			if (!cJSON_IsString(t) || !resolve_target(g.nodes, t->valuestring)) {
				if (unknown[0]) strncat(unknown, ", ", sizeof unknown - strlen(unknown) - 1);
				strncat(unknown, cJSON_IsString(t) ? t->valuestring : "?", sizeof unknown - strlen(unknown) - 1);
			}
		graph_free(&g);
		if (unknown[0]) addp(msg, sizeof msg, "target [%s] is not a known proposition — targets must be proposition ids from the session context; if the material is not in the map, omit target entirely", unknown);
	}
	pthread_mutex_lock(&S->mu);
	int active = active_problem_locked() != NULL;
	pthread_mutex_unlock(&S->mu);
	if (active) addp(msg, sizeof msg, "a problem is already active — call conclude on it before posing the next one");
	if (msg[0]) { *is_error = 1; return rejected("problem", msg); }
	term_lock();
	printf("\n\033[1m▸ Problem\033[0m \033[2m· %s\033[0m\n", S_(a, "mode"));
	prose_block(S_(a, "statement"), "  ", NULL, 0);
	printf("\033[2m  work it below; a blank line sends · /skip dismisses\033[0m\n");
	fflush(stdout);
	term_unlock();
	return strdup("Problem posed — the turn ends here; the learner's thinking traces and answer commits arrive as labeled messages.");
}

static char *tool_conclude(const cJSON *a, int *is_error) {
	char msg[1024] = "";
	static const char *const OUT[] = { "solved", "solvedWithHints", "movedOn", "surrendered", NULL };
	static const char *const RUNG[] = { "R0", "R1", "R2", "R3", "R4", "R5", NULL };
	static const char *const MISS[] = { "cue", "discrimination", "execution", "careless", "structural", NULL };
	const cJSON *rung = cJSON_GetObjectItem(a, "rung"), *miss = cJSON_GetObjectItem(a, "missClass");
	if (S_(a, "outcome") && !strcmp(S_(a, "outcome"), "dismissed")) addp(msg, sizeof msg, "outcome dismissed is synthesized when the learner discards a problem, never yours");
	else if (!one_of(S_(a, "outcome"), OUT)) addp(msg, sizeof msg, "`outcome` must be the bare word solved, solvedWithHints, movedOn, or surrendered");
	if (blank(S_(a, "verdict"))) addp(msg, sizeof msg, "`verdict` is required — what was credited, where it broke");
	if (!rung || !(cJSON_IsNull(rung) || one_of(cJSON_GetStringValue(rung), RUNG))) addp(msg, sizeof msg, "`rung` must be R0 through R5, or null when no hint was given");
	if (!miss || !(cJSON_IsNull(miss) || one_of(cJSON_GetStringValue(miss), MISS))) addp(msg, sizeof msg, "`missClass` must be cue, discrimination, execution, careless, or structural — or null when there was no miss");
	if (!cJSON_IsBool(cJSON_GetObjectItem(a, "retest"))) addp(msg, sizeof msg, "`retest` must be true or false");
	pthread_mutex_lock(&S->mu);
	const cJSON *p = active_problem_locked();
	cJSON *problem = p ? cJSON_Duplicate(p, 1) : NULL;
	pthread_mutex_unlock(&S->mu);
	if (!problem) addp(msg, sizeof msg, "no active problem to conclude — pose one first");
	if (msg[0]) { cJSON_Delete(problem); *is_error = 1; return rejected("conclude", msg); }
	term_lock();
	printf("\n\033[1m── %s ──\033[0m\n", S_(a, "outcome"));
	prose_block(S_(a, "verdict"), NULL, NULL, 0);
	tools_show_solution(problem);
	fflush(stdout);
	term_unlock();
	cJSON_Delete(problem);
	return strdup(CONCLUDE_ACK);
}

/* ── declarations ──────────────────────────────────────────────────────── */

#define OPT_SCHEMA "{\"type\":\"array\",\"minItems\":2,\"items\":{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}},\"required\":[\"label\"]}}"
#define QUIZ_PROPS \
	"\"question\":{\"type\":\"string\"},\"options\":" OPT_SCHEMA ",\"multiSelect\":{\"type\":\"boolean\"}," \
	"\"explanation\":{\"type\":\"string\"},\"correctAnswer\":{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"array\",\"items\":{\"type\":\"string\"}}]}," \
	"\"details\":{\"type\":\"string\"}"
#define EXPLAIN_PROPS \
	"\"question\":{\"type\":\"string\"},\"correctAnswer\":{\"type\":\"string\",\"description\":\"The FULL correct answer in your own words, reasoning included, " \
	"committed before he sees the question; grade his answer strictly against it, never revise it.\"},\"details\":{\"type\":\"string\"}"
/* The required pair leads: fields generated first are dropped least. */
#define TEACH_PROPS \
	"\"type\":{\"type\":\"string\",\"enum\":[\"predict\",\"check\"],\"description\":\"REQUIRED, a bare word: predict = posed BEFORE you teach the step " \
	"(its own turn; arrives open); check = posed AFTER the step's teaching (arrives collapsed behind 'Check your understanding'). ONE question per turn.\"}," \
	"\"nodeId\":{\"type\":\"string\",\"description\":\"REQUIRED plan node this question is about — EXACTLY one of this session's plan_init slugs (validated; " \
	"a mismatch is rejected). The lesson's position is the latest tagged node; stay on a node as long as understanding needs. Off-map checks use ask_user_question.\"},"

#define QUIZ_DESC \
	"Ask the learner ONE graded multiple-choice question and wait for the answer. Ask, then end your turn. " \
	"Options are bare claims (reasoning goes in `explanation`, shown after he answers; write it BEFORE correctAnswer). " \
	"`correctAnswer` is the option `value` (default: label), an array only with multiSelect. Never add an 'I don't know' option — it is added for you. " \
	"Returns verdict, picked, confidence (his stated % that he is right) and optional thinking."
#define EXPLAIN_DESC \
	"Ask the learner to EXPLAIN something in his own words — a graded check with NO options; he types free text. " \
	"THE COMMITMENT RULE: correctAnswer is the FULL correct answer, reasoning included, committed before he sees the question; " \
	"grade his answer strictly against it in your next reply — never revise it after seeing his words. He sees it on submit. " \
	"ONE question per turn: ask, then end your turn. Returns his text (or dontKnow) and optional thinking."
#define TEACH_DESC \
	" Every call declares `type` (predict|check) and `nodeId` (a plan_init slug, validated); checks may also return thinkingWhileReading."
#define ASK_DESC \
	"Ask the learner ONE ungraded question (preferences, goals, direction) and wait. Omit `options` for free text; with options he can always pick Other. " \
	"If you recommend an option, list it first with (Recommended) appended."
#define ASK_SCHEMA \
	"{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\"},\"details\":{\"type\":\"string\"},\"options\":" \
	"{\"type\":\"array\",\"minItems\":2,\"items\":{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"}},\"required\":[\"label\"]}}," \
	"\"multiSelect\":{\"type\":\"boolean\"}},\"required\":[\"question\"]}"

#define T_QUIZ { "quiz", QUIZ_DESC, "{\"type\":\"object\",\"properties\":{" QUIZ_PROPS "},\"required\":[\"question\",\"options\",\"explanation\",\"correctAnswer\"]}", tool_quiz }
#define T_EXPLAIN { "explain", EXPLAIN_DESC, "{\"type\":\"object\",\"properties\":{" EXPLAIN_PROPS "},\"required\":[\"question\",\"correctAnswer\"]}", tool_explain }
#define T_QUIZ_TEACH { "quiz", QUIZ_DESC TEACH_DESC, "{\"type\":\"object\",\"properties\":{" TEACH_PROPS QUIZ_PROPS "}," \
	"\"required\":[\"type\",\"nodeId\",\"question\",\"options\",\"explanation\",\"correctAnswer\"]}", tool_quiz_teach }
#define T_EXPLAIN_TEACH { "explain", EXPLAIN_DESC TEACH_DESC, "{\"type\":\"object\",\"properties\":{" TEACH_PROPS EXPLAIN_PROPS "}," \
	"\"required\":[\"type\",\"nodeId\",\"question\",\"correctAnswer\"]}", tool_explain_teach }
#define T_ASK { "ask_user_question", ASK_DESC, ASK_SCHEMA, tool_ask }
#define T_PROBE_DONE { "probe_done", \
	"Conclude the probing phase, exactly once, with your assessment: one entry per dependency strand (name; where his edge sits — floor, ceiling, misconceptions, what to build from). " \
	"Refused while a question is open. Optional overall `summary` of his goal and edge.", \
	"{\"type\":\"object\",\"properties\":{\"assessment\":{\"type\":\"object\",\"properties\":{\"strands\":{\"type\":\"array\",\"items\":{\"type\":\"object\"," \
	"\"properties\":{\"strand\":{\"type\":\"string\"},\"assessment\":{\"type\":\"string\"}},\"required\":[\"strand\",\"assessment\"]}},\"summary\":{\"type\":\"string\"}}," \
	"\"required\":[\"strands\"]}},\"required\":[\"assessment\"]}", tool_probe_done }
#define T_PLAN_INIT { "plan_init", \
	"Write or REWRITE this session's plan map — the dependency graph the learner sees printed as soon as this succeeds. Call once right after the probe concludes, " \
	"to present the map; re-call whenever the map is wrong (the new block replaces the old tree). A mermaid graph block: one node per line (`slug[Short Title]` — " \
	"slugs are permanent lowercase ids: a-z, 0-9, hyphens/underscores), `-->` edges meaning 'from must be understood before to' (chains fine). " \
	"A map, not a syllabus — one reasoning step per node, compress. Every check names its node via `nodeId`; where the lesson stands derives from those tags. " \
	"Returns the valid node ids.", \
	"{\"type\":\"object\",\"properties\":{\"mermaid\":{\"type\":\"string\",\"description\":\"e.g. graph TD\\n  primitives[Number primitives] --> operators[Arithmetic operators] --> " \
	"expressions[Evaluating expressions]\\nIds are permanent lowercase slugs; titles are free single-line text; no subgraphs, styles, or classes.\"}},\"required\":[\"mermaid\"]}", \
	tool_plan_init }

#define T_LIST_SESSIONS { "list_sessions", \
	"List the sessions of the learner's persistent knowledge graph — every session that contributed a proposition, most recent first, with how many " \
	"propositions were born in it and its id (use it with list_propositions). Reading this first shows what he has already covered.", \
	"{\"type\":\"object\",\"properties\":{}}", tool_list_sessions }
#define T_LIST_PROPOSITIONS { "list_propositions", \
	"List the propositions BORN in one knowledge-graph session — what was covered there. Names only.", \
	"{\"type\":\"object\",\"properties\":{\"session\":{\"type\":\"string\",\"description\":\"The session's id exactly as list_sessions reported it\"}},\"required\":[\"session\"]}", \
	tool_list_propositions }
#define T_FIND_CANDIDATES { "find_candidates", \
	"Search the knowledge graph for propositions about an idea (word-overlap ranking over names, statements and aliases). Returns up to 5: [id] name — birth session.", \
	"{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The idea to look for, in plain words.\"}},\"required\":[\"query\"]}", \
	tool_find_candidates }
#define T_PROBLEM { "problem", \
	"Pose one practice problem. Ends the turn — the learner works in a thinking field; his thinking traces and answer commits arrive back as labeled " \
	"'Your thinking' messages. The modelSolution rides this call's arguments and stays hidden from the learner until you conclude (conclude shows it " \
	"verbatim — the reveal needs no new field). Only one problem may be active — conclude it before posing the next.", \
	"{\"type\":\"object\",\"properties\":{" \
	"\"statement\":{\"type\":\"string\",\"description\":\"REQUIRED — the problem as the learner sees it (markdown + LaTeX). Must be self-contained and solvable from the session's material. NEVER announces the target proposition.\"}," \
	"\"mode\":{\"type\":\"string\",\"enum\":[\"recall\",\"discrimination\",\"composition\"],\"description\":\"REQUIRED — the demand mode (bare word).\"}," \
	"\"target\":{\"type\":\"array\",\"minItems\":1,\"items\":{\"type\":\"string\"},\"description\":\"The proposition id(s) this problem exercises, from the session context's [id] list — include when the material is in the map; OMIT entirely when it is not. An unknown proposition is rejected.\"}," \
	"\"modelSolution\":{\"type\":\"string\",\"description\":\"REQUIRED — the full solution, moves in order, committed BEFORE the learner sees the problem. This IS the reveal — conclude shows it verbatim; never re-author it.\"}}," \
	"\"required\":[\"statement\",\"mode\",\"modelSolution\"]}", tool_problem }
#define T_CONCLUDE { "conclude", \
	"Conclude the active problem: records the outcome + verdict and reveals the committed solution. After the result, say your closing words about the " \
	"learner's work if useful, then END YOUR TURN — no further tool calls. The next problem is NEVER posed here: it waits for the learner's explicit go-ahead " \
	"(a [NEXT PROBLEM] message, or his own words asking for the next one). Nothing may conclude unless a problem is active.", \
	"{\"type\":\"object\",\"properties\":{" \
	"\"outcome\":{\"type\":\"string\",\"enum\":[\"solved\",\"solvedWithHints\",\"movedOn\",\"surrendered\"],\"description\":\"REQUIRED — bare word (dismissed is synthesized, never yours).\"}," \
	"\"verdict\":{\"type\":\"string\",\"description\":\"REQUIRED — the plain-language verdict: what was credited, where it broke. On a wrong answer: localize the break, do NOT give the correction.\"}," \
	"\"rung\":{\"anyOf\":[{\"type\":\"string\",\"enum\":[\"R0\",\"R1\",\"R2\",\"R3\",\"R4\",\"R5\"]},{\"type\":\"null\"}],\"description\":\"REQUIRED — the hint-ladder rung the problem needed, or null = no hint was given.\"}," \
	"\"missClass\":{\"anyOf\":[{\"type\":\"string\",\"enum\":[\"cue\",\"discrimination\",\"execution\",\"careless\",\"structural\"]},{\"type\":\"null\"}],\"description\":\"REQUIRED — the miss classification, or null = no miss.\"}," \
	"\"retest\":{\"type\":\"boolean\",\"description\":\"REQUIRED — true marks the problem for a fresh-surface retest before the session ends.\"}}," \
	"\"required\":[\"outcome\",\"verdict\",\"rung\",\"missClass\",\"retest\"]}", tool_conclude }

static const tool_t PROBER_QUIZ[] = { T_QUIZ, T_ASK, T_PROBE_DONE, T_LIST_SESSIONS, T_LIST_PROPOSITIONS, T_FIND_CANDIDATES };
static const tool_t PROBER_EXPLAIN[] = { T_EXPLAIN, T_ASK, T_PROBE_DONE, T_LIST_SESSIONS, T_LIST_PROPOSITIONS, T_FIND_CANDIDATES };
static const tool_t PRACTICE[] = { T_PROBLEM, T_CONCLUDE, T_LIST_SESSIONS, T_LIST_PROPOSITIONS, T_FIND_CANDIDATES };
static const tool_t TEACHER_QUIZ[] = { T_QUIZ_TEACH, T_ASK, T_PLAN_INIT };
static const tool_t TEACHER_EXPLAIN[] = { T_EXPLAIN_TEACH, T_ASK, T_PLAN_INIT };

/* base + extra in one static array (built once per set). */
static const tool_t *joined(tool_t *out, int *built, const tool_t *base, int nb, const tool_t *extra, int ne, int *n) {
	if (!*built) { memcpy(out, base, sizeof *base * (size_t)nb); memcpy(out + nb, extra, sizeof *extra * (size_t)ne); *built = 1; }
	*n = nb + ne;
	return out;
}

const tool_t *tools_practice(int *n) {
	static tool_t all[16];
	static int built;
	int ne;
	const tool_t *jobs = jobs_practice_tools(&ne);
	return joined(all, &built, PRACTICE, (int)(sizeof PRACTICE / sizeof PRACTICE[0]), jobs, ne, n);
}

const tool_t *tools_prober(const char *mechanic, int *n) {
	*n = (int)(sizeof PROBER_QUIZ / sizeof PROBER_QUIZ[0]);
	return !strcmp(mechanic, "explain") ? PROBER_EXPLAIN : PROBER_QUIZ;
}

const tool_t *tools_teacher(const char *mechanic, int *n) {
	static tool_t q[16], e[16];
	static int bq, be;
	int ne;
	const tool_t *jobs = jobs_teacher_tools(&ne);
	return !strcmp(mechanic, "explain") ? joined(e, &be, TEACHER_EXPLAIN, 3, jobs, ne, n) : joined(q, &bq, TEACHER_QUIZ, 3, jobs, ne, n);
}
