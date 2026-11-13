#!/usr/bin/env bash
# End-to-end with a fake claude: real REPL, real MCP server, scripted model.
set -euo pipefail
cd "$(dirname "$0")/.."
make -s build/dio
export PATH="$PWD/tests/bin:$PATH"
scratch=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf "$scratch"' EXIT
fail=0; [ -n "${KEEP:-}" ] && echo "scratch: $scratch"
ok() { if grep -q -- "$2" <<<"$1"; then echo "ok   $3"; else echo "FAIL $3"; fail=1; fi; }
count() { grep -o -- "$2" <<<"$1" | wc -l | tr -d ' '; }

# ── multiple choice (default mechanic) ──
export DIOTIMA_HOME=$scratch/quiz FAKE_OUT=$scratch/quiz.r
mkdir -p "$DIOTIMA_HOME"
# probe: thinking, 2, 90% | predict: -, 1, 60% | check: open, reading note, -, 2, default | /plan
out=$(printf 'four obviously\n2\n90\n\n1\n60\n\nrows of dots\n\n2\n\n/plan\n' | ./build/dio new "arithmetic" 2>&1) || { echo "$out"; exit 1; }
echo "$out"
plain=$(sed $'s/\033\\[[0-9;]*m//g' <<<"$out")
f=$(ls "$DIOTIMA_HOME"/chats/*.jsonl)
ok "$out" "Let me check your arithmetic" "prose streamed"
ok "$out" "✓ correct" "quiz graded"
ok "$out" "probe concluded" "handoff announced"
ok "$(cat "$f")" "\"name\":\"quiz\"" "quiz call recorded in transcript"
ok "$(cat "$FAKE_OUT.tu1")" 'confidence\\":90' "confidence returned to model"
ok "$(cat "$FAKE_OUT.tu2")" 'quiz rejected: correctAnswer \\"zzz\\" matches no option' "bad quiz bounced to the model"
ok "$(cat "$FAKE_OUT.tu3")" 'Probe concluded' "probe_done accepted"
ok "$(cat "$FAKE_OUT.replay.tu3")" "already executed" "replayed tool_use id refused"
ok "$(cat "$FAKE_OUT.stdin.teacher")" "call plan_init and present the plan" "teacher woken with the handoff steer"
ok "$(cat "$FAKE_OUT.tp1")" 'plan_init rejected.*node id `Bad`' "bad mermaid bounced with the slug rule"
ok "$(cat "$FAKE_OUT.tp2")" 'nodeIds\\":\[\\"count\\",\\"add\\",\\"mult\\"\]' "plan_init returns the node ids"
ok "$plain" "^      ○ mult  Multiplication ← add, count" "plan printed as an indented DAG"
ok "$(cat "$FAKE_OUT.tq1")" 'nodeId \\"nope\\" matches no node.*count, add, mult' "unknown nodeId bounced with the slugs"
ok "$(cat "$FAKE_OUT.tq2")" 'verdict\\":\\"correct' "predict answered"
[ "$(count "$out" "Check your understanding")" = 1 ] && echo "ok   only the check is gated" || { echo "FAIL only the check is gated"; fail=1; }
ok "$(cat "$FAKE_OUT.tq3")" 'thinkingWhileReading\\":\\"rows of dots' "reading note returned on the check"
ok "$plain" "✓ add" "/plan marks the visited node"
ok "$plain" "● mult" "/plan marks the current node"
[ "$(count "$(cat "$f")" "call plan_init and present")" = 1 ] || { echo "FAIL steer once"; fail=1; }
ok "$plain" "· graph: +2 propositions, 1 chain" "session end extracts the prose into the graph"
ok "$(cat "$FAKE_OUT.stdin.extractor")" "TEACHER \[m[0-9]*\] Here is the map" "extractor sees the teacher prose with message ids"
ok "$(cat "$FAKE_OUT.ex1")" "not a passage of THIS stretch" "bad messageId bounced to the extractor"
[ ! -e "$FAKE_OUT.au2" ] && echo "ok   empty graph skips the auditor" || { echo "FAIL empty graph skips the auditor"; fail=1; }
g=$(./build/dio graph | sed $'s/\033\\[[0-9;]*m//g')
ok "$g" "^● arithmetic  2 ideas · 1 derivation  (c" "dio graph heads a birth session with counts"
ok "$g" "└─ Repeated addition ⇐ Counting on" "dio graph shows the derivation inline"
ok "$(NO_COLOR=1 ./build/dio graph)" "^● arithmetic" "piped graph output carries no color codes"
nlines=$(wc -l <"$DIOTIMA_HOME/graph/coverage.jsonl")
# resume: the steer is not delivered twice (the fake teacher re-runs and hits EOF)
printf '' | ./build/dio resume "$(basename "$f" .jsonl)" >/dev/null 2>&1 || true
[ "$(count "$(cat "$f")" "call plan_init and present")" = 1 ] && echo "ok   steer delivered once across resume" || { echo "FAIL steer delivered once across resume"; fail=1; }
ok "$(cat "$FAKE_OUT.stdin.extractor")" "THE GRAPH SO FAR" "second extraction carries the same-source digest"
ok "$(cat "$FAKE_OUT.au1")" 'candidateIds' "auditor searches candidates"
ok "$(cat "$FAKE_OUT.au2")" 'is not an existing record' "unknown linkTo bounced to the auditor"
ok "$(cat "$FAKE_OUT.au3")" 'Review submitted' "auditor review accepted"
[ "$(grep -c '"name":"Counting on"' "$DIOTIMA_HOME/graph/nodes.jsonl")" = 2 ] && [ "$(./build/dio graph | sed $'s/\033\\[[0-9;]*m//g' | grep -c '─ Counting on$')" = 1 ] \
  && echo "ok   linked proposition appends a line, merges to one node" || { echo "FAIL linked proposition appends a line, merges to one node"; fail=1; }
[ "$(wc -l <"$DIOTIMA_HOME/graph/coverage.jsonl")" -gt "$nlines" ] && echo "ok   only new prose re-extracted" || { echo "FAIL only new prose re-extracted"; fail=1; }

ok "$(cat "$FAKE_OUT.tu0")" "knowledge graph is empty" "prober reads the (empty) map"

# /quit at a question's answer> prompt ends the session
q=$(export DIOTIMA_HOME=$scratch/quitq FAKE_OUT=$scratch/quitq.r; mkdir -p "$DIOTIMA_HOME"; printf '/quit\n' | ./build/dio new "arithmetic" 2>&1; echo "rc=$?")
ok "$q" "rc=0" "/quit at answer> exits cleanly"
if grep -q "pick one" <<<"$q"; then echo "FAIL /quit is not taken as a bad answer"; fail=1; else echo "ok   /quit is not taken as a bad answer"; fi
ok "$(cat "$scratch/quitq.r.tu1")" "learner closed the session" "the model is told the learner left"

# ── practice from a graph session ──
sid=$(basename "$f" .jsonl)
cov_before=$(wc -l <"$DIOTIMA_HOME/graph/coverage.jsonl")
# thinking (2 lines, blank sends) | pause: enter = next | /skip | enter = next
out=$(cd "$scratch" && printf '3+3 is 6\nso 6\n\n\n/skip\n\n' | "$OLDPWD/build/dio" practice "$sid" 2>&1) || { echo "$out"; exit 1; }
echo "$out"
ok "$(cat "$FAKE_OUT.stdin.practice")" "\[Session context\]" "practice opens with the picked session context"
ok "$(cat "$FAKE_OUT.stdin.practice")" "\[n_[0-9a-f]*\] Counting on:" "picks carry ids, names and statements"
ok "$(cat "$FAKE_OUT.stdin.practice")" "Reasoning chains among them" "chains among the picks included"
ok "$(cat "$FAKE_OUT.builtins" 2>/dev/null)" "ok" "practice may read files (Read/Glob/Grep)"
ok "$(cat "$FAKE_OUT.cwd")" "$(cd "$scratch" && pwd -P)" "practice child runs in the launch directory"
ok "$(cat "$FAKE_OUT.pr1")" "modelSolution. is required" "problem without a committed solution bounced"
ok "$(cat "$FAKE_OUT.pr2")" "Problem posed" "problem posed with a valid target"
ok "$out" "▸ Problem" "problem shown to the learner"
ok "$(cat "$DIOTIMA_HOME"/chats/c*.jsonl)" 'Your thinking:\\n3+3 is 6\\nso 6' "multi-line thinking sent as one submission"
ok "$(cat "$FAKE_OUT.co1")" "synthesized" "model may not conclude as dismissed"
ok "$(cat "$FAKE_OUT.co2")" "Problem concluded" "conclude accepted"
ok "$out" "model solution:" "committed solution revealed on conclude"
ok "$(cat "$FAKE_OUT.pr3")" 'target \[no-such-node\] is not a known proposition' "unknown target bounced"
ok "$(cat "$FAKE_OUT.pr4")" "Problem posed" "next problem posed after the go-ahead"
ok "$out" "── dismissed ──" "/skip dismisses the live problem"
ok "$(cat "$FAKE_OUT.co3")" "no active problem" "conclude after a dismissal is refused"
[ "$(wc -l <"$DIOTIMA_HOME/graph/coverage.jsonl")" = "$cov_before" ] && echo "ok   practice writes nothing to the graph" || { echo "FAIL practice writes nothing to the graph"; fail=1; }

# ── free recall ──
export DIOTIMA_HOME=$scratch/explain FAKE_OUT=$scratch/explain.r
mkdir -p "$DIOTIMA_HOME"
echo '{"mechanic":"explain"}' >"$DIOTIMA_HOME/config.json"
# probe: -, answer | predict: -, answer | check: open, -, -, (don't know)
# a tiny budget: the probe's prose alone triggers a background run while the teacher works
out=$(printf '\nfour, two and two\n\ncount on one\n\n\n\n\n' | DIOTIMA_CHUNK_TOKENS=5 ./build/dio new "arithmetic" 2>&1) || { echo "$out"; exit 1; }
echo "$out"
ok "$(cat "$FAKE_OUT.tu1")" 'kind\\":\\"explain\\",\\"text\\":\\"four, two and two' "free-recall answer returned"
ok "$out" "committed answer" "commitment revealed on submit"
ok "$(cat "$FAKE_OUT.tu2")" "correctAnswer is required" "uncommitted explain bounced"
ok "$(cat "$FAKE_OUT.tq1")" 'explain rejected: `type` must be the bare word predict.*got \\"guess' "bad type bounced"
ok "$(cat "$FAKE_OUT.tq3")" 'dontKnow\\":true' "blank check answer is a don't-know"
[ "$(count "$out" "· graph: +")" = 2 ] && echo "ok   background run and exit flush both report" || { echo "FAIL background run and exit flush both report"; fail=1; }
[ "$(wc -l <"$DIOTIMA_HOME/graph/coverage.jsonl")" -ge 3 ] && echo "ok   background + exit runs cover all prose" || { echo "FAIL background + exit runs cover all prose"; fail=1; }
# ── background jobs: research, demo, visual, pdf, show_image (teacher, after a concluded probe) ──
export DIOTIMA_HOME=$scratch/jobs FAKE_OUT=$scratch/jobs.r DIOTIMA_NO_OPEN=1 FAKE_MODE=jobs
mkdir -p "$DIOTIMA_HOME/chats"
chat="$DIOTIMA_HOME/chats/cjobs.jsonl"
cat >"$chat" <<'JSONL'
{"role":"user","content":[{"type":"text","text":"binary search"}]}
{"role":"assistant","content":[{"type":"tool_use","id":"pd1","name":"probe_done","input":{"assessment":{"strands":[{"strand":"arrays","assessment":"solid"}]}}}]}
{"role":"user","content":[{"type":"tool_result","tool_use_id":"pd1","text":"Probe concluded."}]}
JSONL
# keep stdin open until the four quick jobs have delivered, then quit (the slow one is aborted)
out=$( { for i in $(seq 1 160); do [ "$(grep -c '\[job j_' "$chat")" -ge 4 ] && break; sleep 0.25; done; echo /quit; } \
  | (cd tests/fixtures && ../../build/dio resume cjobs) 2>&1) || { echo "$out"; exit 1; }
echo "$out"
t=$(cat "$chat")
ok "$(cat "$FAKE_OUT.tj1")" "research rejected: .question. is required" "job without its required field bounced"
ok "$(cat "$FAKE_OUT.tj2")" "Dispatched (job j_[0-9a-f]*, research). This call is fire-and-forget" "research dispatched fire-and-forget"
ok "$(cat "$FAKE_OUT.tj6")" 'not a readable file' "missing PDF bounced at dispatch"
ok "$(cat "$FAKE_OUT.tj7")" "Shown to the learner: dot.png" "show_image shows the learner's picture"
ok "$(cat "$FAKE_OUT.dm1")" "nothing written yet" "run_demo before write_demo refused"
ok "$(cat "$FAKE_OUT.dm3")" "PASSED: no console errors.*Probe result: true" "demo tested in headless Chrome with a probe"
ok "$(cat "$FAKE_OUT.vs1")" "no SVG yet" "render before write refused"
ok "$(cat "$FAKE_OUT.vs3")" "does not appear" "edit_svg needs an exact match"
ok "$(cat "$FAKE_OUT.vs5")" '"type":"image"' "render_svg returns the render as an image"
ok "$(cat "$FAKE_OUT.stdin.visual")" "40×30 pixels" "annotator told the learner image size"
ok "$(cat "$FAKE_OUT.pd1")" "Pages: 1" "pdf_info triages the document"
ok "$(cat "$FAKE_OUT.pd3")" "1 hit" "pdf_search finds the term"
ok "$(cat "$FAKE_OUT.pd4")" '"type":"image"' "pdf_render returns page images"
ok "$(cat "$FAKE_OUT.pd5")" "bad page spec" "out-of-range pages bounced"
ok "$t" "· research\] Research complete" "research brief delivered as a message"
ok "$t" "· demo\] The demo .*is built, headlessly tested" "demo delivered"
ok "$t" "· visual\] The annotated image is drawn" "annotated figure delivered"
ok "$t" "· pdf\] PDF reading complete" "pdf brief delivered"
ok "$t" "· research\] Research aborted" "the job still running at quit is recorded as aborted"
ok "$(sed $'s/\033\\[[0-9;]*m//g' <<<"$out")" "▸ demo: Halving" "demo announced to the learner"
ok "$(cat "$FAKE_OUT.stdin.teacher")" "\[job j_" "teacher woken with the job results"
ls "$DIOTIMA_HOME"/jobs/*/figure.png >/dev/null 2>&1 && echo "ok   figure rendered to a PNG" || { echo "FAIL figure rendered to a PNG"; fail=1; }
# a job a dead process left behind is recorded as aborted on resume
printf '%s\n' '{"role":"assistant","content":[{"type":"tool_use","id":"zz","name":"demo","input":{"title":"t","brief":"b"}}]}' \
  '{"role":"user","content":[{"type":"tool_result","tool_use_id":"zz","text":"Dispatched (job j_dead000000, demo). ..."}]}' >>"$chat"
printf '/quit\n' | (cd tests/fixtures && ../../build/dio resume cjobs) >/dev/null 2>&1 || true
ok "$(cat "$chat")" "job j_dead000000 · demo\] The demo job was aborted" "orphaned job recorded as aborted on resume"
unset FAKE_MODE DIOTIMA_NO_OPEN

export DIOTIMA_HOME=$scratch/explain
echo '{"mechanic":"essay"}' >"$DIOTIMA_HOME/config.json"
ok "$(./build/dio new x </dev/null 2>&1 || true)" 'mechanic.*must be' "invalid mechanic refused"
exit $fail
