#include "coverage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"

static const char *role_of(const cJSON *m) { return cJSON_GetStringValue(cJSON_GetObjectItem(m, "role")); }

/* The view lines of message i, as a JSON array of strings (empty = invisible). */
static cJSON *view_lines(const cJSON *m, int i) {
	cJSON *lines = cJSON_CreateArray();
	const char *r = role_of(m);
	if (!r || strcmp(r, "assistant") || cJSON_IsTrue(cJSON_GetObjectItem(m, "failed"))) return lines;
	const cJSON *b;
	cJSON_ArrayForEach(b, cJSON_GetObjectItem(m, "content")) {
		const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(b, "type")), *txt = cJSON_GetStringValue(cJSON_GetObjectItem(b, "text"));
		if (!t || strcmp(t, "text") || !txt) continue;
		while (*txt == ' ' || *txt == '\n' || *txt == '\t' || *txt == '\r') txt++;
		size_t l = strlen(txt);
		while (l && (txt[l - 1] == ' ' || txt[l - 1] == '\n' || txt[l - 1] == '\t' || txt[l - 1] == '\r')) l--;
		if (!l) continue;
		char *s = malloc(l + 32);
		int h = snprintf(s, 32, "TEACHER [m%d] ", i);
		memcpy(s + h, txt, l);
		s[h + l] = 0;
		cJSON_AddItemToArray(lines, cJSON_CreateString(s));
		free(s);
	}
	return lines;
}

static int covered(const cJSON *entries, const char *chat, const char *fp) {
	const cJSON *e;
	cJSON_ArrayForEach(e, entries) {
		const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(e, "chatId")), *f = cJSON_GetStringValue(cJSON_GetObjectItem(e, "fp"));
		if (c && f && !strcmp(c, chat) && !strcmp(f, fp)) return 1;
	}
	return 0;
}

slot_t *coverage_uncovered(const cJSON *messages, const cJSON *entries, const char *chat_id, int *n, int *tokens) {
	int cap = cJSON_GetArraySize(messages) + 1, i = 0;
	slot_t *out = calloc((size_t)cap, sizeof *out);
	*n = 0;
	if (tokens) *tokens = 0;
	const cJSON *m;
	cJSON_ArrayForEach(m, messages) {
		cJSON *lines = view_lines(m, i);
		if (cJSON_GetArraySize(lines) > 0) {
			cJSON *pair = cJSON_CreateArray();
			cJSON_AddItemToArray(pair, cJSON_CreateString(role_of(m)));
			cJSON_AddItemToArray(pair, cJSON_Duplicate(lines, 1));
			char *js = cJSON_PrintUnformatted(pair);
			slot_t s = { .index = i };
			snprintf(s.mid, sizeof s.mid, "m%d", i);
			sha256_hex(js, strlen(js), s.fp);
			size_t chars = 0;
			const cJSON *l;
			cJSON_ArrayForEach(l, lines) chars += strlen(l->valuestring) + 2;
			s.tokens = (int)((chars - 2 + 3) / 4);
			if (!covered(entries, chat_id, s.fp)) { out[(*n)++] = s; if (tokens) *tokens += s.tokens; }
			free(js);
			cJSON_Delete(pair);
		}
		cJSON_Delete(lines);
		i++;
	}
	return out;
}

int coverage_batches(const slot_t *slots, int n, int budget, int *starts) {
	int nb = 0, cur = 0;
	for (int i = 0; i < n; i++) {
		if (i == 0 || cur + slots[i].tokens > budget) { starts[nb++] = i; cur = 0; }
		cur += slots[i].tokens;
	}
	starts[nb] = n;
	return nb;
}

char *coverage_view(const cJSON *messages, const slot_t *slots, int from, int to, const char *title) {
	size_t cap = 256, len = 0;
	char *out = malloc(cap);
	out[0] = 0;
#define PUT(s) do { size_t l_ = strlen(s); while (len + l_ + 1 > cap) out = realloc(out, cap *= 2); memcpy(out + len, s, l_ + 1); len += l_; } while (0)
	if (title && *title) { PUT("# "); PUT(title); PUT("\n\n"); }
	int first = 1;
	for (int k = from; k < to; k++) {
		cJSON *lines = view_lines(cJSON_GetArrayItem(messages, slots[k].index), slots[k].index);
		const cJSON *l;
		cJSON_ArrayForEach(l, lines) { if (!first) PUT("\n\n"); first = 0; PUT(l->valuestring); }
		cJSON_Delete(lines);
	}
#undef PUT
	return out;
}
