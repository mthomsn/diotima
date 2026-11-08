#include "extract.h"
#include <pthread.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "agent.h"
#include "claude.h"
#include "coverage.h"
#include "graph.h"
#include "session.h"
#include "store.h"
#include "term.h"

#define EXTRACTOR_TURNS 4
#define AUDITOR_TURNS 16

static pthread_mutex_t run_mu = PTHREAD_MUTEX_INITIALIZER, note_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t bg;
static int bg_running;           /* guarded by note_mu */
static volatile int foreground;  /* a foreground run is active: ^C aborts it */
static volatile int aborted;
static volatile pid_t stage_pid;
static char notes[4096];

static int budget(void) {
	const char *e = getenv("DIOTIMA_CHUNK_TOKENS");
	int b = e ? atoi(e) : 0;
	return b > 0 ? b : 4096;
}

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...) {
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	if (foreground) { printf("\033[2m%s\033[0m\n", line); fflush(stdout); return; }
	pthread_mutex_lock(&note_mu);
	strncat(notes, line, sizeof notes - strlen(notes) - 2);
	strcat(notes, "\n");
	pthread_mutex_unlock(&note_mu);
}

void extract_drain_notes(void) {
	pthread_mutex_lock(&note_mu);
	if (notes[0]) { printf("\033[2m%s\033[0m", notes); fflush(stdout); notes[0] = 0; }
	pthread_mutex_unlock(&note_mu);
}

void extract_interrupt(void) {
	if (!foreground) return;
	aborted = 1;
	if (stage_pid > 0) kill(stage_pid, SIGTERM);
}

/* ── the submit seam: one stage at a time, so plain statics suffice ────── */

static cJSON *cur_props;      /* the extractor's propositions (auditor input) */
static cJSON *cur_existing;   /* existing node ids */
static cJSON *cur_batch;      /* this stretch's message ids */
static cJSON *cur_nodes;      /* merged graph nodes (candidate search) */
static cJSON *submitted;      /* the accepted submission */

static const char *S_(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return cJSON_IsString(v) ? v->valuestring : NULL; }
static int in_arr(const cJSON *a, const char *s) { const cJSON *v; cJSON_ArrayForEach(v, a) if (cJSON_IsString(v) && !strcmp(v->valuestring, s)) return 1; return 0; }
static char *err_out(int *is_error, const char *fmt, const char *a) {
	*is_error = 1;
	size_t n = strlen(fmt) + (a ? strlen(a) : 0) + 1;
	char *m = malloc(n);
	snprintf(m, n, fmt, a ? a : "");
	return m;
}
static int blank(const char *s) { if (!s) return 1; for (; *s; s++) if (*s != ' ' && *s != '\n' && *s != '\t') return 0; return 1; }

static char *tool_submit_findings(const cJSON *a, int *is_error) {
	const cJSON *props = cJSON_GetObjectItem(a, "propositions"), *chains = cJSON_GetObjectItem(a, "chains"), *p, *c, *x;
	if (!cJSON_IsArray(props)) return err_out(is_error, "`propositions` must be an array (pass [] when there are none) — never a string.%s", NULL);
	if (!cJSON_IsArray(chains)) return err_out(is_error, "`chains` must be an array (pass [] when the stretch makes no arguments) — never omit it or send a string.%s", NULL);
	cJSON *ids = cJSON_CreateArray();
	char *bad = NULL;
	cJSON_ArrayForEach(p, props) {
		const char *id = S_(p, "id"), *mid = S_(p, "messageId");
		const cJSON *al = cJSON_GetObjectItem(p, "aliases");
		if (blank(id) || blank(S_(p, "name")) || blank(S_(p, "statement"))) { bad = err_out(is_error, "propositions: every entry needs a non-empty string `id`, `name` and `statement`.%s", NULL); break; }
		if (al && !cJSON_IsArray(al)) { bad = err_out(is_error, "propositions: \"%s\" — `aliases` must be an array of strings.", id); break; }
		if (blank(mid)) { bad = err_out(is_error, "propositions: \"%s\" is missing its messageId — copy the message id from the brackets of the passage (TEACHER [id]) that most directly states or explains it.", id); break; }
		if (!in_arr(cur_batch, mid)) { bad = err_out(is_error, "propositions: \"%s\" names a messageId that is not a passage of THIS stretch — copy the id from the brackets of one of this stretch's own TEACHER [id] passages.", id); break; }
		cJSON_AddItemToArray(ids, cJSON_CreateString(id));
	}
	if (!bad) cJSON_ArrayForEach(c, chains) {
		const cJSON *pr = cJSON_GetObjectItem(c, "premises");
		const char *concl = S_(c, "conclusion");
		if (!cJSON_IsArray(pr) || cJSON_GetArraySize(pr) == 0) { bad = err_out(is_error, "chains: premises must name at least one proposition — a proposition with no argument stays in the propositions list alone.%s", NULL); break; }
		const char *unknown = concl && (in_arr(ids, concl) || in_arr(cur_existing, concl)) ? NULL : concl ? concl : "(missing conclusion)";
		cJSON_ArrayForEach(x, pr) if (!unknown && (!cJSON_IsString(x) || (!in_arr(ids, x->valuestring) && !in_arr(cur_existing, x->valuestring)))) unknown = cJSON_IsString(x) ? x->valuestring : "(non-string)";
		if (unknown) { bad = err_out(is_error, "chains: \"%s\" is neither a submitted proposition id nor an existing idea id from THE GRAPH SO FAR — every premise and conclusion must resolve.", unknown); break; }
		if (in_arr(pr, concl)) { bad = err_out(is_error, "chains: a conclusion cannot be its own premise.%s", NULL); break; }
	}
	cJSON_Delete(ids);
	if (bad) return bad;
	cJSON_Delete(submitted);
	submitted = cJSON_Duplicate(a, 1);
	return strdup("Findings submitted. End your turn.");
}

static char *tool_find_candidates(const cJSON *a, int *is_error) {
	const cJSON *reqs = cJSON_GetObjectItem(a, "propositions");
	if (!cJSON_IsArray(reqs) || cJSON_GetArraySize(reqs) == 0) return err_out(is_error, "`propositions` must be a non-empty array of {id, hints?, offset?}.%s", NULL);
	char err[512];
	cJSON *r = graph_candidates(cur_nodes, cur_props, reqs, err, sizeof err);
	if (!r) return err_out(is_error, "%s", err);
	char *s = cJSON_PrintUnformatted(r);
	cJSON_Delete(r);
	return s;
}

static char *tool_submit_review(const cJSON *a, int *is_error) {
	const cJSON *links = cJSON_GetObjectItem(a, "links"), *l;
	if (!cJSON_IsArray(links)) return err_out(is_error, "`links` must be an array (pass [] when nothing matches) — never omit it or send a string.%s", NULL);
	cJSON_ArrayForEach(l, links) {
		const char *id = S_(l, "id"), *to = S_(l, "linkTo");
		int known = 0;
		const cJSON *p;
		cJSON_ArrayForEach(p, cur_props) if (id && S_(p, "id") && !strcmp(S_(p, "id"), id)) known = 1;
		if (!known) return err_out(is_error, "links: \"%s\" is not a finding id — use the ids of the proposed findings.", id);
		if (!to || !in_arr(cur_existing, to)) return err_out(is_error, "links: linkTo \"%s\" is not an existing record — link only to candidate ids you have seen.", to);
	}
	cJSON_Delete(submitted);
	submitted = cJSON_Duplicate(a, 1);
	return strdup("Review submitted. End your turn.");
}

#define ENDPOINT(role) "{\"type\":\"string\",\"description\":\"The id this " role " — EITHER an existing graph idea id from THE GRAPH SO FAR in the prompt, verbatim (\\\"n_…\\\"), OR the local id of a proposition in this submission (\\\"p1\\\", …).\"}"
static const tool_t EXTRACTOR_TOOLS[] = {{ "submit_findings", "Submit the session findings.",
	"{\"type\":\"object\",\"properties\":{"
	"\"propositions\":{\"type\":\"array\",\"description\":\"Only the NEW core ideas this stretch contributes — ideas the prompt's THE GRAPH SO FAR already holds are referenced by id in chains, never restated here.\","
	"\"items\":{\"type\":\"object\",\"properties\":{"
	"\"id\":{\"type\":\"string\",\"description\":\"Local id: \\\"p1\\\", \\\"p2\\\", … — referenced by chains in this call.\"},"
	"\"name\":{\"type\":\"string\",\"description\":\"A very short label — established terminology, the term the discipline itself uses, a glossary headword a textbook would print. Not a sentence, needn't be unique or self-contained: this becomes the node's global label in the graph.\"},"
	"\"statement\":{\"type\":\"string\",\"description\":\"One true, self-contained, world-facing core idea at full precision. Never about the learner, the teacher, or the reading: not \\\"the learner understands X\\\" but \\\"X\\\". A misconception enters as its TRUE correction.\"},"
	"\"aliases\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Verbatim phrasings the source used for the same proposition, if any.\"},"
	"\"messageId\":{\"type\":\"string\",\"description\":\"The message id of the ONE passage in this stretch that most directly states or explains this proposition — copy it EXACTLY from that passage's \\\"TEACHER [id]\\\" label.\"}},"
	"\"required\":[\"id\",\"name\",\"statement\",\"messageId\"]}},"
	"\"chains\":{\"type\":\"array\",\"description\":\"Every argument this stretch makes — including arguments that join new ideas to established ones; conclusions reusable by id as premises of later chains; bare ideas (no argument) ride the propositions list alone; pass [] when none — never omit the field.\","
	"\"items\":{\"type\":\"object\",\"properties\":{\"premises\":{\"type\":\"array\",\"minItems\":1,\"items\":" ENDPOINT("premise uses") ",\"description\":\"Premise ids used TOGETHER: they jointly entail the conclusion; independent reasons are separate chains.\"},"
	"\"conclusion\":" ENDPOINT("conclusion is") "},\"required\":[\"premises\",\"conclusion\"]}}},"
	"\"required\":[\"propositions\",\"chains\"]}", tool_submit_findings }};

static const tool_t AUDITOR_TOOLS[] = {
	{ "find_candidates",
	  "Find existing proposition candidates for a batch of proposed ids, using their full text and optional hints. Ranks word overlap across names, statements and aliases, "
	  "not exact phrases. The first page is already in your input. A search returns five ranked candidates; nextOffset continues that query. An unmatched finding can remain new; "
	  "a continuation is an option, not a requirement to keep searching.",
	  "{\"type\":\"object\",\"properties\":{\"propositions\":{\"type\":\"array\",\"minItems\":1,\"description\":\"Look up only findings needing a specific additional search. Include each id once.\","
	  "\"items\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"An id from the proposed findings; its name, statement and aliases are searched automatically.\"},"
	  "\"hints\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Optional alternative words or phrases to add to the search.\"},"
	  "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"A previous result's nextOffset, to continue that query (same hints).\"}},\"required\":[\"id\"]}}},\"required\":[\"propositions\"]}",
	  tool_find_candidates },
	{ "submit_review", "Submit the identity verdicts ONLY (links) — never the propositions themselves.",
	  "{\"type\":\"object\",\"properties\":{\"links\":{\"type\":\"array\",\"description\":\"Identity verdicts — which findings propositions ARE existing graph nodes. A proposition absent from here = create a new node. Pass [] when none — never omit the field.\","
	  "\"items\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\",\"description\":\"The proposition id from the findings (\\\"p1\\\", …).\"},"
	  "\"linkTo\":{\"type\":\"string\",\"description\":\"The id of the EXISTING graph node this proposition IS — your identity verdict. EQUIVALENCE BIAS: paraphrases, rewordings, and compressions of the same claim MERGE; only genuinely distinct propositions stay apart.\"}},"
	  "\"required\":[\"id\",\"linkTo\"]}}},\"required\":[\"links\"]}",
	  tool_submit_review },
};

/* ── running a stage ───────────────────────────────────────────────────── */

static void on_stage_event(const cJSON *ev, void *ctx) {
	(void)ctx;
	claude_announce(MCP_CHAN_GRAPH, ev);
}

/* Run one stage child; returns its submission (caller frees) or NULL. */
static cJSON *stage(agent_kind_t kind, const tool_t *tools, int n, int turns, const char *input) {
	cJSON_Delete(submitted);
	submitted = NULL;
	const char *pf = agent_prompt_file(kind);
	if (!pf) return NULL;
	char errname[32];
	snprintf(errname, sizeof errname, "%s", agent_name(kind));
	claude_job_t j = { .prompt_file = pf, .input = input, .errname = errname, .tools = tools, .ntools = n,
		.chan = MCP_CHAN_GRAPH, .background = 1, .max_turns = turns, .on_event = on_stage_event, .pid = &stage_pid };
	claude_run(&j);
	cJSON *out = submitted;
	submitted = NULL;
	return out;
}

static void first_user_text(const cJSON *messages, char *out, size_t n) {
	const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 0), "content"), 0), "text"));
	snprintf(out, n, "%s", t ? t : "");
}

/* Extract and merge every uncovered batch (all = ignore the threshold). */
static void pipeline(int all) {
	pthread_mutex_lock(&run_mu);
	pthread_mutex_lock(&S->mu);
	cJSON *messages = cJSON_Duplicate(S->messages, 1);
	pthread_mutex_unlock(&S->mu);
	cJSON *cov = graph_coverage();
	int n = 0, tokens = 0;
	slot_t *slots = coverage_uncovered(messages, cov, S->id, &n, &tokens);
	cJSON_Delete(cov);
	if (n == 0 || (!all && tokens < budget())) goto done;
	char title[4096];
	first_user_text(messages, title, sizeof title);
	int *starts = calloc((size_t)n + 1, sizeof(int)), nb = coverage_batches(slots, n, budget(), starts);
	merge_stats total = {0};
	int failed = 0;
	for (int b = 0; b < nb && !aborted; b++) {
		graph_t g = graph_load();
		cur_existing = cJSON_CreateArray();
		const cJSON *x;
		cJSON_ArrayForEach(x, g.nodes) cJSON_AddItemToArray(cur_existing, cJSON_CreateString(cJSON_GetStringValue(cJSON_GetObjectItem(x, "id"))));
		cur_batch = cJSON_CreateArray();
		for (int k = starts[b]; k < starts[b + 1]; k++) cJSON_AddItemToArray(cur_batch, cJSON_CreateString(slots[k].mid));
		char *view = coverage_view(messages, slots, starts[b], starts[b + 1], title), *digest = graph_digest(&g, S->id);
		size_t len = strlen(view) + strlen(digest) + 4;
		char *prompt = malloc(len);
		snprintf(prompt, len, "%s%s%s", digest, *digest ? "\n\n" : "", view);
		free(view); free(digest);
		cJSON *findings = stage(AGENT_EXTRACTOR, EXTRACTOR_TOOLS, 1, EXTRACTOR_TURNS, prompt);
		free(prompt);
		cJSON *review = NULL;
		if (findings) {
			cur_props = cJSON_CreateArray();
			cJSON *reqs = cJSON_CreateArray();
			const cJSON *p;
			cJSON_ArrayForEach(p, cJSON_GetObjectItem(findings, "propositions")) {
				cJSON *q = cJSON_CreateObject(), *r = cJSON_CreateObject();
				const char *pid = cJSON_GetStringValue(cJSON_GetObjectItem(p, "id"));
				int dup = 0;
				const cJSON *y;
				cJSON_ArrayForEach(y, cur_props) if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(y, "id")), pid)) dup = 1;
				if (dup) { cJSON_Delete(q); cJSON_Delete(r); continue; }
				cJSON_AddStringToObject(q, "id", pid);
				cJSON_AddStringToObject(q, "name", cJSON_GetStringValue(cJSON_GetObjectItem(p, "name")));
				cJSON_AddStringToObject(q, "statement", cJSON_GetStringValue(cJSON_GetObjectItem(p, "statement")));
				const cJSON *al = cJSON_GetObjectItem(p, "aliases");
				cJSON_AddItemToObject(q, "aliases", cJSON_IsArray(al) ? cJSON_Duplicate(al, 1) : cJSON_CreateArray());
				cJSON_AddItemToArray(cur_props, q);
				cJSON_AddStringToObject(r, "id", pid);
				cJSON_AddItemToArray(reqs, r);
			}
			cur_nodes = g.nodes;
			char err[256];
			cJSON *initial = cJSON_GetArraySize(reqs) ? graph_candidates(g.nodes, cur_props, reqs, err, sizeof err) : NULL;
			cJSON_Delete(reqs);
			/* nothing to link to: the auditor would only say "new" — skip its call */
			if (!initial || cJSON_GetArraySize(cJSON_GetObjectItem(initial, "candidates")) == 0) review = cJSON_Parse("{\"links\":[]}");
			else {
				cJSON *in = cJSON_CreateObject();
				cJSON_AddItemToObject(in, "propositions", cJSON_Duplicate(cur_props, 1));
				cJSON_AddItemToObject(in, "matches", cJSON_DetachItemFromObject(initial, "matches"));
				cJSON_AddItemToObject(in, "candidates", cJSON_DetachItemFromObject(initial, "candidates"));
				char *js = cJSON_Print(in);
				const char *head = "Review these proposed findings against the supplied existing records. matches points from finding ids to candidateIds; candidates contains those records:\n\n";
				char *ain = malloc(strlen(head) + strlen(js) + 1);
				strcpy(ain, head); strcat(ain, js);
				review = stage(AGENT_AUDITOR, AUDITOR_TOOLS, 2, AUDITOR_TURNS, ain);
				free(ain); free(js); cJSON_Delete(in);
			}
			cJSON_Delete(initial);
			cur_nodes = NULL;
		}
		if (findings && review && !aborted) {
			merge_stats st;
			graph_merge(S->id, findings, cJSON_GetObjectItem(review, "links"), slots + starts[b], starts[b + 1] - starts[b], &st);
			total.nodes += st.nodes; total.linked += st.linked; total.chains += st.chains; total.dropped += st.dropped;
		} else failed = 1;
		cJSON_Delete(findings); cJSON_Delete(review);
		cJSON_Delete(cur_props); cJSON_Delete(cur_existing); cJSON_Delete(cur_batch);
		cur_props = cur_existing = cur_batch = NULL;
		graph_free(&g);
		if (failed) break;
	}
	free(starts);
	if (total.nodes || total.linked || total.chains) {
		char linked[64] = "";
		if (total.linked) snprintf(linked, sizeof linked, " (%d linked to earlier sessions)", total.linked);
		note("· graph: +%d proposition%s, %d chain%s%s", total.nodes, total.nodes == 1 ? "" : "s", total.chains, total.chains == 1 ? "" : "s", linked);
	}
	if (aborted) note("· graph: extraction skipped; the rest waits for the next run");
	else if (failed) note("· graph: extraction failed (stage stderr in %s/tmp/); the prose stays uncovered for the next run", store_root());
done:
	free(slots);
	cJSON_Delete(messages);
	pthread_mutex_unlock(&run_mu);
}

static void *bg_main(void *arg) {
	(void)arg;
	pipeline(0);
	pthread_mutex_lock(&note_mu);
	bg_running = 0;
	pthread_mutex_unlock(&note_mu);
	return NULL;
}

void extract_maybe_start(void) {
	pthread_mutex_lock(&note_mu);
	int busy = bg_running;
	pthread_mutex_unlock(&note_mu);
	if (busy) return;
	/* cheap check here so a below-budget session never spawns a thread */
	pthread_mutex_lock(&S->mu);
	cJSON *cov = graph_coverage();
	int n = 0, tokens = 0;
	free(coverage_uncovered(S->messages, cov, S->id, &n, &tokens));
	pthread_mutex_unlock(&S->mu);
	cJSON_Delete(cov);
	if (tokens < budget()) return;
	pthread_mutex_lock(&note_mu);
	if (bg_running) { pthread_mutex_unlock(&note_mu); return; }
	bg_running = 1;
	pthread_mutex_unlock(&note_mu);
	if (bg) pthread_join(bg, NULL);
	pthread_create(&bg, NULL, bg_main, NULL);
}

static int run_foreground(void) {
	pthread_mutex_lock(&S->mu);
	cJSON *cov = graph_coverage();
	int n = 0, tokens = 0;
	free(coverage_uncovered(S->messages, cov, S->id, &n, &tokens));
	pthread_mutex_unlock(&S->mu);
	cJSON_Delete(cov);
	if (!n) return 0;
	printf("\033[2m· updating the knowledge graph (%d tokens of prose; ^C to skip)…\033[0m\n", tokens);
	fflush(stdout);
	aborted = 0;
	foreground = 1;
	pipeline(1);
	foreground = 0;
	return aborted ? 1 : 0;
}

void extract_finish(void) {
	if (bg) { pthread_join(bg, NULL); bg = 0; }
	extract_drain_notes();
	run_foreground();
}

int extract_now(void) {
	int rc = run_foreground();
	extract_drain_notes();
	return rc;
}
