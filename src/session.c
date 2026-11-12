#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
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

typedef struct { char *id; time_t mt; } ent_t;
static int newest_first(const void *a, const void *b) {
	const ent_t *x = a, *y = b;
	if (x->mt != y->mt) return x->mt < y->mt ? 1 : -1;
	return strcmp(y->id, x->id);
}

int session_list(char ***ids) {
	char dir[1100], p[1400];
	*ids = NULL;
	if (store_path(dir, sizeof dir, "chats/x")) return 0;
	dir[strlen(dir) - 2] = 0;
	DIR *d = opendir(dir);
	if (!d) return 0;
	ent_t *e = NULL;
	int n = 0, cap = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		size_t l = strlen(de->d_name);
		if (l <= 6 || strcmp(de->d_name + l - 6, ".jsonl")) continue;
		struct stat st;
		snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
		if (stat(p, &st)) continue;
		if (n == cap) e = realloc(e, sizeof *e * (cap = cap ? cap * 2 : 16));
		e[n].id = strndup(de->d_name, l - 6);
		e[n++].mt = st.st_mtime;
	}
	closedir(d);
	qsort(e, n, sizeof *e, newest_first);
	*ids = malloc(sizeof **ids * (n ? n : 1));
	for (int i = 0; i < n; i++) (*ids)[i] = e[i].id;
	free(e);
	return n;
}

void session_list_free(char **ids, int n) {
	for (int i = 0; i < n; i++) free(ids[i]);
	free(ids);
}

int session_resolve(const char *arg, char *out, size_t n, char *err, size_t en) {
	char **ids;
	int cnt = session_list(&ids), rc = -1;
	size_t al = arg ? strlen(arg) : 0;
	if (!cnt) { snprintf(err, en, "no sessions yet (start one with dio new)"); goto done; }
	if (!al) { snprintf(out, n, "%s", ids[0]); rc = 0; goto done; }
	if (strspn(arg, "0123456789") == al) {
		long k = strtol(arg, NULL, 10);
		if (k >= 1 && k <= cnt) { snprintf(out, n, "%s", ids[k - 1]); rc = 0; }
		else snprintf(err, en, "no session #%s: dio ls lists %d", arg, cnt);
		goto done;
	}
	int hit = -1, hits = 0;
	for (int i = 0; i < cnt; i++) {
		size_t il = strlen(ids[i]);
		if (!strcmp(ids[i], arg)) { hit = i; hits = 1; break; }
		if (il > al && !strcmp(ids[i] + il - al, arg)) { hit = i; hits++; }
	}
	if (hits == 1) { snprintf(out, n, "%s", ids[hit]); rc = 0; }
	else if (hits) snprintf(err, en, "%s matches %d sessions; type more of the id (see dio ls)", arg, hits);
	else snprintf(err, en, "no such session %s (see dio ls)", arg);
done:
	session_list_free(ids, cnt);
	return rc;
}

void session_title(const char *id, char *out, size_t n) {
	char rel[128], p[1100], *line = NULL;
	size_t cap = 0;
	out[0] = 0;
	snprintf(rel, sizeof rel, "chats/%s.jsonl", id);
	if (store_path(p, sizeof p, rel)) return;
	FILE *f = fopen(p, "r");
	if (!f) return;
	if (getline(&line, &cap, f) > 0) {
		cJSON *m = cJSON_Parse(line);
		const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(m, "content"), 0), "text"));
		if (t && !strncmp(t, "[Session context]", 17)) snprintf(out, n, "practice: from the knowledge map");
		else if (t && !strncmp(t, "[Practice session]", 18)) { const char *nl = strchr(t, '\n'); snprintf(out, n, "practice: %s", nl ? nl + 1 : ""); }
		else if (t) snprintf(out, n, "%s", t);
		cJSON_Delete(m);
	}
	free(line);
	fclose(f);
	for (char *c = out; *c; c++) if (*c == '\n' || *c == '\t') *c = ' ';
	size_t l = strlen(out), k = l;
	if (l + 1 < n) return;
	while (k && ((unsigned char)out[k - 1] & 0xC0) == 0x80) k--; /* clipped: drop a torn last character */
	if (k) {
		unsigned char c = (unsigned char)out[k - 1];
		size_t want = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
		if (l - (k - 1) < want) out[k - 1] = 0;
	}
}
