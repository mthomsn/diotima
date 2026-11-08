#ifndef PHASE_H
#define PHASE_H
#include <stddef.h>
#include "../vendor/cJSON.h"

typedef enum { PHASE_PROBE, PHASE_PLAN, PHASE_TEACH } phase_t;

/* Message format (one per transcript line):
 *   {"role":"user"|"assistant","content":[blocks],"failed"?:true}
 *   block: {"type":"text","text"} | {"type":"tool_use","id","name","input"}
 *        | {"type":"tool_result","tool_use_id","is_error"?:bool,"ok"?:bool,"text"}
 * Phase is a pure fold over these events; nothing is stored. */
phase_t phase_of(const cJSON *messages);
const char *phase_name(phase_t p);

/* Position on the plan, derived from nodeId-tagged quiz/explain calls:
 * current = node of the latest tagged call; visited = every earlier tagged
 * node (sticky). Returns the current id (static buffer) or NULL. If
 * visited_out is non-NULL it receives a new array of node ids (caller frees). */
const char *phase_position(const cJSON *messages, const cJSON *tree, cJSON **visited_out);

/* The live plan: the latest non-failed plan_init call with a later clean
 * result, re-parsed from its mermaid. New tree or NULL (no plan yet).
 * With tree non-NULL, phase_position ignores tags naming no node of it. */
cJSON *phase_plan(const cJSON *messages);

/* 1 if a probe_done call has concluded (clean result matching its call). */
int phase_probe_concluded(const cJSON *messages);

/* Practice sessions: the transcript opens with a "[Session context]" or
 * "[Practice session]" user block (written by `dio practice`). */
int phase_is_practice(const cJSON *messages);

/* The live problem: the latest non-failed `problem` call with a clean result
 * and no later concluded `conclude`. Its input (borrowed) or NULL. */
const cJSON *phase_active_problem(const cJSON *messages);
#endif
