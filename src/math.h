#ifndef MATH_H
#define MATH_H
#include <stddef.h>
/* Terminal math. Model text carries LaTeX ($…$ inline, $$…$$ display); the
 * transcript keeps it raw, only what is printed changes. Inline math becomes
 * Unicode (x² ∑ᵢ α→β); display math goes to math_display when it is set
 * (a typeset image), else Unicode on its own indented line. Code spans and
 * fenced blocks pass through untouched. */

/* LaTeX → Unicode. malloc'd. */
char *math_unicode(const char *tex);

/* Shows display math; 0 when it did (cursor left at the start of a line). */
extern int (*math_display)(const char *tex);

typedef struct {
	void (*out)(const char *s, size_t n);
	void (*atom)(const char *s, size_t n); /* optional: inline math goes here, whole, instead of out */
	int oneline; /* display math stays in the line, as Unicode */
	int st, bol, ticks, bs, dollar, close, eat_nl;
	char *buf;
	size_t n, cap;
} math_stream_t;

/* Start with { .out = fn, .bol = 1 }. Feed streamed text in arbitrary chunks; flush at the end of a message
 * (an unclosed $ is then printed as it came). Text outside math goes to out. */
void math_feed(math_stream_t *m, const char *s);
void math_flush(math_stream_t *m);

/* One whole field (a question, an option, a verdict) to stdout, with no
 * trailing newline. A literal backslash-n outside math prints as a newline. */
void math_puts(const char *s);

/* A one-line label (a plan title, a graph proposition) with its math as
 * Unicode, display math included. malloc'd. */
char *math_text(const char *s);
#endif
