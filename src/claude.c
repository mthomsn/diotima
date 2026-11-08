#include "claude.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "math.h"
#include "session.h"
#include "store.h"
#include "term.h"

static volatile pid_t child = 0;

void claude_interrupt(void) { if (child > 0) kill(child, SIGTERM); }

static int streamed, col0 = 1;

static void raw_out(const char *s, size_t n) {
	fwrite(s, 1, n, stdout);
	col0 = s[n - 1] == '\n';
	fflush(stdout);
}

/* Model prose passes through the math filter: $…$ is shown typeset. */
static math_stream_t ms = { .out = raw_out, .bol = 1 };

static void out_text(const char *t) { if (t && *t) math_feed(&ms, t); }

static void ensure_newline(void) {
	math_flush(&ms);
	if (!col0) raw_out("\n", 1);
}

static const char *strip(const char *name) {
	size_t l = strlen(MCP_PREFIX);
	return !strncmp(name, MCP_PREFIX, l) ? name + l : name;
}

/* Flatten a tool_result's content (string or [{type:text,text}]) to one string. */
static char *flatten(const cJSON *c) {
	if (cJSON_IsString(c)) return strdup(c->valuestring);
	char *s = calloc(1, 1);
	const cJSON *p;
	cJSON_ArrayForEach(p, c) {
		const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(p, "text"));
		if (!t) continue;
		s = realloc(s, strlen(s) + strlen(t) + 2);
		if (*s) strcat(s, "\n");
		strcat(s, t);
	}
	return s;
}

static void on_event(const cJSON *ev, void *ctx) {
	(void)ctx;
	const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "type"));
	if (!type) return;
	if (!strcmp(type, "stream_event")) {
		const cJSON *e = cJSON_GetObjectItem(ev, "event"), *d = cJSON_GetObjectItem(e, "delta");
		const char *et = cJSON_GetStringValue(cJSON_GetObjectItem(e, "type")), *dt = cJSON_GetStringValue(cJSON_GetObjectItem(d, "type"));
		if (et && dt && !strcmp(et, "content_block_delta") && !strcmp(dt, "text_delta")) {
			pthread_mutex_lock(&term_mu);
			out_text(cJSON_GetStringValue(cJSON_GetObjectItem(d, "text")));
			pthread_mutex_unlock(&term_mu);
			streamed = 1;
		}
	} else if (!strcmp(type, "assistant")) {
		const cJSON *content = cJSON_GetObjectItem(cJSON_GetObjectItem(ev, "message"), "content"), *b;
		cJSON *msg = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(msg, "content");
		cJSON_AddStringToObject(msg, "role", "assistant");
		pthread_mutex_lock(&term_mu);
		cJSON_ArrayForEach(b, content) {
			const char *bt = cJSON_GetStringValue(cJSON_GetObjectItem(b, "type"));
			if (!bt) continue;
			if (!strcmp(bt, "text")) {
				const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(b, "text"));
				if (!t || !*t) continue;
				if (!streamed) out_text(t);
				cJSON *nb = cJSON_CreateObject();
				cJSON_AddStringToObject(nb, "type", "text");
				cJSON_AddStringToObject(nb, "text", t);
				cJSON_AddItemToArray(arr, nb);
			} else if (!strcmp(bt, "tool_use")) {
				const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(b, "name"));
				if (nm && strncmp(nm, MCP_PREFIX, strlen(MCP_PREFIX))) { /* a CLI built-in (practice file reads): show it */
					const cJSON *in = cJSON_GetObjectItem(b, "input");
					const char *arg = cJSON_GetStringValue(cJSON_GetObjectItem(in, "file_path"));
					if (!arg) arg = cJSON_GetStringValue(cJSON_GetObjectItem(in, "pattern"));
					ensure_newline();
					printf("\033[2m· %s %s\033[0m\n", nm, arg ? arg : "");
					col0 = 1;
				}
				cJSON *nb = cJSON_CreateObject();
				cJSON_AddStringToObject(nb, "type", "tool_use");
				cJSON_AddStringToObject(nb, "id", cJSON_GetStringValue(cJSON_GetObjectItem(b, "id")));
				cJSON_AddStringToObject(nb, "name", strip(cJSON_GetStringValue(cJSON_GetObjectItem(b, "name"))));
				cJSON_AddItemToObject(nb, "input", cJSON_Duplicate(cJSON_GetObjectItem(b, "input"), 1));
				cJSON_AddItemToArray(arr, nb);
			}
		}
		ensure_newline();
		streamed = 0;
		pthread_mutex_unlock(&term_mu);
		if (cJSON_GetArraySize(arr) == 0) { cJSON_Delete(msg); return; }
		/* record first, then announce: a handler that runs sees its own call in the transcript */
		cJSON *calls = cJSON_Duplicate(arr, 1);
		session_append(S, msg);
		cJSON_ArrayForEach(b, calls) {
			const char *bt = cJSON_GetStringValue(cJSON_GetObjectItem(b, "type"));
			if (bt && !strcmp(bt, "tool_use")) {
				char full[200];
				snprintf(full, sizeof full, MCP_PREFIX "%s", cJSON_GetStringValue(cJSON_GetObjectItem(b, "name")));
				mcp_note_tool_use(MCP_CHAN_AGENT, cJSON_GetStringValue(cJSON_GetObjectItem(b, "id")), full, cJSON_GetObjectItem(b, "input"));
			}
		}
		cJSON_Delete(calls);
	} else if (!strcmp(type, "user")) {
		const cJSON *content = cJSON_GetObjectItem(cJSON_GetObjectItem(ev, "message"), "content"), *b;
		cJSON *msg = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(msg, "content");
		cJSON_AddStringToObject(msg, "role", "user");
		if (cJSON_IsArray(content))
			cJSON_ArrayForEach(b, content) {
				const char *bt = cJSON_GetStringValue(cJSON_GetObjectItem(b, "type"));
				if (!bt || strcmp(bt, "tool_result")) continue;
				cJSON *nb = cJSON_CreateObject();
				char *txt = flatten(cJSON_GetObjectItem(b, "content"));
				cJSON_AddStringToObject(nb, "type", "tool_result");
				cJSON_AddStringToObject(nb, "tool_use_id", cJSON_GetStringValue(cJSON_GetObjectItem(b, "tool_use_id")));
				if (cJSON_IsTrue(cJSON_GetObjectItem(b, "is_error"))) cJSON_AddTrueToObject(nb, "is_error");
				cJSON_AddStringToObject(nb, "text", txt);
				free(txt);
				cJSON_AddItemToArray(arr, nb);
			}
		if (cJSON_GetArraySize(arr)) session_append(S, msg); else cJSON_Delete(msg);
	} else if (!strcmp(type, "result")) {
		if (cJSON_IsTrue(cJSON_GetObjectItem(ev, "is_error"))) {
			pthread_mutex_lock(&term_mu);
			ensure_newline();
			fprintf(stderr, "dio: claude reported an error: %s\n", cJSON_GetStringValue(cJSON_GetObjectItem(ev, "result")) ?: "?");
			pthread_mutex_unlock(&term_mu);
		}
	}
}

void claude_announce(int chan, const cJSON *ev) {
	const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "type"));
	if (!type || strcmp(type, "assistant")) return;
	const cJSON *b;
	cJSON_ArrayForEach(b, cJSON_GetObjectItem(cJSON_GetObjectItem(ev, "message"), "content")) {
		const char *bt = cJSON_GetStringValue(cJSON_GetObjectItem(b, "type"));
		const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(b, "id")), *name = cJSON_GetStringValue(cJSON_GetObjectItem(b, "name"));
		if (bt && id && name && !strcmp(bt, "tool_use")) mcp_note_tool_use(chan, id, name, cJSON_GetObjectItem(b, "input"));
	}
}

int claude_run(const claude_job_t *j) {
	mcp_set_tools(j->chan, j->tools, j->ntools);
	char allowed[1024] = "";
	for (int i = 0; i < j->ntools; i++) {
		strncat(allowed, i ? "," MCP_PREFIX : MCP_PREFIX, sizeof allowed - strlen(allowed) - 1);
		strncat(allowed, j->tools[i].name, sizeof allowed - strlen(allowed) - 1);
	}
	if (j->builtin) {
		if (allowed[0]) strncat(allowed, ",", sizeof allowed - strlen(allowed) - 1);
		strncat(allowed, j->builtin, sizeof allowed - strlen(allowed) - 1);
	}
	char tmpl[] = "/tmp/diotima-run-XXXXXX", errpath[1100], rel[64];
	char *dir = mkdtemp(tmpl);
	snprintf(rel, sizeof rel, "tmp/%s.stderr", j->errname ? j->errname : "claude");
	store_path(errpath, sizeof errpath, rel);
	int in[2], out[2];
	if (!dir || pipe(in) || pipe(out)) return -1;
	pid_t pid = fork();
	if (pid == 0) {
		if (j->background) setpgid(0, 0); /* the terminal's ^C must not reach it */
		dup2(in[0], 0); dup2(out[1], 1);
		int ef = errpath[0] ? open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0600) : -1;
		if (ef >= 0) dup2(ef, 2);
		close(in[0]); close(in[1]); close(out[0]); close(out[1]);
		if (chdir(j->cwd ? j->cwd : dir)) _exit(127);
		setenv("CLAUDE_CODE_DISABLE_CLAUDE_MDS", "1", 1);
		setenv("MCP_TOOL_TIMEOUT", "1800000", 1);
		const char *model = getenv("DIOTIMA_MODEL");
		char *argv[32];
		int n = 0;
		argv[n++] = "claude"; argv[n++] = "-p";
		argv[n++] = "--output-format"; argv[n++] = "stream-json";
		argv[n++] = "--verbose"; argv[n++] = "--include-partial-messages";
		argv[n++] = "--mcp-config"; argv[n++] = (char *)mcp_config(j->chan); argv[n++] = "--strict-mcp-config";
		argv[n++] = "--tools"; argv[n++] = (char *)(j->builtin ? j->builtin : "");
		argv[n++] = "--allowedTools"; argv[n++] = allowed;
		argv[n++] = "--no-session-persistence"; argv[n++] = "--disable-slash-commands";
		argv[n++] = "--system-prompt-file"; argv[n++] = (char *)j->prompt_file;
		if (model) { argv[n++] = "--model"; argv[n++] = (char *)model; }
		char turns[16];
		if (j->max_turns > 0) { snprintf(turns, sizeof turns, "%d", j->max_turns); argv[n++] = "--max-turns"; argv[n++] = turns; }
		argv[n] = NULL;
		execvp("claude", argv);
		_exit(127);
	}
	close(in[0]); close(out[1]);
	if (j->pid) *j->pid = pid;
	/* feed the input, then close stdin so claude starts */
	const char *input = j->input ? j->input : "";
	size_t len = strlen(input), off = 0;
	while (off < len) { ssize_t w = write(in[1], input + off, len - off); if (w <= 0) break; off += (size_t)w; }
	close(in[1]);

	FILE *f = fdopen(out[0], "r");
	char *line = NULL;
	size_t cap = 0;
	while (getline(&line, &cap, f) > 0) {
		cJSON *ev = cJSON_Parse(line);
		if (ev) { j->on_event(ev, j->ctx); cJSON_Delete(ev); }
	}
	free(line);
	fclose(f);
	int st = 0;
	waitpid(pid, &st, 0);
	if (j->pid) *j->pid = 0;
	rmdir(dir);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static char launch_dir[1024];
__attribute__((constructor)) static void remember_cwd(void) { if (!getcwd(launch_dir, sizeof launch_dir)) launch_dir[0] = 0; }
const char *claude_launch_dir(void) { return launch_dir; }

int claude_turn(const char *prompt_file, const tool_t *tools, int ntools, const char *builtin) {
	char *history = session_history_text(S);
	streamed = 0;
	claude_job_t j = { .prompt_file = prompt_file, .tools = tools, .ntools = ntools, .chan = MCP_CHAN_AGENT,
		.builtin = builtin, .cwd = builtin && launch_dir[0] ? launch_dir : NULL,
		.input = history, .errname = "claude", .on_event = on_event, .pid = &child };
	int code = claude_run(&j);
	free(history);
	pthread_mutex_lock(&term_mu);
	ensure_newline();
	pthread_mutex_unlock(&term_mu);
	if (code && code != 128 + SIGTERM) {
		char errpath[1100];
		store_path(errpath, sizeof errpath, "tmp/claude.stderr");
		fprintf(stderr, "dio: claude exited with status %d (stderr: %s)\n", code, errpath);
	}
	return code;
}
