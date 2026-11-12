/* dio — a terminal learning harness. See README.md. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "agent.h"
#include "claude.h"
#include "extract.h"
#include "graph.h"
#include "img.h"
#include "jobs.h"
#include "math.h"
#include "proc.h"
#include "mcp.h"
#include "phase.h"
#include "plan.h"
#include "session.h"
#include "store.h"
#include "term.h"
#include "tools.h"

static void on_sigint(int sig) {
	(void)sig;
	claude_interrupt();
	extract_interrupt();
}

static void die(const char *msg) { fprintf(stderr, "dio: %s\n", msg); exit(1); }

static const char *mechanic = "quiz";

/* The learner-voiced steer that opens the teacher's first turn (the original's text). */
static const char *HANDOFF_TEXT =
	"Next: plan the full teaching arc from my current understanding to the goal understanding. "
	"Apply the teaching methodology in your system prompt, and find the optimal way to teach me this. "
	"Find the unconditional truths, plan the motivated derivations/discoveries, so every reasoning step has to land as obviously true. "
	"Take your time here and really think this through. "
	"When it's genuinely reasoned out, call plan_init and present the plan; I'll accept it or ask for changes.";

/* Delivered once: found in the transcript, so a resumed session never repeats it. */
static void ensure_handoff_steer(void) {
	const cJSON *m, *b;
	cJSON_ArrayForEach(m, S->messages)
		cJSON_ArrayForEach(b, cJSON_GetObjectItem(m, "content")) {
			const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(b, "text"));
			if (t && !strcmp(t, HANDOFF_TEXT)) return;
		}
	session_append_user_text(S, HANDOFF_TEXT);
}

/* One agent turn, picked from the transcript. Returns claude's exit code. */
static int run_turn(void) {
	int n;
	const tool_t *tools;
	agent_kind_t kind;
	const char *builtin = NULL;
	if (phase_is_practice(S->messages)) { kind = AGENT_PRACTICE; tools = tools_practice(&n); builtin = LEARNER_FILE_TOOLS; }
	else if (phase_of(S->messages) == PHASE_PROBE) { kind = AGENT_PROBER; tools = tools_prober(mechanic, &n); }
	else { ensure_handoff_steer(); kind = AGENT_TEACHER; tools = tools_teacher(mechanic, &n); builtin = LEARNER_FILE_TOOLS; }
	const char *pf = agent_prompt_file(kind);
	if (!pf) die("could not compose the system prompt (set DIOTIMA_PROMPTS?)");
	return claude_turn(pf, tools, n, builtin);
}

static int print_plan(void) {
	pthread_mutex_lock(&S->mu);
	cJSON *tree = phase_plan(S->messages);
	pthread_mutex_unlock(&S->mu);
	if (!tree) { printf("no plan yet\n"); return 1; }
	char *view = tools_plan_view(tree);
	printf("%s\n", view);
	free(view);
	cJSON_Delete(tree);
	return 0;
}

/* Practice model-facing texts (the original's). */
static const char *NEXT_PROBLEM_TEXT =
	"[NEXT PROBLEM] — the learner's explicit go-ahead: pose the next problem now per the selection rules, or wrap the session in text if the arc is complete.";

/* /skip: the learner discards the live problem — a synthesized conclude
 * (outcome dismissed) and its result, so the fold sees it concluded. */
static void dismiss_problem(const cJSON *problem) {
	char id[48];
	snprintf(id, sizeof id, "dismiss_%lx", (unsigned long)time(NULL));
	cJSON *call = cJSON_Parse("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"name\":\"conclude\",\"input\":"
		"{\"outcome\":\"dismissed\",\"verdict\":\"The learner dismissed the problem without concluding it.\",\"rung\":null,\"missClass\":null,\"retest\":false}}]}");
	cJSON_AddStringToObject(cJSON_GetArrayItem(cJSON_GetObjectItem(call, "content"), 0), "id", id);
	cJSON *res = cJSON_CreateObject(), *c = cJSON_AddArrayToObject(res, "content"), *b = cJSON_CreateObject();
	cJSON_AddStringToObject(res, "role", "user");
	cJSON_AddStringToObject(b, "type", "tool_result");
	cJSON_AddStringToObject(b, "tool_use_id", id);
	cJSON_AddStringToObject(b, "text", tools_conclude_ack());
	cJSON_AddItemToArray(c, b);
	printf("\033[1m── dismissed ──\033[0m\n");
	tools_show_solution(problem);
	session_append(S, call);
	session_append(S, res);
}

/* Practice input. Returns 1 when a message was appended or a job result
 * delivered (run a turn), 0 on quit. */
static int practice_input(void) {
	for (;;) {
		pthread_mutex_lock(&S->mu);
		const cJSON *p = phase_active_problem(S->messages);
		cJSON *problem = p ? cJSON_Duplicate(p, 1) : NULL;
		pthread_mutex_unlock(&S->mu);
		char line[4096];
		if (!problem) {
			term_lock();
			int r = term_ask_wakeable("\nyou (enter = next problem)> ", line, sizeof line);
			term_unlock();
			if (r == 1) { if (jobs_deliver()) return 1; continue; }
			if (r < 0 || !strcmp(line, "/quit")) return 0;
			session_append_user_text(S, line[0] ? line : NEXT_PROBLEM_TEXT);
			return 1;
		}
		/* thinking: lines accumulate; a blank line sends */
		size_t cap = 4096, len = 0;
		char *buf = malloc(cap);
		buf[0] = 0;
		int eof = 0, skip = 0, woke = 0;
		term_lock();
		for (;;) {
			int r = len ? term_ask("        > ", line, sizeof line) : term_ask_wakeable("\nthinking> ", line, sizeof line);
			if (r == 1) { woke = 1; break; } /* a job result, with nothing typed yet */
			if ((eof = r < 0)) break;
			if (!len && !strcmp(line, "/quit")) { eof = 1; break; }
			if (!len && !strcmp(line, "/skip")) { skip = 1; break; }
			if (!line[0]) { if (len) break; continue; }
			size_t l = strlen(line);
			while (len + l + 2 > cap) buf = realloc(buf, cap *= 2);
			if (len) buf[len++] = '\n';
			memcpy(buf + len, line, l + 1);
			len += l;
		}
		if (skip) dismiss_problem(problem);
		term_unlock();
		cJSON_Delete(problem);
		if (woke) { free(buf); if (jobs_deliver()) return 1; continue; }
		if (skip) { free(buf); continue; }
		if (eof && !len) { free(buf); return 0; }
		char *msg = malloc(len + 32);
		snprintf(msg, len + 32, "Your thinking:\n%s", buf);
		session_append_user_text(S, msg);
		free(msg); free(buf);
		return 1;
	}
}

static void converse(void) {
	char line[4096];
	int practice = phase_is_practice(S->messages);
	for (;;) {
		int code = run_turn();
		if (S->quit) return;
		if (!practice) {
			extract_maybe_start();
			if (S->handoff) {
				S->handoff = 0;
				printf("\n── probe concluded ──\n");
				if (phase_of(S->messages) != PHASE_PROBE) { code = run_turn(); extract_maybe_start(); if (S->quit) return; }
			}
		}
		if (jobs_deliver()) continue; /* results that landed during the turn wake the agent now */
		if (code) fprintf(stderr, "dio: the turn failed; type a message to retry\n");
		if (practice) { if (!practice_input()) return; continue; }
		int woke = 0;
		for (;;) {
			term_lock();
			extract_drain_notes();
			int r = term_ask_wakeable("\nyou> ", line, sizeof line);
			term_unlock();
			if (r == 1) { if (jobs_deliver()) { woke = 1; break; } continue; }
			if (r < 0 || !strcmp(line, "/quit")) return;
			if (!strcmp(line, "/phase")) { printf("%s\n", phase_name(phase_of(S->messages))); continue; }
			if (!strcmp(line, "/plan")) { print_plan(); continue; }
			if (!strcmp(line, "/demo")) {
				const char *d = jobs_latest_demo();
				if (!d) printf("no demo yet\n");
				else { char *argv[] = { "open", (char *)d, NULL }; printf("%s\n", d); if (!getenv("DIOTIMA_NO_OPEN")) proc_run(argv, NULL, 0); }
				continue;
			}
			if (line[0]) break;
		}
		if (!woke) session_append_user_text(S, line);
	}
}

/* The practice opening: picks (every proposition born in the chosen graph
 * sessions, plus the chains among them) or a free-form topic. */
static char *practice_context(int argc, char **argv) {
	graph_t g = graph_load();
	cJSON *ids, *picked = cJSON_CreateArray();
	char *idx = tools_session_index(&g, &ids);
	char topic[4096] = "";
	int nids = cJSON_GetArraySize(ids), all_ids = argc > 0;
	for (int i = 0; i < argc; i++) {
		int hit = 0;
		const cJSON *x;
		cJSON_ArrayForEach(x, ids) if (!strcmp(x->valuestring, argv[i])) hit = 1;
		if (!hit) all_ids = 0;
	}
	if (all_ids) for (int i = 0; i < argc; i++) cJSON_AddItemToArray(picked, cJSON_CreateString(argv[i]));
	else if (argc) for (int i = 0; i < argc; i++) { if (i) strncat(topic, " ", sizeof topic - strlen(topic) - 1); strncat(topic, argv[i], sizeof topic - strlen(topic) - 1); }
	else {
		term_lock();
		if (nids) printf("Knowledge graph sessions:\n%s", idx);
		int eof = term_ask(nids ? "pick sessions (e.g. 1,3) or type a topic> " : "what do you want to practice?> ", topic, sizeof topic);
		term_unlock();
		if (eof || !topic[0]) { free(idx); cJSON_Delete(ids); cJSON_Delete(picked); graph_free(&g); return NULL; }
		int numeric = nids > 0;
		for (char *c = topic; *c; c++) if (!((*c >= '0' && *c <= '9') || *c == ',' || *c == ' ')) numeric = 0;
		if (numeric) {
			for (char *t = strtok(topic, ", "); t; t = strtok(NULL, ", ")) {
				int k = atoi(t);
				if (k < 1 || k > nids) { fprintf(stderr, "dio: no session %d\n", k); exit(1); }
				cJSON_AddItemToArray(picked, cJSON_CreateString(cJSON_GetArrayItem(ids, k - 1)->valuestring));
			}
			topic[0] = 0;
		}
	}
	free(idx);
	cJSON_Delete(ids);
	size_t cap = 1024, len = 0;
	char *out = malloc(cap);
	out[0] = 0;
#define ADD(...) do { char t_[8192]; int l_ = snprintf(t_, sizeof t_, __VA_ARGS__); while (len + (size_t)l_ + 1 > cap) out = realloc(out, cap *= 2); memcpy(out + len, t_, (size_t)l_ + 1); len += (size_t)l_; } while (0)
	if (!cJSON_GetArraySize(picked)) ADD("[Practice session] — the learner's topic, in his own words:\n%s", topic);
	else {
		ADD("[Session context] — picked from the learner's knowledge map when starting this session:\n\n"
			"Propositions (id — name: statement; the problem tool's target field uses these ids):\n");
		cJSON *in = cJSON_CreateArray();
		const cJSON *n, *c, *p;
		cJSON_ArrayForEach(n, g.nodes) {
			const char *b = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(n, "provenance"), 0), "chatId")), *id = cJSON_GetStringValue(cJSON_GetObjectItem(n, "id"));
			int want = 0;
			cJSON_ArrayForEach(p, picked) if (b && !strcmp(p->valuestring, b)) want = 1;
			if (!want) continue;
			cJSON_AddItemToArray(in, cJSON_CreateString(id));
			ADD("- [%s] %s: %s\n", id, cJSON_GetStringValue(cJSON_GetObjectItem(n, "name")), cJSON_GetStringValue(cJSON_GetObjectItem(n, "statement")));
		}
		int head = 0;
		cJSON_ArrayForEach(c, g.chains) {
			int inside = 0;
			const cJSON *x;
			cJSON_ArrayForEach(x, in) if (!strcmp(x->valuestring, cJSON_GetStringValue(cJSON_GetObjectItem(c, "conclusion")))) inside = 1;
			cJSON_ArrayForEach(p, cJSON_GetObjectItem(c, "premises")) { int ok = 0; cJSON_ArrayForEach(x, in) if (!strcmp(x->valuestring, p->valuestring)) ok = 1; if (!ok) inside = 0; }
			if (!inside) continue;
			if (!head++) ADD("\nReasoning chains among them (premises → conclusion):\n");
			/* premises by id, as names */
			int np = cJSON_GetArraySize(cJSON_GetObjectItem(c, "premises"));
			const char *pr[64];
			for (int k = 0; k < np && k < 64; k++) pr[k] = cJSON_GetArrayItem(cJSON_GetObjectItem(c, "premises"), k)->valuestring;
			for (int a = 0; a < np && a < 64; a++) for (int b2 = a + 1; b2 < np && b2 < 64; b2++) if (strcmp(pr[a], pr[b2]) > 0) { const char *t = pr[a]; pr[a] = pr[b2]; pr[b2] = t; }
			ADD("- ");
			for (int k = 0; k < np && k < 64; k++) {
				const char *nm = pr[k];
				cJSON_ArrayForEach(n, g.nodes) if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(n, "id")), pr[k])) { nm = cJSON_GetStringValue(cJSON_GetObjectItem(n, "name")); break; }
				ADD("%s%s", k ? ", " : "", nm);
			}
			const char *cn = cJSON_GetStringValue(cJSON_GetObjectItem(c, "conclusion"));
			cJSON_ArrayForEach(n, g.nodes) if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(n, "id")), cn)) { cn = cJSON_GetStringValue(cJSON_GetObjectItem(n, "name")); break; }
			ADD(" → %s\n", cn);
		}
		cJSON_Delete(in);
		if (len && out[len - 1] == '\n') out[--len] = 0;
	}
#undef ADD
	cJSON_Delete(picked);
	graph_free(&g);
	return out;
}

static int cmd_ls(void) {
	char **ids;
	int n = session_list(&ids);
	struct winsize ws;
	int width = isatty(1) && !ioctl(1, TIOCGWINSZ, &ws) && ws.ws_col ? ws.ws_col : 100;
	for (int i = 0; i < n; i++) {
		char title[512];
		int room = width - 7 - (int)strlen(ids[i]);  /* "%3d  " + id + "  " */
		session_title(ids[i], title, room < 21 ? 21 : room + 1 > (int)sizeof title ? sizeof title : (size_t)room + 1);
		printf("%3d  %s  %s\n", i + 1, ids[i], title);
	}
	session_list_free(ids, n);
	return 0;
}

/* argv[2] as the learner typed it (number, suffix, id, or nothing = newest) → open it. */
static void open_named(session_t *s, int argc, char **argv) {
	char id[48], err[256];
	if (session_resolve(argc > 2 ? argv[2] : NULL, id, sizeof id, err, sizeof err)) die(err);
	if (session_open(s, id)) die("no such session (see dio ls)");
}

int main(int argc, char **argv) {
	static session_t sess;
	const char *cmd = argc > 1 ? argv[1] : "help";
	math_display = img_show_tex;
	if (!strcmp(cmd, "ls")) return cmd_ls();
	if (!strcmp(cmd, "graph")) {
		graph_t g = graph_load();
		struct winsize ws;
		int tty = isatty(1), width = tty && !ioctl(1, TIOCGWINSZ, &ws) && ws.ws_col ? ws.ws_col : 100;
		char *view = graph_render(&g, tty && !getenv("NO_COLOR"), width);
		fputs(view, stdout);
		free(view);
		graph_free(&g);
		return 0;
	}
	if (!strcmp(cmd, "extract")) {
		open_named(&sess, argc, argv);
		S = &sess;
		if (mcp_start()) die("cannot start the tool server");
		signal(SIGINT, on_sigint);
		signal(SIGPIPE, SIG_IGN);
		int rc = extract_now();
		return rc;
	}
	if (!strcmp(cmd, "plan")) {
		open_named(&sess, argc, argv);
		S = &sess;
		return print_plan();
	}
	if (strcmp(cmd, "new") && strcmp(cmd, "resume") && strcmp(cmd, "practice")) {
		puts("usage: dio new [what you want to learn]\n       dio practice [graph-session-id… | topic]\n       dio resume [id]\n"
			"       dio plan [id]\n       dio graph\n       dio extract [id]\n       dio ls\n"
			"id: full id, a unique tail of one, or its number in dio ls; omitted = newest");
		return strcmp(cmd, "help") ? 1 : 0;
	}
	if (!(mechanic = store_mechanic())) die("config.json: \"mechanic\" must be \"quiz\" or \"explain\"");
	int resume = !strcmp(cmd, "resume");
	if (resume) open_named(&sess, argc, argv);
	else if (session_open(&sess, NULL)) die("cannot create session");
	S = &sess;
	if (mcp_start()) die("cannot start the tool server");
	signal(SIGINT, on_sigint);
	signal(SIGPIPE, SIG_IGN);
	if (!strcmp(cmd, "practice")) {
		char *ctx = practice_context(argc - 2, argv + 2);
		if (!ctx) return 0;
		session_append_user_text(S, ctx);
		free(ctx);
		printf("session %s\n", S->id);
	} else if (!resume) {
		char goal[4096] = "";
		for (int i = 2; i < argc; i++) { strncat(goal, i > 2 ? " " : "", sizeof goal - strlen(goal) - 1); strncat(goal, argv[i], sizeof goal - strlen(goal) - 1); }
		if (!goal[0]) {
			term_lock();
			int eof = term_ask("what do you want to learn?> ", goal, sizeof goal);
			term_unlock();
			if (eof || !goal[0]) return 0;
		}
		session_append_user_text(S, goal);
		printf("session %s\n", S->id);
	}
	if (resume) jobs_recover(); /* jobs a previous run left outstanding were aborted */
	converse();
	jobs_abort_all();
	if (!phase_is_practice(S->messages)) extract_finish(); /* practice writes nothing to the graph */
	return 0;
}
