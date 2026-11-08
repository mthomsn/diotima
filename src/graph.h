#ifndef GRAPH_H
#define GRAPH_H
#include <stddef.h>
#include "../vendor/cJSON.h"
#include "coverage.h"

/* The persistent knowledge graph: append-only ledgers under <root>/graph/.
 *   nodes.jsonl   {id,name,statement,aliases,createdAt,provenance:[{chatId,messageId}]}
 *                 ids repeat (link-appends): the first line defines name/statement/
 *                 createdAt; later phrasings join aliases, provenance unions.
 *   chains.jsonl  {id,premises,conclusion,createdAt,provenance} — premises jointly
 *                 entail the conclusion; lines merge by (sorted premises, conclusion).
 *   coverage.jsonl {chatId,fp,tokens,foldedAt} — which prose has been extracted.
 * Not guaranteed acyclic (only plan trees are DAGs). */
typedef struct { cJSON *nodes, *chains; } graph_t;

graph_t graph_load(void);                    /* merged view of the ledgers */
graph_t graph_fold(const cJSON *node_lines, const cJSON *chain_lines);
void graph_free(graph_t *g);
cJSON *graph_coverage(void);                 /* coverage.jsonl rows */

/* Drop redundant same-conclusion chains (prefer fewest premises; ancestry
 * cover), greedy in (premise count, createdAt, key) order. New array. */
cJSON *graph_simplify(const cJSON *chain_lines);

/* "THE GRAPH SO FAR" for one source, or "" when it owns nothing (malloc'd). */
char *graph_digest(const graph_t *g, const char *source);

/* Ranked existing-node candidates (BM25 over name+statement / name+alias),
 * 5 per page. requests: [{id, hints?, offset?}] over propositions [{id,name,
 * statement,aliases}]. Returns {matches:[{propositionId,candidateIds,nextOffset}],
 * candidates:[{id,name,statement,matchedAliases}]} or NULL with err. */
cJSON *graph_candidates(const cJSON *nodes, const cJSON *propositions, const cJSON *requests, char *err, size_t errn);

typedef struct { int nodes, linked, chains, dropped; } merge_stats;

/* The deterministic merger: findings {propositions:[{id,name,statement,aliases?,
 * messageId}], chains:[{premises,conclusion}]} + links [{id,linkTo}] become ledger
 * appends; then the chain ledger is simplified; then slots are marked covered. */
int graph_merge(const char *chat_id, const cJSON *findings, const cJSON *links, const slot_t *slots, int nslots, merge_stats *st);

/* `dio graph`: propositions grouped by birth session as a tree, derivations inline
 * when they fit `width` cells (else beneath). With color, each session gets a colored
 * ● and premises from other sessions carry theirs; without, they read "↗ <title>". */
char *graph_render(const graph_t *g, int color, int width);
#endif
