#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/plan.h"
#include "../src/phase.h"
#include "../src/store.h"
#include "../src/sha256.h"
#include "../src/coverage.h"
#include "../src/graph.h"
#include "../src/math.h"
#include "../src/session.h"
#include <sys/stat.h>
#include <utime.h>

static int fails, runs;
#define CHECK(c) do { runs++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static cJSON *J(const char *s) { cJSON *j = cJSON_Parse(s); if (!j) { printf("bad json: %s\n", s); exit(2); } return j; }

static void plan_case(const char *nodes, const char *edges, const char *want_substr) {
	cJSON *n = J(nodes), *e = J(edges);
	char err[512] = "";
	int rc = plan_validate(n, e, err, sizeof err);
	if (want_substr) { CHECK(rc != 0); CHECK(strstr(err, want_substr) != NULL); if (!strstr(err, want_substr)) printf("  got: %s\n", err); }
	else { CHECK(rc == 0); if (rc) printf("  got: %s\n", err); }
	cJSON_Delete(n); cJSON_Delete(e);
}

static void test_plan(void) {
	const char *abc = "[{\"id\":\"a\",\"title\":\"A\"},{\"id\":\"b\",\"title\":\"B\"},{\"id\":\"c\",\"title\":\"C\"}]";
	plan_case(abc, "[{\"from\":\"a\",\"to\":\"b\"},{\"from\":\"b\",\"to\":\"c\"},{\"from\":\"a\",\"to\":\"c\"}]", NULL);
	plan_case(abc, "[]", NULL);
	plan_case(abc, "[{\"from\":\"a\",\"to\":\"a\"}]", "self-loop");
	plan_case(abc, "[{\"from\":\"a\",\"to\":\"b\"},{\"from\":\"a\",\"to\":\"b\"}]", "already exists");
	plan_case(abc, "[{\"from\":\"a\",\"to\":\"z\"}]", "unknown node id \"z\"");
	plan_case(abc, "[{\"from\":\"a\",\"to\":\"b\"},{\"from\":\"b\",\"to\":\"c\"},{\"from\":\"c\",\"to\":\"a\"}]", "cycle");
	plan_case("[{\"id\":\"a\",\"title\":\"A\"},{\"id\":\"a\",\"title\":\"B\"}]", "[]", "Duplicate node id");
	plan_case("[{\"id\":\"\",\"title\":\"A\"}]", "[]", "non-empty `id`");
	plan_case("[{\"id\":\"A B\",\"title\":\"A\"}]", "[]", "lowercase slug");
	plan_case("[{\"id\":\"a\"}]", "[]", "non-empty `title`");

	cJSON *n = J(abc), *e = J("[{\"from\":\"a\",\"to\":\"b\"}]");
	char err[256];
	cJSON *t = plan_create(n, e, err, sizeof err);
	CHECK(t != NULL);
	CHECK(plan_has_node(t, "b") && !plan_has_node(t, "q"));
	CHECK(cJSON_GetObjectItem(t, "version")->valueint == 1);
	cJSON_Delete(t); cJSON_Delete(n); cJSON_Delete(e);
}

static const char *PROBE_QUIZ = "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"q1\",\"name\":\"quiz\",\"input\":{\"question\":\"x\"}}]}";

static void test_phase(void) {
	cJSON *m = cJSON_CreateArray();
	CHECK(phase_of(m) == PHASE_PROBE);
	cJSON_AddItemToArray(m, J(PROBE_QUIZ)); /* untagged probe quiz does not advance */
	CHECK(phase_of(m) == PHASE_PROBE);
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"pd\",\"name\":\"probe_done\",\"input\":{}}]}"));
	CHECK(phase_of(m) == PHASE_PROBE); /* bare call: not concluded */
	cJSON_AddItemToArray(m, J("{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"pd\",\"is_error\":true,\"text\":\"refused\"}]}"));
	CHECK(phase_of(m) == PHASE_PROBE); /* errored */
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"pd2\",\"name\":\"probe_done\",\"input\":{}}]}"));
	cJSON_AddItemToArray(m, J("{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"pd2\",\"ok\":false,\"text\":\"no\"}]}"));
	CHECK(phase_of(m) == PHASE_PROBE); /* explicit ok:false */
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"pd3\",\"name\":\"probe_done\",\"input\":{}}]}"));
	cJSON_AddItemToArray(m, J("{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"pd3\",\"text\":\"ok\"}]}"));
	CHECK(phase_of(m) == PHASE_PLAN);
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"pl\",\"name\":\"plan_init\",\"input\":{}}]}"));
	CHECK(phase_of(m) == PHASE_PLAN); /* plan_init is not the teach seam */
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"quiz\",\"input\":{\"nodeId\":\"a\",\"type\":\"predict\"}}]}"));
	CHECK(phase_of(m) == PHASE_TEACH); /* the CALL is the seam, not its answer */

	cJSON *vis = NULL;
	const char *cur = phase_position(m, NULL, &vis);
	CHECK(cur && !strcmp(cur, "a") && cJSON_GetArraySize(vis) == 0);
	cJSON_Delete(vis);
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t2\",\"name\":\"explain\",\"input\":{\"nodeId\":\"b\"}}]}"));
	cur = phase_position(m, NULL, &vis);
	CHECK(cur && !strcmp(cur, "b") && cJSON_GetArraySize(vis) == 1 && !strcmp(vis->child->valuestring, "a"));
	cJSON_Delete(vis); cJSON_Delete(m);

	/* a failed probe_done generation never concludes */
	m = cJSON_CreateArray();
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"failed\":true,\"content\":[{\"type\":\"tool_use\",\"id\":\"pd\",\"name\":\"probe_done\",\"input\":{}}]}"));
	cJSON_AddItemToArray(m, J("{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"pd\",\"text\":\"ok\"}]}"));
	CHECK(phase_of(m) == PHASE_PROBE);
	cJSON_Delete(m);
}

static void test_store(void) {
	char dir[] = "/tmp/diotima-test-XXXXXX";
	CHECK(mkdtemp(dir) != NULL);
	setenv("DIOTIMA_HOME", dir, 1);
	char p[1200];
	CHECK(store_path(p, sizeof p, "chats/x/log.jsonl") == 0);
	CHECK(store_append_line(p, "{\"a\":1}") == 0);
	CHECK(store_append_line(p, "garbage") == 0);
	CHECK(store_append_line(p, "{\"a\":2}") == 0);
	cJSON *rows = store_read_jsonl(p);
	CHECK(cJSON_GetArraySize(rows) == 2);
	cJSON_Delete(rows);
	CHECK(store_path(p, sizeof p, "meta.json") == 0);
	cJSON *o = J("{\"k\":\"v\"}");
	CHECK(store_write_json(p, o) == 0);
	cJSON *back = store_read_json(p);
	CHECK(back && !strcmp(cJSON_GetObjectItem(back, "k")->valuestring, "v"));
	cJSON_Delete(o); cJSON_Delete(back);
	char *missing = store_read_file("/nonexistent/zzz");
	CHECK(missing == NULL);
	char cmd[1300];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	if (system(cmd)) {}
}

static int has_id(const cJSON *nodes, const char *id) {
	const cJSON *n;
	cJSON_ArrayForEach(n, nodes) if (!strcmp(cJSON_GetObjectItem(n, "id")->valuestring, id)) return 1;
	return 0;
}

/* The real graph is NOT guaranteed acyclic (unlike plan trees), so only
 * referential integrity is asserted: every chain endpoint is a node. */
static void test_golden(void) {
	cJSON *nodes = store_read_jsonl("/Users/matt/learnharness/tests/fixtures/golden-data/graph/nodes.jsonl");
	cJSON *chains = store_read_jsonl("/Users/matt/learnharness/tests/fixtures/golden-data/graph/chains.jsonl");
	if (cJSON_GetArraySize(nodes) == 0) { printf("skip golden (fixtures absent)\n"); return; }
	int dangling = 0;
	const cJSON *c, *p;
	cJSON_ArrayForEach(c, chains) {
		if (!has_id(nodes, cJSON_GetObjectItem(c, "conclusion")->valuestring)) dangling++;
		cJSON_ArrayForEach(p, cJSON_GetObjectItem(c, "premises")) if (!has_id(nodes, p->valuestring)) dangling++;
	}
	CHECK(dangling == 0);
	printf("golden: %d nodes, %d chains, %d dangling\n", cJSON_GetArraySize(nodes), cJSON_GetArraySize(chains), dangling);
	cJSON_Delete(nodes); cJSON_Delete(chains);
}

static cJSON *mer(const char *src, char *err) { err[0] = 0; return plan_from_mermaid(src, err, 2048); }

static void test_mermaid(void) {
	char err[2048];
	cJSON *t = mer("%% the map\ngraph TD\n  primitives[Number primitives] --> operators(\"Arithmetic operators\") --> expressions\n"
		"  expressions[Evaluating expressions]\n  a_b ==> expressions\n  a_b -.-> operators\n  primitives --> operators\n", err);
	CHECK(t != NULL);
	if (!t) printf("  got: %s\n", err);
	else {
		cJSON *n = cJSON_GetObjectItem(t, "nodes");
		CHECK(cJSON_GetArraySize(n) == 4);
		CHECK(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(n, 1), "title")->valuestring, "Arithmetic operators"));
		CHECK(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(n, 2), "title")->valuestring, "Evaluating expressions")); /* later label upgrades bare id */
		CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(t, "edges")) == 4); /* duplicate edge deduped */
		char *v = plan_render(t, "operators", J("[\"primitives\"]"));
		CHECK(strstr(v, "✓") && strstr(v, "●") && strstr(v, "← operators, a_b"));
		CHECK(strstr(v, "\n      ") != NULL); /* expressions sits at depth 2 */
		free(v);
		cJSON_Delete(t);
	}
	CHECK(!mer("graph TD\n  Bad.Id --> b\n  c --> c\n  x y z\n", err));
	CHECK(strstr(err, "line 2: node id `Bad.Id`") && strstr(err, "line 3: self-loop") && strstr(err, "line 4: can't parse"));
	CHECK(!mer("graph TD\n a --> b --> c\n c --> a\n", err) && strstr(err, "cycle"));
	CHECK(!mer("sequenceDiagram\n a --> b\n", err) && strstr(err, "line 1: expected the header"));
	CHECK(!mer("  \n%% nothing\n", err) && strstr(err, "empty"));
	CHECK(!mer(NULL, err) && strstr(err, "empty"));
	CHECK(!mer("flowchart LR\n", err) && strstr(err, "No nodes"));
	CHECK(!mer("graph TD\n a[x] --> b[y\n", err) && strstr(err, "can't read the edge"));
}

static void test_plan_derived(void) {
	cJSON *m = cJSON_CreateArray();
	CHECK(phase_plan(m) == NULL);
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"p1\",\"name\":\"plan_init\",\"input\":{\"mermaid\":\"graph TD\\n a --> b\"}}]}"));
	CHECK(phase_plan(m) == NULL); /* no result yet */
	cJSON_AddItemToArray(m, J("{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"p1\",\"text\":\"ok\"}]}"));
	cJSON *t = phase_plan(m);
	CHECK(t && plan_has_node(t, "b") && !plan_has_node(t, "c"));
	cJSON_Delete(t);
	/* a rejected rewrite leaves the earlier map live; a clean one replaces it */
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"p2\",\"name\":\"plan_init\",\"input\":{\"mermaid\":\"graph TD\\n a --> c\"}}]}"));
	cJSON_AddItemToArray(m, J("{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"p2\",\"is_error\":true,\"text\":\"no\"}]}"));
	t = phase_plan(m);
	CHECK(t && plan_has_node(t, "b") && !plan_has_node(t, "c"));
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"q1\",\"name\":\"quiz\",\"input\":{\"nodeId\":\"a\",\"type\":\"check\"}}]}"));
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"q2\",\"name\":\"quiz\",\"input\":{\"nodeId\":\"zzz\",\"type\":\"check\"}}]}"));
	cJSON *vis = NULL;
	const char *cur = phase_position(m, t, &vis);
	CHECK(cur && !strcmp(cur, "a") && cJSON_GetArraySize(vis) == 0); /* unknown tag moves nothing */
	cJSON_Delete(vis);
	cur = phase_position(m, NULL, NULL);
	CHECK(cur && !strcmp(cur, "zzz"));
	cJSON_Delete(t);
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"p3\",\"name\":\"plan_init\",\"input\":{\"mermaid\":\"graph TD\\n a --> c\"}}]}"));
	cJSON_AddItemToArray(m, J("{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"p3\",\"text\":\"ok\"}]}"));
	t = phase_plan(m);
	CHECK(t && plan_has_node(t, "c") && !plan_has_node(t, "b"));
	cJSON_Delete(t); cJSON_Delete(m);
}

static void test_mechanic(void) {
	char dir[] = "/tmp/diotima-test-XXXXXX", p[1200];
	CHECK(mkdtemp(dir) != NULL);
	setenv("DIOTIMA_HOME", dir, 1);
	CHECK(!strcmp(store_mechanic(), "quiz"));
	snprintf(p, sizeof p, "%s/config.json", dir);
	store_write_atomic(p, "{\"mechanic\":\"explain\"}");
	CHECK(!strcmp(store_mechanic(), "explain"));
	store_write_atomic(p, "{\"mechanic\":\"essay\"}");
	CHECK(store_mechanic() == NULL);
	store_write_atomic(p, "{}");
	CHECK(!strcmp(store_mechanic(), "quiz"));
	char cmd[1300];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	if (system(cmd)) {}
}

static void test_sha256(void) {
	char h[65];
	sha256_hex("", 0, h);
	CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	sha256_hex("abc", 3, h);
	CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	const char *q = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; /* 56 bytes: two padding blocks */
	sha256_hex(q, strlen(q), h);
	CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

static void test_coverage(void) {
	cJSON *m = J("[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"goal\"}]},"
		"{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"  Prose one.  \"},{\"type\":\"tool_use\",\"id\":\"a\",\"name\":\"quiz\",\"input\":{}}]},"
		"{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"a\",\"text\":\"x\"}]},"
		"{\"role\":\"assistant\",\"failed\":true,\"content\":[{\"type\":\"text\",\"text\":\"failed prose\"}]},"
		"{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"b\",\"name\":\"quiz\",\"input\":{}}]},"
		"{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Prose two, which is a fair bit longer than the first.\"}]}]");
	cJSON *none = cJSON_CreateArray();
	int n, tok;
	slot_t *s = coverage_uncovered(m, none, "c1", &n, &tok);
	CHECK(n == 2 && s[0].index == 1 && !strcmp(s[1].mid, "m5")); /* user, tool-only and failed messages are invisible */
	CHECK(s[0].tokens == (int)((strlen("TEACHER [m1] Prose one.") + 3) / 4));
	char *v = coverage_view(m, s, 0, n, "goal");
	CHECK(!strcmp(v, "# goal\n\nTEACHER [m1] Prose one.\n\nTEACHER [m5] Prose two, which is a fair bit longer than the first."));
	free(v);
	char row[200];
	snprintf(row, sizeof row, "[{\"chatId\":\"c1\",\"fp\":\"%s\"},{\"chatId\":\"other\",\"fp\":\"%s\"}]", s[0].fp, s[1].fp);
	cJSON *cov = J(row);
	slot_t *s2 = coverage_uncovered(m, cov, "c1", &n, &tok);
	CHECK(n == 1 && s2[0].index == 5); /* covered by fingerprint, per chat */
	int starts[8];
	slot_t b[4] = { {.tokens = 3}, {.tokens = 3}, {.tokens = 9}, {.tokens = 1} };
	CHECK(coverage_batches(b, 4, 6, starts) == 3 && starts[1] == 2 && starts[2] == 3 && starts[3] == 4); /* over-budget rides alone */
	free(s); free(s2); cJSON_Delete(m); cJSON_Delete(none); cJSON_Delete(cov);
}

static cJSON *chain(const char *id, const char *prem, const char *concl, const char *at) {
	char b[300];
	snprintf(b, sizeof b, "{\"id\":\"%s\",\"premises\":%s,\"conclusion\":\"%s\",\"createdAt\":\"%s\",\"provenance\":[{\"chatId\":\"c\",\"messageId\":\"\"}]}", id, prem, concl, at);
	return J(b);
}
static int has_chain(const cJSON *arr, const char *id) { const cJSON *c; cJSON_ArrayForEach(c, arr) if (!strcmp(cJSON_GetObjectItem(c, "id")->valuestring, id)) return 1; return 0; }

static void test_graph(void) {
	/* fold: first line defines; later phrasings join aliases; provenance unions */
	cJSON *nl = J("[{\"id\":\"n_a\",\"name\":\"A\",\"statement\":\"a1\",\"aliases\":[],\"provenance\":[{\"chatId\":\"c1\",\"messageId\":\"m1\"}]},"
		"{\"id\":\"n_a\",\"name\":\"A2\",\"statement\":\"a2\",\"aliases\":[\"a3\",\"a1\"],\"provenance\":[{\"chatId\":\"c2\",\"messageId\":\"m4\"},{\"chatId\":\"c1\",\"messageId\":\"m1\"}]}]");
	cJSON *cl = cJSON_CreateArray();
	cJSON_AddItemToArray(cl, chain("arg_1", "[\"n_b\",\"n_a\"]", "n_x", "1"));
	cJSON_AddItemToArray(cl, chain("arg_9", "[\"n_a\",\"n_b\"]", "n_x", "2"));
	graph_t g = graph_fold(nl, cl);
	cJSON *a = cJSON_GetArrayItem(g.nodes, 0);
	CHECK(cJSON_GetArraySize(g.nodes) == 1 && !strcmp(cJSON_GetObjectItem(a, "name")->valuestring, "A"));
	CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(a, "aliases")) == 3 && cJSON_GetArraySize(cJSON_GetObjectItem(a, "provenance")) == 2);
	CHECK(cJSON_GetArraySize(g.chains) == 1 && has_chain(g.chains, "arg_1")); /* same premise SET = one chain */
	char *d = graph_digest(&g, "c2");
	CHECK(strstr(d, "THE GRAPH SO FAR") && strstr(d, "n_a | A | a1") && !strstr(d, "DERIVATIONS"));
	free(d);
	d = graph_digest(&g, "nobody");
	CHECK(!strcmp(d, ""));
	free(d);
	graph_free(&g); cJSON_Delete(nl); cJSON_Delete(cl);

	/* simplify: superset premises drop; ancestry cover drops; leanest earliest stands */
	cl = cJSON_CreateArray();
	cJSON_AddItemToArray(cl, chain("k1", "[\"a\",\"b\"]", "x", "1"));
	cJSON_AddItemToArray(cl, chain("k2", "[\"x\",\"c\"]", "y", "2"));
	cJSON_AddItemToArray(cl, chain("d1", "[\"a\",\"b\",\"c\"]", "y", "3"));   /* covered by ancestry of k2 */
	cJSON_AddItemToArray(cl, chain("d2", "[\"a\",\"b\",\"q\"]", "x", "4"));   /* superset of k1 */
	cJSON_AddItemToArray(cl, chain("k3", "[\"q\"]", "x", "5"));             /* independent route stays */
	cJSON_AddItemToArray(cl, chain("k4", "[\"m\"]", "n", "6"));             /* an isolated cycle: */
	cJSON_AddItemToArray(cl, chain("k5", "[\"n\"]", "m", "7"));             /* terminates, both kept */
	cJSON *kept = graph_simplify(cl);
	CHECK(has_chain(kept, "k1") && has_chain(kept, "k2") && has_chain(kept, "k3") && !has_chain(kept, "d1") && !has_chain(kept, "d2"));
	CHECK(has_chain(kept, "k4") && has_chain(kept, "k5"));
	cJSON_Delete(kept); cJSON_Delete(cl);

	/* candidates: BM25 ranks the paraphrase first; alias match reported; paging */
	cJSON *nodes = J("[{\"id\":\"n_1\",\"name\":\"Chain rule\",\"statement\":\"The derivative of a composition is the product of derivatives.\",\"aliases\":[\"d/dx f(g(x)) = f'(g(x)) g'(x)\"]},"
		"{\"id\":\"n_2\",\"name\":\"Product rule\",\"statement\":\"The derivative of a product is f'g + fg'.\",\"aliases\":[]},"
		"{\"id\":\"n_3\",\"name\":\"Photosynthesis\",\"statement\":\"Plants convert light into chemical energy.\",\"aliases\":[]}]");
	cJSON *props = J("[{\"id\":\"p1\",\"name\":\"Composition rule\",\"statement\":\"Differentiating a composition multiplies the derivatives\",\"aliases\":[]}]");
	char err[256];
	cJSON *r = graph_candidates(nodes, props, J("[{\"id\":\"p1\"}]"), err, sizeof err);
	cJSON *ids = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(r, "matches"), 0), "candidateIds");
	CHECK(r && !strcmp(cJSON_GetArrayItem(ids, 0)->valuestring, "n_1") && cJSON_GetArraySize(ids) == 2); /* no overlap with n_3 */
	cJSON_Delete(r);
	CHECK(!graph_candidates(nodes, props, J("[{\"id\":\"p9\"}]"), err, sizeof err) && strstr(err, "Unknown proposition id"));
	CHECK(!graph_candidates(nodes, props, J("[{\"id\":\"p1\"},{\"id\":\"p1\"}]"), err, sizeof err) && strstr(err, "Repeated"));
	r = graph_candidates(nodes, props, J("[{\"id\":\"p1\",\"offset\":1}]"), err, sizeof err);
	CHECK(r && cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(r, "matches"), 0), "candidateIds")) == 1);
	cJSON_Delete(r); cJSON_Delete(nodes); cJSON_Delete(props);
}

static void test_golden_graph(void) {
	cJSON *nodes = store_read_jsonl("/Users/matt/learnharness/tests/fixtures/golden-data/graph/nodes.jsonl");
	cJSON *chains = store_read_jsonl("/Users/matt/learnharness/tests/fixtures/golden-data/graph/chains.jsonl");
	if (cJSON_GetArraySize(chains) == 0) { cJSON_Delete(nodes); cJSON_Delete(chains); return; }
	cJSON *kept = graph_simplify(chains);
	CHECK(cJSON_GetArraySize(kept) <= cJSON_GetArraySize(chains));
	cJSON *again = graph_simplify(kept);
	CHECK(cJSON_GetArraySize(again) == cJSON_GetArraySize(kept)); /* a fixed point */
	graph_t g = graph_fold(nodes, chains);
	printf("golden graph: %d lines -> %d nodes; %d chains, %d after simplify\n", cJSON_GetArraySize(nodes), cJSON_GetArraySize(g.nodes), cJSON_GetArraySize(chains), cJSON_GetArraySize(kept));
	graph_free(&g); cJSON_Delete(kept); cJSON_Delete(again); cJSON_Delete(nodes); cJSON_Delete(chains);
}

static void test_practice(void) {
	cJSON *m = J("[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"[Session context] — picked\"}]}]");
	CHECK(phase_is_practice(m));
	CHECK(phase_active_problem(m) == NULL);
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"p1\",\"name\":\"problem\",\"input\":{\"statement\":\"s1\"}}]}"));
	CHECK(phase_active_problem(m) == NULL); /* posed only once its result lands */
	cJSON_AddItemToArray(m, J("{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"p1\",\"text\":\"posed\"}]}"));
	const cJSON *a = phase_active_problem(m);
	CHECK(a && !strcmp(cJSON_GetObjectItem(a, "statement")->valuestring, "s1"));
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"c1\",\"name\":\"conclude\",\"input\":{}}]}"));
	cJSON_AddItemToArray(m, J("{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"c1\",\"is_error\":true,\"text\":\"no\"}]}"));
	CHECK(phase_active_problem(m) != NULL); /* a refused conclude concludes nothing */
	cJSON_AddItemToArray(m, J("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"c2\",\"name\":\"conclude\",\"input\":{}}]}"));
	cJSON_AddItemToArray(m, J("{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"c2\",\"text\":\"done\"}]}"));
	CHECK(phase_active_problem(m) == NULL);
	cJSON_Delete(m);
	m = J("[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"binary search\"}]}]");
	CHECK(!phase_is_practice(m));
	cJSON_Delete(m);
}

static void ucase(const char *tex, const char *want) {
	char *u = math_unicode(tex);
	runs++;
	if (strcmp(u, want)) { fails++; printf("FAIL math_unicode(%s) = \"%s\", want \"%s\"\n", tex, u, want); }
	free(u);
}

static char mout[4096];
static size_t mlen;
static void mcat(const char *s, size_t n) { memcpy(mout + mlen, s, n); mlen += n; mout[mlen] = 0; }
static int shown;
static int fake_display(const char *tex) { shown++; mcat("[IMG:", 5); mcat(tex, strlen(tex)); mcat("]\n", 2); return 0; }

/* Streams `in` 1 byte at a time (worst-case chunking) and compares. */
static void scase(const char *in, const char *want) {
	math_stream_t m = { .out = mcat, .bol = 1 };
	mlen = 0; mout[0] = 0;
	for (const char *p = in; *p; p++) { char c[2] = { *p, 0 }; math_feed(&m, c); }
	math_flush(&m);
	runs++;
	if (strcmp(mout, want)) { fails++; printf("FAIL stream(%s)\n  got  \"%s\"\n  want \"%s\"\n", in, mout, want); }
}

static void test_math(void) {
	ucase("x^2", "x²");
	ucase("x_{ij}^{n+1}", "xᵢⱼⁿ⁺¹");
	ucase("e^{i\\pi}", "e^(iπ)");
	ucase("\\sum_{i=1}^{n} i", "∑ᵢ₌₁ⁿ i");
	ucase("\\frac{a}{b}", "a/b");
	ucase("\\frac{x+1}{2}", "(x+1)/2");
	ucase("\\sqrt{x+1}", "√(x+1)");
	ucase("\\sqrt[3]{8}", "³√8");
	ucase("f: \\mathbb{R} \\to \\mathbb{R}", "f: ℝ → ℝ");
	ucase("\\left( \\alpha \\right.", "( α");
	ucase("\\text{if } x \\neq 0", "if x ≠ 0");
	ucase("\\{ x \\mid x > 0 \\}", "{ x ∣ x > 0 }");
	ucase("\\sin x", "sin x");
	ucase("f'(x)", "f′(x)");
	ucase("\\not\\in", "∈\xcc\xb8");

	scase("so $x^2$ grows", "so x² grows");
	scase("from $5 to $10 a day", "from $5 to $10 a day");
	scase("it costs $ 3", "it costs $ 3");
	scase("escaped \\$x$ stays", "escaped $x$ stays");
	scase("`$x$` and $y$", "`$x$` and y");
	scase("```\n$a$\n```\n$b$", "```\n$a$\n```\nb");
	scase("unclosed $x and more", "unclosed $x and more");
	scase("a\\nb", "a\nb");
	scase("see:\n$$\n\\frac{1}{3}\n$$\nafter", "see:\n    1/3\nafter");
	math_display = fake_display;
	shown = 0;
	scase("see:\n$$\n\\int_0^1 x\n$$\nafter", "see:\n[IMG:\n\\int_0^1 x\n]\nafter");
	scase("inline $$x$$ mid", "inline \n[IMG:x]\n mid");
	CHECK(shown == 2);
	math_display = NULL;

	/* one-line labels: display math stays in the line, the transcript text is untouched */
	char *t = math_text("Why $\\sum_{k} 2^{-k}$ is $$\\frac{1}{2}\\\\ 1$$ ok");
	CHECK(!strcmp(t, "Why ∑ₖ 2⁻ᵏ is 1/2 1 ok"));
	free(t);

	/* the plan map and the graph view show names typeset */
	char err[512];
	cJSON *tree = plan_from_mermaid("graph TD\n  sq[\"Why $x^2 \\geq 0$\"] --> sum[\"The sum $\\sum_i x_i^2$\"]\n", err, sizeof err);
	CHECK(tree != NULL);
	if (tree) {
		char *v = plan_render(tree, "sq", NULL);
		CHECK(strstr(v, "Why x² ≥ 0") && strstr(v, "The sum ∑ᵢ xᵢ²") && !strchr(v, '$'));
		free(v);
		cJSON_Delete(tree);
	}
	cJSON *nl = J("[{\"id\":\"n_a\",\"name\":\"$\\\\hat{x}$ is unbiased\",\"statement\":\"s\",\"aliases\":[],\"provenance\":[{\"chatId\":\"cz\",\"messageId\":\"m\"}]},"
		"{\"id\":\"n_b\",\"name\":\"$\\\\mathbb{E}[x]$\",\"statement\":\"s\",\"aliases\":[],\"provenance\":[{\"chatId\":\"cz\",\"messageId\":\"m\"}]}]");
	cJSON *cl = J("[{\"id\":\"arg_1\",\"premises\":[\"n_b\"],\"conclusion\":\"n_a\"}]");
	graph_t g = graph_fold(nl, cl);
	char *r = graph_render(&g, 0, 25);
	/* "  └─ x̂ is unbiased ⇐ 𝔼[x]" is 25 cells only because the accent takes none: it fits at width 25 */
	CHECK(strstr(r, "x\xcc\x82 is unbiased ⇐ 𝔼[x]\n") != NULL);
	if (!strstr(r, "unbiased ⇐")) printf("  got: %s\n", r);
	free(r);
	graph_free(&g); cJSON_Delete(nl); cJSON_Delete(cl);
}

static void touch_chat(const char *dir, const char *id, time_t t) {
	char p[1300];
	snprintf(p, sizeof p, "%s/chats/%s.jsonl", dir, id);
	FILE *f = fopen(p, "w"); fputs("{\"role\":\"user\",\"content\":[]}\n", f); fclose(f);
	struct utimbuf u = { t, t }; utime(p, &u);
}

static void test_resolve(void) {
	char dir[] = "/tmp/diotima-test-XXXXXX", p[1300], id[48], err[256];
	CHECK(mkdtemp(dir) != NULL);
	setenv("DIOTIMA_HOME", dir, 1);
	CHECK(session_resolve(NULL, id, sizeof id, err, sizeof err) != 0 && strstr(err, "no sessions"));
	CHECK(store_path(p, sizeof p, "chats/x") == 0);
	touch_chat(dir, "c6aeff8c5ea16", 3000);
	touch_chat(dir, "c6aeff8c5ba16", 2000);
	touch_chat(dir, "c6aeff8c50001", 1000);
#define R(a, want) do { id[0] = 0; int rc_ = session_resolve(a, id, sizeof id, err, sizeof err); CHECK(rc_ == 0 && !strcmp(id, want)); if (rc_ || strcmp(id, want)) printf("  %s -> %s / %s\n", a ? a : "(null)", id, err); } while (0)
	R(NULL, "c6aeff8c5ea16");
	R("", "c6aeff8c5ea16");
	R("1", "c6aeff8c5ea16");
	R("2", "c6aeff8c5ba16");
	R("3", "c6aeff8c50001");
	R("ea16", "c6aeff8c5ea16");
	R("c6aeff8c5ba16", "c6aeff8c5ba16");
	R("c50001", "c6aeff8c50001");
#undef R
	CHECK(session_resolve("4", id, sizeof id, err, sizeof err) != 0 && strstr(err, "lists 3"));
	/* all digits always means an ls number, never a tail */
	CHECK(session_resolve("50001", id, sizeof id, err, sizeof err) != 0 && strstr(err, "lists 3"));
	CHECK(session_resolve("ba16", id, sizeof id, err, sizeof err) == 0 && !strcmp(id, "c6aeff8c5ba16"));
	CHECK(session_resolve("a16", id, sizeof id, err, sizeof err) != 0 && strstr(err, "matches 2"));
	CHECK(session_resolve("zz", id, sizeof id, err, sizeof err) != 0 && strstr(err, "no such session"));
	/* dio ls titles: the opening goal, practice topics, clipped on whole UTF-8 characters */
	char t[64];
#define W(name, body) do { snprintf(p, sizeof p, "%s/chats/%s.jsonl", dir, name); FILE *f_ = fopen(p, "w"); fputs(body "\n", f_); fclose(f_); } while (0)
	W("t1", "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"recurrence relations\\nand more\"}]}");
	W("t2", "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"[Practice session] — topic:\\nbinomials\"}]}");
	W("t3", "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"[Session context] — picked\\n- [p1] x\"}]}");
	W("t4", "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"aéé\"}]}");
#undef W
	session_title("t1", t, sizeof t); CHECK(!strcmp(t, "recurrence relations and more"));
	session_title("t2", t, sizeof t); CHECK(!strcmp(t, "practice: binomials"));
	session_title("t3", t, sizeof t); CHECK(!strcmp(t, "practice: from the knowledge map"));
	session_title("t4", t, 5); CHECK(!strcmp(t, "a\xc3\xa9"));   /* "aé" + half an é → "aé" */
	session_title("t4", t, 6); CHECK(!strcmp(t, "a\xc3\xa9\xc3\xa9")); /* fits exactly: kept */
	session_title("nope", t, sizeof t); CHECK(t[0] == 0);
	char cmd[1300];
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
	if (system(cmd)) {}
}

int main(void) {
	test_resolve();
	test_math();
	test_practice();
	test_sha256(); test_coverage(); test_graph(); test_golden_graph();
	test_plan(); test_phase(); test_store(); test_golden(); test_mermaid(); test_plan_derived(); test_mechanic();
	printf("%d checks, %d failed\n", runs, fails);
	return fails ? 1 : 0;
}
