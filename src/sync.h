#ifndef SYNC_H
#define SYNC_H
/* Keep the data root in step with its git remote, when the root is a git
 * checkout (otherwise both are no-ops). Failures warn on stderr and never stop dio. */
void sync_pull(void);                 /* fetch + rebase others' changes in */
void sync_push(const char *what);     /* commit everything (tmp/ is gitignored) and push */
#endif
