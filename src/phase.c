#include "phase.h"
#include <string.h>
#include "plan.h"

static const char *s(const cJSON *o, const char *k) {
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int is_assistant(const cJSON *m) { const char *r = s(m, "role"); return r && !strcmp(r, "assistant"); }

static int tagged_quiz(const cJSON *b) {
	const char *n = s(b, "name");
	if (!n || (strcmp(n, "quiz") && strcmp(n, "explain"))) return 0;
	const char *id = s(cJSON_GetObjectItemCaseSensitive(b, "input"), "nodeId");
	return id && *id;
}

/* A concluded call: non-failed assistant call + a LATER clean result with its id. */
static int result_clean_for(const cJSON *messages, const cJSON *from_msg, const char *id) {
	int past = 0;
	const cJSON *m, *b;
	cJSON_ArrayForEach(m, messages) {
		if (m == from_msg) { past = 1; continue; }
		if (!past) continue;
		cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(m, "content")) {
			const char *t = s(b, "type"), *tu = s(b, "tool_use_id");
			if (!t || strcmp(t, "tool_result") || !tu || strcmp(tu, id)) continue;
			if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "is_error"))) return 0;
			if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(b, "ok"))) return 0;
			return 1;
		}
	}
	return 0;
}

/* Index of the message where probe concluded (the result message), or -1. */
static int probe_conclusion_index(const cJSON *messages) {
	int i = 0;
	const cJSON *m, *b;
	cJSON_ArrayForEach(m, messages) {
		if (is_assistant(m) && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "failed")))
			cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(m, "content")) {
				const char *t = s(b, "type"), *n = s(b, "name"), *id = s(b, "id");
				if (t && n && id && !strcmp(t, "tool_use") && !strcmp(n, "probe_done") && result_clean_for(messages, m, id)) {
					/* seam = the result message following this call */
					int j = i + 1;
					const cJSON *r = m->next;
					for (; r; r = r->next, j++) {
						const cJSON *rb;
						cJSON_ArrayForEach(rb, cJSON_GetObjectItemCaseSensitive(r, "content")) {
							const char *tu = s(rb, "tool_use_id");
							if (tu && !strcmp(tu, id)) return j;
						}
					}
				}
			}
		i++;
	}
	return -1;
}

int phase_probe_concluded(const cJSON *messages) { return probe_conclusion_index(messages) >= 0; }

phase_t phase_of(const cJSON *messages) {
	int seam = probe_conclusion_index(messages);
	if (seam < 0) return PHASE_PROBE;
	int i = 0;
	const cJSON *m, *b;
	cJSON_ArrayForEach(m, messages) {
		if (i > seam && is_assistant(m))
			cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(m, "content"))
				if (!strcmp(s(b, "type") ? s(b, "type") : "", "tool_use") && tagged_quiz(b)) return PHASE_TEACH;
		i++;
	}
	return PHASE_PLAN;
}

const char *phase_name(phase_t p) { return p == PHASE_PROBE ? "probe" : p == PHASE_PLAN ? "plan" : "teach"; }

const char *phase_position(const cJSON *messages, const cJSON *tree, cJSON **visited_out) {
	static char cur[128];
	cur[0] = 0;
	cJSON *visited = cJSON_CreateArray();
	const cJSON *m, *b;
	cJSON_ArrayForEach(m, messages) {
		if (!is_assistant(m)) continue;
		cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(m, "content")) {
			if (!tagged_quiz(b)) continue;
			const char *id = s(cJSON_GetObjectItemCaseSensitive(b, "input"), "nodeId");
			if (tree && !plan_has_node(tree, id)) continue; /* unknown to the live map: moves nothing */
			if (cur[0]) {
				int have = 0; const cJSON *v;
				cJSON_ArrayForEach(v, visited) if (!strcmp(v->valuestring, cur)) have = 1;
				if (!have && strcmp(cur, id)) cJSON_AddItemToArray(visited, cJSON_CreateString(cur));
			}
			strncpy(cur, id, sizeof cur - 1);
		}
	}
	if (visited_out) *visited_out = visited; else cJSON_Delete(visited);
	return cur[0] ? cur : NULL;
}

cJSON *phase_plan(const cJSON *messages) {
	const cJSON *m, *b, *latest = NULL;
	cJSON_ArrayForEach(m, messages) {
		if (!is_assistant(m) || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "failed"))) continue;
		cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(m, "content")) {
			const char *t = s(b, "type"), *n = s(b, "name"), *id = s(b, "id");
			if (t && n && id && !strcmp(t, "tool_use") && !strcmp(n, "plan_init") && result_clean_for(messages, m, id)) latest = b;
		}
	}
	if (!latest) return NULL;
	char err[64];
	return plan_from_mermaid(s(cJSON_GetObjectItemCaseSensitive(latest, "input"), "mermaid"), err, sizeof err);
}

int phase_is_practice(const cJSON *messages) {
	const cJSON *m = cJSON_GetArrayItem(messages, 0);
	const char *t = s(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(m, "content"), 0), "text");
	return t && (!strncmp(t, "[Session context]", 17) || !strncmp(t, "[Practice session]", 18));
}

const cJSON *phase_active_problem(const cJSON *messages) {
	const cJSON *m, *b, *active = NULL;
	cJSON_ArrayForEach(m, messages) {
		if (!is_assistant(m) || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "failed"))) continue;
		cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(m, "content")) {
			const char *t = s(b, "type"), *n = s(b, "name"), *id = s(b, "id");
			if (!t || !n || !id || strcmp(t, "tool_use") || !result_clean_for(messages, m, id)) continue;
			if (!strcmp(n, "problem")) active = cJSON_GetObjectItemCaseSensitive(b, "input");
			else if (!strcmp(n, "conclude")) active = NULL;
		}
	}
	return active;
}
