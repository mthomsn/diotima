#ifndef SESSION_H
#define SESSION_H
#include <pthread.h>
#include "../vendor/cJSON.h"

typedef struct {
	char id[48];
	char path[1100];       /* chats/<id>.jsonl, one message per line */
	cJSON *messages;
	pthread_mutex_t mu;
	int open_questions;    /* quiz/ask calls currently waiting on the learner */
	int handoff;           /* probe_done succeeded: wake the teacher after this turn */
	int quit;              /* learner closed stdin */
} session_t;

extern session_t *S;

/* id NULL creates a new session. 0 on success. */
int session_open(session_t *s, const char *id);
void session_append(session_t *s, cJSON *msg);       /* takes ownership */
void session_append_user_text(session_t *s, const char *text);
char *session_history_text(session_t *s);             /* malloc'd transcript for the model */
#endif
