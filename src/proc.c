#include "proc.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int proc_run(char *const argv[], char **out, int merge) {
	int fd[2];
	if (pipe(fd)) return -1;
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		dup2(fd[1], 1);
		int nul = open("/dev/null", O_RDWR);
		dup2(merge ? fd[1] : nul, 2);
		dup2(nul, 0);
		close(fd[0]); close(fd[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fd[1]);
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	ssize_t r;
	while ((r = read(fd[0], buf + len, cap - len - 1)) > 0) { len += (size_t)r; if (len + 1 >= cap) buf = realloc(buf, cap *= 2); }
	buf[len] = 0;
	close(fd[0]);
	int st = 0;
	waitpid(pid, &st, 0);
	if (out) *out = buf; else free(buf);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
