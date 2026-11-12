#ifndef TERM_H
#define TERM_H
#include <pthread.h>
#include <stddef.h>
/* Held by the main thread while it prints model output and by question
 * handlers for the whole of an interaction, so the two never interleave. */
extern pthread_mutex_t term_mu;
/* Take/release term_mu. Taking it erases the waiting spinner; releasing it restarts its quiet clock. */
void term_lock(void);
void term_unlock(void);
/* While on (a teacher turn), a dim "⠋ thinking" shows at the cursor after 300ms with no output. */
void term_spin(int on);
/* Print label, read one line (no newline) into buf. -1 on EOF. Caller holds term_mu. */
int term_ask(const char *label, char *buf, size_t n);
/* Like term_ask, but returns 1 (buf empty) when term_wake() fires first. */
int term_ask_wakeable(const char *label, char *buf, size_t n);
/* Interrupt a term_ask_wakeable (any thread; a wake with no waiter is kept). */
void term_wake(void);
/* The kitty graphics protocol is available (Ghostty/kitty, stdout a tty). */
int term_has_images(void);
#endif
