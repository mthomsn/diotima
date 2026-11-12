# diotima

A terminal learning harness in C, after [learnharness](https://github.com/amosblomqvist/learnharness).
It probes what you already know, plans a dependency map to your goal, and teaches one
reasoning step at a time. What you learn goes into a knowledge graph that every later
session can read. Practice sessions then drill what the graph holds.

The model is your local `claude` CLI on its own subscription auth; no API key is needed.
Its tools reach it through a loopback MCP server inside `dio`. Everything dio keeps is
plain JSON lines under `~/.diotima`.

## Install

    make                      # plain C11 + vendored cJSON → build/dio

Runtime needs, all on `PATH`:

| for | needs |
|---|---|
| everything | `claude` (signed in) |
| figures, pictures | `magick` (ImageMagick), `rsvg-convert` (librsvg) |
| PDFs | poppler: `pdfinfo`, `pdftotext`, `pdftoppm`, `pdfimages` |
| demos | Google Chrome (or `DIOTIMA_CHROME=/path/to/chrome`) |
| inline images | Ghostty or kitty; other terminals print the image's path instead |
| typeset display math | `pdflatex` (any TeX Live with amsmath) and poppler, in Ghostty or kitty |

On macOS: `brew install imagemagick librsvg poppler`.

## A day with dio

**Start something new.**

    dio new "how binary search avoids off-by-one errors"

dio prints the session id. The *prober* then measures where you stand. It asks what you
want out of this, then asks graded questions until it knows where your understanding runs
out on each strand the topic depends on. If earlier sessions put things in the graph, it
reads them first and skips what you already hold. When it has enough, it hands over:
`── probe concluded ──`.

**Agree on the map.** The *teacher* writes a plan: a small dependency graph from what you
already know to your goal, printed like this.

    ○ elim        One comparison rules out a whole side
      ○ middle      Why probe the middle: halving, log n
    ○ invariant   What a loop invariant is
      ○ interval    Search space as a half-open interval ← elim, invariant

Accept it or ask for changes. Teaching starts once you agree. As the lesson moves, `/plan`
shows where you are: ● current, ✓ visited, ○ upcoming. Your position comes from which node
the teacher's questions are about; you never tick anything off.

**Learn.** The teacher explains, and each step comes with questions:

- A **prediction** comes before the teaching and appears straight away. Guess honestly;
  being wrong is the point.
- A **check** comes after the teaching and waits behind
  `▸ Check your understanding — press enter when ready`. Read first, then press enter. You
  can say what you were thinking while reading, and the teacher reads that too.

For a multiple-choice question, type the option number (`1,3` when several apply, `0` for
"I don't know"). Then give your confidence (0–100) and, optionally, your reasoning. The
verdict and explanation show immediately. With free recall (see `config.json` below), you
type your answer; afterwards you see the answer the model committed to before asking, and
the teacher grades yours against it.

Talk to it at `you>` like any conversation.

**Math.** The model writes LaTeX, and dio shows it typeset. Inline `$…$` becomes Unicode
in the line ($x^2$ → x², `\sum_{i=1}^n` → ∑ᵢ₌₁ⁿ, `\mathbb{R}` → ℝ). Display `$$…$$` is
typeset by pdflatex and shown as an image in Ghostty or kitty. Images are cached under
`tmp/math/`. Without pdflatex or image support, display math prints as indented Unicode
instead. Code spans and fenced blocks are never touched, `$5 and $10` stays as written,
and the transcript keeps the raw LaTeX. `dio graph` and the plan map show their names' math as
Unicode.

**Let it fetch things.** When something needs checking or building, the teacher sends a job
off and keeps teaching:

| job | what you get |
|---|---|
| research | a sourced web brief, cited in the lesson |
| pdf | a brief from a PDF you name, with page citations |
| demo | an interactive HTML page, tested in headless Chrome, then opened in your browser |
| visual | a precise figure (geometry, number lines, plots) drawn inline; given one of your pictures, that picture marked up |

Up to three jobs run at once. If you're sitting at an empty `you>` when one finishes, the
teacher picks it up straight away. Otherwise it waits until the current question or turn is
done. You can point the teacher at your files ("look at notes/ch3.pdf", "here's my circuit:
photo.jpg"). Paths are relative to the directory you started dio in.

**Stop whenever.** `/quit`, or Ctrl-D. dio then:

1. aborts running jobs and records that in the transcript, so the teacher knows on resume;
2. extracts the lesson into the knowledge graph (Ctrl-C skips this; the remainder is
   extracted next time).

Extraction also runs in the background during long lessons. A dim `· graph:` line reports
each batch.

**Come back.** `dio ls` lists sessions, numbered newest first. `dio resume` picks up the
newest exactly where you left off; `dio resume 2` takes the second in the list, and
`dio resume a16` takes the session whose id ends in `a16` (any unique tail works). A bare
number always means the list position, so a tail made only of digits needs more of the id.
Wherever a command takes an id, the same shorthands work. The phase, the plan and your
position are all worked out from the transcript.

**See what you know.** `dio graph` prints every proposition you've been taught, grouped by
the session it was first learned in. Each session gets its own colored ●:

    ● eigenvalues and why matrices have them  5 ideas · 4 derivations  (c6b07c9e00002)
      ├─ Linear map
      ├─ Eigenvector ⇐ Linear map
      ├─ Diagonalization ⇐ Eigenvector ∧ Linear map
      └─ Linear ODE systems decouple in an eigenbasis
           ⇐ Diagonalization ∧ Exponential is its own derivative ●

`⇐ A ∧ B` means A and B together give you that idea. A derivation goes on the same line
when it fits the terminal width, otherwise underneath. A premise you learned in another
session carries that session's colored ●. When the output isn't a terminal, or `NO_COLOR`
is set, it reads `↗ <session title>` instead. The id in each heading is what
`dio practice` takes.

**Practice.** A practice session poses problems straight away, from the graph or from a
topic:

    dio practice                    # pick graph sessions by number, or type a topic
    dio practice c6aefe480323c      # every proposition first learned in that session
    dio practice "recurrence relations"

Work at `thinking>`: lines accumulate, and a blank line sends them. Ask for a hint by
saying you're stuck. Commit to an answer by stating it. When the problem is concluded you
see the verdict and the solution the model committed to before posing it. In the pause
between problems, ask about the one just finished, or press enter for the next. `/skip`
abandons a problem. Practice can do research and read your files and PDFs, but writes
nothing to the graph.

## Commands

| command | does |
|---|---|
| `dio new [goal…]` | start a learning session (asks for the goal if none is given) |
| `dio resume [id]` | continue a learning or practice session (newest if no id) |
| `dio practice [id… \| topic]` | start a practice session from graph sessions or a topic |
| `dio ls` | list sessions, numbered newest first, with what each set out to learn |
| `dio plan [id]` | print a session's plan map with your position |
| `dio graph` | print the knowledge graph |
| `dio extract [id]` | extract a session's unextracted prose into the graph now |

In a session:

| input | where | does |
|---|---|---|
| `/plan` | learning | show the map: ● current, ✓ visited, ○ upcoming |
| `/phase` | learning | show the phase: probe, plan or teach |
| `/demo` | learning | reopen the latest demo in the browser |
| `/skip` | practice, at `thinking>` | abandon the problem; its solution is shown |
| enter on an empty line | practice pause | ask for the next problem |
| `/quit`, Ctrl-D | any prompt (in practice, an empty `thinking>`) | end the session; an answer you already gave still counts |
| Ctrl-C | during a reply | stop the reply; during end-of-session extraction, skip it |

## Configuration

`~/.diotima/config.json`, optional:

    {"mechanic": "quiz"}       multiple choice, with confidence (default)
    {"mechanic": "explain"}    free recall: answer in your own words

The mechanic applies to every session, for both probing and teaching.

| variable | effect |
|---|---|
| `DIOTIMA_HOME` | data directory (default `~/.diotima`) |
| `DIOTIMA_MODEL` | model for every claude call, e.g. `sonnet` (default: the CLI's default) |
| `DIOTIMA_CHUNK_TOKENS` | graph extraction batch size and background trigger, in tokens (default 4096) |
| `DIOTIMA_CHROME` | the Chrome binary used to test demos |
| `DIOTIMA_PROMPTS` | a prompts directory to use instead of the built-in one |
| `DIOTIMA_NO_OPEN` | set it to stop dio opening demos in the browser |
| `NO_COLOR` | plain `dio graph` output with no color codes |

## What it keeps

    ~/.diotima/
      chats/<id>.jsonl      the transcript, one message per line: the only session state
      graph/nodes.jsonl     propositions (append-only; repeated ids merge, first line wins)
      graph/chains.jsonl    premises ⇒ conclusion (rewritten only to drop redundant chains)
      graph/coverage.jsonl  which assistant prose has been extracted (sha256 fingerprints)
      jobs/<id>/            job artifacts: demo.html, figure.svg/png, page renders
      config.json           the mechanic
      tmp/                  composed prompts, claude stderr logs

dio never stores progress: the phase, the plan, your position on it, the live practice
problem and outstanding jobs are all worked out from the transcript each time.

## How it works

One `dio` process runs the line REPL and an MCP server on 127.0.0.1. Every agent turn is a
fresh `claude -p`. It gets the transcript as text, a composed system prompt from `prompts/`,
and only that agent's tools; streamed output prints as it arrives. Tool calls are contracts.
A call runs only after the stream has announced it, and runs once. A bad call goes back to
the model with an error that says how to fix it.

| agent | tools |
|---|---|
| prober | quiz or explain, ask_user_question, probe_done, graph readers |
| teacher | plan_init, quiz or explain (tagged predict/check + plan node), ask_user_question, research, pdf, demo, visual, show_image, Read/Glob/Grep |
| practice | problem, conclude, graph readers, research, pdf, Read/Glob/Grep |
| extractor → auditor | submit_findings; find_candidates, submit_review (background, after teaching) |
| researcher / pdf reader / demo builder / visual maker | WebSearch/WebFetch; pdf_*; write_demo, run_demo; write_svg, edit_svg, render_svg |

    src/main.c                REPL, commands, turn selection, job wake-ups
    src/plan.c phase.c        mermaid → plan DAG and its view; phase, position, plan, problem from the transcript
    src/mcp.c claude.c        MCP server (a channel per concurrent child) + admission; claude children
    src/tools.c agent.c       agent tools; system-prompt composition
    src/coverage.c graph.c    prose fingerprints (sha256.c); ledgers, merge, chain simplification, BM25, view
    src/extract.c             extractor → auditor → merge, background and end-of-session
    src/jobs.c                research/pdf/demo/visual runners, delivery, aborts
    src/chrome.c img.c        headless Chrome over CDP pipes; images, kitty graphics, typeset display math
    src/math.c                LaTeX → Unicode; the stream filter that finds $…$ in model output
    src/term.c proc.c store.c line input that a finished job can interrupt; subprocesses; atomic files

## Test

    make test        # unit: math, plan, mermaid, phase, practice folds, store, config, sha256,
                     #       coverage, graph fold/simplify/candidates, golden fixtures
    tests/e2e.sh     # the real REPL and MCP server, driven by a scripted fake claude (tests/bin/claude):
                     # probe → plan → teach, graph extraction, practice, jobs (real headless Chrome)

Tests never touch `~/.diotima`; each run uses a temp `DIOTIMA_HOME`.

## License

AGPL-3.0. The prompts in `prompts/` come from learnharness.
