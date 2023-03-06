#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Global Pipes */
int pipefd[11][2] = {0};

static void check_error(int ret, const char *message) {
    if (ret != -1) {
        return;
    }
    int err = errno;
    perror(message);
    exit(err);
}

static void parent(int in_pipefd[2], int out_pipefd[2], pid_t child_pid, int *exit_code) {
	close(in_pipefd[1]);
	close(in_pipefd[0]);
	close(out_pipefd[1]);
	close(out_pipefd[0]);

	/* update exit code */
	int wstatus;
	check_error(waitpid(child_pid, &wstatus, 0),"waitpid");
	// assert(WIFEXITED(wstatus));
	*exit_code = WEXITSTATUS(wstatus);
}

static void child(int in_pipefd[2], int out_pipefd[2], const char *program, int index, int argc) {
	/* Extract Input From Prev Pipe */
	if (index != 1){
		dup2(in_pipefd[0],STDIN_FILENO);
	}
	close(in_pipefd[0]);
	close(in_pipefd[1]);

	/* Write Output To Next Pipe */
	if (index != argc-1){
		dup2(out_pipefd[1],STDOUT_FILENO);
	}
	close(out_pipefd[0]);
	close(out_pipefd[1]);

	/* Close All Pipes */
	for (int i = 0; i < argc; i++){
		close(pipefd[i][0]);
		close(pipefd[i][1]);
	}

	/* Invalid Return */
    int ret = execlp(program, program, NULL);
	if (ret == -1) exit(ENOENT);
}

int main(int argc, char *argv[]) {
	/* Empty return EINVAL*/
	if (argc <= 1)
		return EINVAL;

	/* Init PipeFD array */
	/* First Input as STDIN; Last Output as STDOUT */
	for (int i = 0; i < argc; i++){
		pipe(pipefd[i]); // e.g. 4th is 3's output and 4's input
	}

	int exit_code = 0;
	int pid_q[10] = {0};
	
	for (int i = 1; i < argc; i++){
		pid_t pid = fork();
		if (pid > 0) pid_q[i-1] = pid;
		else child(pipefd[i-1], pipefd[i], argv[i], i, argc);
	}

	for (int i = 0; i < 10; i++){
		if (pid_q[i] == 0) break;
		parent(pipefd[i], pipefd[i+1], pid_q[i], &exit_code);
		// printf("%d\n",exit_code);
		if (exit_code != 0) 
			return exit_code;
	}

	return exit_code;
}
