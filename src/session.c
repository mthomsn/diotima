#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "store.h"

session_t *S;

int session_open(session_t *s, const char *id) {
	memset(s, 0, sizeof *s);
	pthread_mutex_init(&s->mu, NULL);
	if (id) snprintf(s->id, sizeof s->id, "%s", id);
	else snprintf(s->id, sizeof s->id, "c%lx%04x", (unsigned long)time(NULL), (unsigned)(getpid() & 0xffff));
	char rel[128];
	snprintf(rel, sizeof rel, "chats/%s.jsonl", s->id);
	if (store_path(s->path, sizeof s->path, rel)) return -1;
	s->messages = store_read_jsonl(s->path);
	if (id && cJSON_GetArraySize(s->messages) == 0) return -1;
	return 0;
}

void session_append(session_t *s, cJSON *msg) {
	char *line = cJSON_PrintUnformatted(msg);
	pthread_mutex_lock(&s->mu);
	cJSON_AddItemToArray(s->messages, msg);
	if (line) store_append_line(s->path, line);
	pthread_mutex_unlock(&s->mu);
	free(line);
}

void session_append_user_text(session_t *s, const char *text) {
	cJSON *m = cJSON_CreateObject(), *c = cJSON_AddArrayToObject(m, "content"), *b = cJSON_CreateObject();
	cJSON_AddStringToObject(m, "role", "user");
	cJSON_AddStringToObject(b, "type", "text");
	cJSON_AddStringToObject(b, "text", text);
	cJSON_AddItemToArray(c, b);
	session_append(s, m);
}

typedef struct { char *p; size_t n, cap; } buf_t;
static void bput(buf_t *b, const char *s) {
	size_t l = strlen(s);
	if (b->n + l + 1 > b->cap) { b->cap = (b->n + l + 1) * 2; b->p = realloc(b->p, b->cap); }
	memcpy(b->p + b->n, s, l + 1);
	b->n += l;
}

char *session_history_text(session_t *s) {
	buf_t b = {0};
	bput(&b, "Conversation so far. You are the assistant; continue from the end.\n\n");
	pthread_mutex_lock(&s->mu);
	const cJSON *m, *blk;
	cJSON_ArrayForEach(m, s->messages) {
		const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(m, "role"));
		int user = role && !strcmp(role, "user");
		cJSON_ArrayForEach(blk, cJSON_GetObjectItem(m, "content")) {
			const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(blk, "type"));
			if (!t) continue;
			if (!strcmp(t, "text")) {
				bput(&b, user ? "[learner] " : "[assistant] ");
				bput(&b, cJSON_GetStringValue(cJSON_GetObjectItem(blk, "text")));
				bput(&b, "\n\n");
			} else if (!strcmp(t, "tool_use")) {
				char *in = cJSON_PrintUnformatted(cJSON_GetObjectItem(blk, "input"));
				bput(&b, "[assistant called ");
				bput(&b, cJSON_GetStringValue(cJSON_GetObjectItem(blk, "name")));
				bput(&b, "] ");
				bput(&b, in ? in : "{}");
				bput(&b, "\n\n");
				free(in);
			} else if (!strcmp(t, "tool_result")) {
				bput(&b, cJSON_IsTrue(cJSON_GetObjectItem(blk, "is_error")) ? "[tool error] " : "[tool result] ");
				bput(&b, cJSON_GetStringValue(cJSON_GetObjectItem(blk, "text")));
				bput(&b, "\n\n");
			}
		}
	}
	pthread_mutex_unlock(&s->mu);
	return b.p;
}
