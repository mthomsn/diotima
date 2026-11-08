#ifndef CLAUDE_H
#define CLAUDE_H
#include <sys/types.h>
#include "mcp.h"
/* Run one agent turn: spawn `claude -p`, feed it the transcript, stream its
 * output to the terminal and record the new messages in the session.
 * Returns the child's exit status (0 = clean). */
int claude_turn(const char *prompt_file, const tool_t *tools, int ntools, const char *builtin);

/* A generic child: stdin = input, every stream-json event to on_event. A
 * background child gets its own process group (no ^C). Exit status. */
typedef struct {
	const char *prompt_file, *input, *errname;
	const char *builtin;   /* CLI built-ins to allow, e.g. "Read,Glob,Grep"; NULL = none */
	const char *cwd;       /* NULL = a fresh empty temp dir */
	const tool_t *tools;
	int ntools, chan, background, max_turns;   /* max_turns 0 = CLI default */
	void (*on_event)(const cJSON *ev, void *ctx);
	void *ctx;
	volatile pid_t *pid;   /* set while running, 0 after */
} claude_job_t;
int claude_run(const claude_job_t *j);
/* Admission for a non-agent channel: note the event's tool_use blocks. */
void claude_announce(int chan, const cJSON *ev);
/* The directory dio was launched from (learner paths resolve against it). */
const char *claude_launch_dir(void);
void claude_interrupt(void);   /* SIGINT: terminate the running child, if any */
#endif
