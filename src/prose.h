#ifndef PROSE_H
#define PROSE_H
#include <stddef.h>
/* Teacher prose on the terminal: markdown rendered (**bold**, *italic*, `code`,
 * # headings, - and 1. lists, ``` fences), word-wrapped with hanging indents,
 * behind a dim │ gutter. Streams a word at a time. Text reaches it after the
 * math filter, so math arrives as atoms: unbreakable, never read as markdown. */

extern int prose_width;                              /* -1 (default): the terminal's, at most 88; 0 = no wrapping */
extern void (*prose_out)(const char *s, size_t n);   /* default: stdout */

/* The teacher's stream. */
void prose_begin(void);                    /* a new turn: blank line before its first line; width re-read */
void prose_text(const char *s, size_t n);  /* markdown */
void prose_atom(const char *s, size_t n);  /* verbatim, one unbreakable word piece */
void prose_end(void);                      /* flush, end the line, clear styles */

/* One whole model-written field (math typeset, markdown, wrapped), each line
 * behind a prefix: first for the first line, rest for the others (NULL = same
 * as first). first NULL = the teacher's │ bar. Leaves the teacher's stream as it was. */
void prose_block(const char *text, const char *first, const char *rest, int bold);
#endif
