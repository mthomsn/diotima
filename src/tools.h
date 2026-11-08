#ifndef TOOLS_H
#define TOOLS_H
#include "mcp.h"
/* Tool sets per agent kind; mechanic is "quiz" (multiple choice) or "explain" (free recall). */
const tool_t *tools_prober(const char *mechanic, int *n);
const tool_t *tools_teacher(const char *mechanic, int *n);
const tool_t *tools_practice(int *n);
/* Read-only CLI built-ins the teacher and practice agents may use on the learner's files. */
#define LEARNER_FILE_TOOLS "Read,Glob,Grep"

/* Pure authoring check for quiz input; returns NULL if fine, else a malloc'd problem list. */
char *quiz_problems(const cJSON *args);

/* The plan view with the session's derived position marked (malloc'd). */
char *tools_plan_view(const cJSON *tree);

/* Graph sessions, most recent first: "1. title (…) — id: x" lines (malloc'd);
 * ids_out (optional) receives the ids in the same order. */
#include "graph.h"
char *tools_session_index(const graph_t *g, cJSON **ids_out);
const char *tools_conclude_ack(void);
void tools_show_solution(const cJSON *problem);   /* caller holds term_mu */
#endif
