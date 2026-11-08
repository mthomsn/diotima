#ifndef COVERAGE_H
#define COVERAGE_H
#include "../vendor/cJSON.h"
/* Extraction coverage. The unit is the assistant's prose: each non-failed
 * assistant message's text blocks, one view line each "TEACHER [m<i>] text"
 * (i = transcript index, stable because the transcript is append-only).
 * fp = sha256 of JSON [role, lines]; tokens = ceil(chars/4). Messages with no
 * prose are invisible. "Uncovered" is derived: fingerprint the transcript,
 * diff against coverage.jsonl. Nothing else is stored. */
typedef struct { int index; char mid[16]; char fp[65]; int tokens; } slot_t;

/* Uncovered visible slots, oldest first (malloc'd; *n set). */
slot_t *coverage_uncovered(const cJSON *messages, const cJSON *entries, const char *chat_id, int *n, int *tokens);

/* Greedy consecutive batches under budget; a slot is never split and an
 * over-budget slot rides alone. Returns batch count; starts[k] indexes slots. */
int coverage_batches(const slot_t *slots, int n, int budget, int *starts);

/* The extractor's view of slots [from,to): "# title\n\nTEACHER [m3] ..." (malloc'd). */
char *coverage_view(const cJSON *messages, const slot_t *slots, int from, int to, const char *title);
#endif
