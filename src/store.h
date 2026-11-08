#ifndef STORE_H
#define STORE_H
#include <stddef.h>
#include "../vendor/cJSON.h"

/* Data root: $DIOTIMA_HOME or ~/.diotima. Result is a static buffer. */
const char *store_root(void);
/* Join root + rel into out; creates parent directories. 0 on success. */
int store_path(char *out, size_t n, const char *rel);

char *store_read_file(const char *path);                 /* malloc'd, NULL if missing */
int store_write_atomic(const char *path, const char *s); /* tmp + rename */
int store_append_line(const char *path, const char *line);
cJSON *store_read_jsonl(const char *path);               /* array; empty if missing; bad lines skipped */
int store_write_json(const char *path, const cJSON *j);
cJSON *store_read_json(const char *path);

/* The quizzing mechanic from <root>/config.json {"mechanic":"quiz"|"explain"}.
 * "quiz" when the file or key is absent; NULL when the value is invalid. */
const char *store_mechanic(void);
#endif
