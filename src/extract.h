#ifndef EXTRACT_H
#define EXTRACT_H
/* The extraction pipeline: uncovered assistant prose → extractor (claude child,
 * submit_findings) → BM25 candidates → auditor (find_candidates, submit_review)
 * → deterministic merge into the graph ledgers. One run at a time, process-wide. */

/* After a turn: start a background run when uncovered prose reaches the budget. */
void extract_maybe_start(void);
/* On session end: wait for a background run, then extract whatever remains. */
void extract_finish(void);
/* `dio extract`: extract everything uncovered now, in the foreground. 0 ok. */
int extract_now(void);
/* Print notes from finished background runs (call at the prompt). */
void extract_drain_notes(void);
/* ^C: abort a foreground (exit/manual) run; background runs ignore it. */
void extract_interrupt(void);
#endif
