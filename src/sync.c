#include "sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "proc.h"
#include "store.h"

static int enabled(void) {
	char p[1100];
	snprintf(p, sizeof p, "%s/.git", store_root());
	return !access(p, F_OK) && !getenv("DIOTIMA_NO_SYNC");
}

/* git -C <root> args…; prints git's output under a warning when it fails. */
static int git(const char *const *args, const char *warn) {
	char *argv[16] = {"git", "-C", (char *)store_root()};
	int n = 3;
	while (*args && n < 15) argv[n++] = (char *)*args++;
	argv[n] = NULL;
	char *out = NULL;
	int rc = proc_run(argv, &out, 1);
	if (rc && warn) fprintf(stderr, "dio: %s (%s)\n%s", warn, store_root(), out ? out : "");
	free(out);
	return rc;
}

void sync_pull(void) {
	if (!enabled()) return;
	setenv("GIT_TERMINAL_PROMPT", "0", 1); /* never block on a password prompt */
	if (git((const char *[]){"pull", "-q", "--rebase", "--autostash", NULL},
	        "could not pull your data — continuing with what is on this machine")) {
		git((const char *[]){"rebase", "--abort", NULL}, NULL);
	}
}

void sync_push(const char *what) {
	if (!enabled()) return;
	setenv("GIT_TERMINAL_PROMPT", "0", 1);
	git((const char *[]){"add", "-A", NULL}, "could not stage your data");
	if (git((const char *[]){"diff", "--cached", "--quiet", NULL}, NULL)) { /* something changed */
		char msg[256];
		snprintf(msg, sizeof msg, "dio %s", what);
		if (git((const char *[]){"commit", "-q", "-m", msg, NULL}, "could not commit your data")) return;
	}
	/* Push even with no new commit: an earlier offline run may have left some behind. */
	sync_pull();
	git((const char *[]){"push", "-q", NULL}, "could not push your data — it is saved here and will go up next time");
}
