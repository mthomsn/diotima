#ifndef PROC_H
#define PROC_H
/* Run argv (no shell), capture stdout (+stderr when merge) into *out
 * (malloc'd, may be NULL). Returns the exit status, -1 if it could not run. */
int proc_run(char *const argv[], char **out, int merge);
#endif
