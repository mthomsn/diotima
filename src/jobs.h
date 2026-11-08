#ifndef JOBS_H
#define JOBS_H
#include "mcp.h"
/* Background jobs: research, pdf, demo, visual — fire-and-forget claude
 * children (at most 3 at once, one MCP channel each). The dispatching tool
 * returns at once; the result arrives later as a "[job <id> · <kind>] …"
 * user message, appended by the main thread between turns. Nothing about a
 * job is stored except its artifacts under <root>/jobs/<id>/; which jobs are
 * outstanding is derived from the transcript. */

/* Tool sets: the teacher gets every job tool + show_image; practice gets research + pdf. */
const tool_t *jobs_teacher_tools(int *n);
const tool_t *jobs_practice_tools(int *n);

/* 1 when finished results are waiting to be delivered. */
int jobs_pending(void);
/* Main thread, between turns: append every finished result to the transcript and
 * show its artifact (figure inline, demo opened). Returns how many. Takes term_mu. */
int jobs_deliver(void);
/* On quit: kill running jobs, drop queued ones, deliver what finished, and record
 * a "was aborted" message for every job still outstanding. */
void jobs_abort_all(void);
/* On resume: record "was aborted" for jobs a previous process left outstanding. */
void jobs_recover(void);
/* The newest delivered demo's HTML path (static buffer) or NULL. */
const char *jobs_latest_demo(void);
#endif
