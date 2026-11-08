#include "store.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *store_root(void) {
	static char root[1024];
	const char *e = getenv("DIOTIMA_HOME"), *h = getenv("HOME");
	if (e && *e) snprintf(root, sizeof root, "%s", e);
	else snprintf(root, sizeof root, "%s/.diotima", h ? h : ".");
	return root;
}

static int mkdirs(char *path) {
	for (char *p = path + 1; *p; p++) {
		if (*p != '/') continue;
		*p = 0;
		if (mkdir(path, 0755) && errno != EEXIST) { *p = '/'; return -1; }
		*p = '/';
	}
	return 0;
}

int store_path(char *out, size_t n, const char *rel) {
	if (snprintf(out, n, "%s/%s", store_root(), rel) >= (int)n) return -1;
	return mkdirs(out);
}

char *store_read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	rewind(f);
	char *buf = malloc((size_t)len + 1);
	size_t got = fread(buf, 1, (size_t)len, f);
	buf[got] = 0;
	fclose(f);
	return buf;
}

int store_write_atomic(const char *path, const char *s) {
	char tmp[1100];
	snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
	FILE *f = fopen(tmp, "wb");
	if (!f) return -1;
	int ok = fputs(s, f) >= 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
	return 0;
}

int store_append_line(const char *path, const char *line) {
	FILE *f = fopen(path, "ab");
	if (!f) return -1;
	int ok = fputs(line, f) >= 0 && fputc('\n', f) != EOF;
	return fclose(f) == 0 && ok ? 0 : -1;
}

cJSON *store_read_jsonl(const char *path) {
	cJSON *arr = cJSON_CreateArray();
	char *txt = store_read_file(path);
	if (!txt) return arr;
	for (char *line = strtok(txt, "\n"); line; line = strtok(NULL, "\n")) {
		cJSON *j = cJSON_Parse(line);
		if (j) cJSON_AddItemToArray(arr, j);
	}
	free(txt);
	return arr;
}

int store_write_json(const char *path, const cJSON *j) {
	char *s = cJSON_Print(j);
	int rc = s ? store_write_atomic(path, s) : -1;
	free(s);
	return rc;
}

cJSON *store_read_json(const char *path) {
	char *txt = store_read_file(path);
	cJSON *j = txt ? cJSON_Parse(txt) : NULL;
	free(txt);
	return j;
}

const char *store_mechanic(void) {
	char p[1100];
	if (store_path(p, sizeof p, "config.json")) return "quiz";
	if (access(p, F_OK)) return "quiz";
	cJSON *c = store_read_json(p);
	if (!c) return NULL;
	const cJSON *m = cJSON_GetObjectItemCaseSensitive(c, "mechanic");
	const char *r = !m ? "quiz" : !cJSON_IsString(m) ? NULL : !strcmp(m->valuestring, "quiz") ? "quiz" : !strcmp(m->valuestring, "explain") ? "explain" : NULL;
	cJSON_Delete(c);
	return r;
}
