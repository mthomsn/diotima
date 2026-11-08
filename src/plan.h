#ifndef PLAN_H
#define PLAN_H
#include <stddef.h>
#include "../vendor/cJSON.h"

/* Validate a plan DAG: nodes [{id,title,summary?}], edges [{from,to}].
 * Returns 0 if valid; else -1 with an agent-readable message in err. */
int plan_validate(const cJSON *nodes, const cJSON *edges, char *err, size_t errn);

/* Build {nodes,edges,version:1}; NULL (and err filled) when invalid. */
cJSON *plan_create(const cJSON *nodes, const cJSON *edges, char *err, size_t errn);

/* 1 if id is a node of the plan tree. */
int plan_has_node(const cJSON *tree, const char *id);

/* Parse plan_init's mermaid subset and validate it into a tree. NULL on
 * failure with every problem (newline-separated, line-numbered) in err. */
cJSON *plan_from_mermaid(const char *src, char *err, size_t errn);

/* Indented terminal view (topological, indent = longest-path depth):
 * ● current, ✓ visited, ○ upcoming. malloc'd. */
char *plan_render(const cJSON *tree, const char *current, const cJSON *visited);
#endif
