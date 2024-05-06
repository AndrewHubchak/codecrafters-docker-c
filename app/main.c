#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

// Usage: your_docker.sh run <image> <command> <arg1> <arg2> ...
int main(int argc, char *argv[]) {
	// Disable output buffering
	setbuf(stdout, NULL);

	int pipefd[2];
	int pipefderr[2];

	if (pipe(pipefd) == -1) {
		printf("was not able to create pipe");
		exit(EXIT_FAILURE);
	}

	if (pipe(pipefderr) == -1) {
		printf("was not able to create pipe for stderr");
		exit(EXIT_FAILURE);
	}

	char *command = argv[3];
	int child_pid = fork();
	if (child_pid == -1) {
	    printf("Error forking!");
	    exit(EXIT_FAILURE);
	}

	if (child_pid == 0) {
		// Replace current program with calling program
		close(pipefd[0]);
		close(pipefderr[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
			printf("dup2 failed for stdout");
			return 1;
		}
		if (dup2(pipefderr[1], STDERR_FILENO) == -1) {
			printf("dup2 failed for stderr");
			return 1;
		}

		close(pipefd[1]);
		close(pipefderr[1]);

		int exec_status = execv(command, &argv[3]);
		exit(EXIT_SUCCESS);
	} else {
		// We're in parent
		close(pipefd[1]); // close write end of the pipe
		close(pipefderr[1]); // close write end of the pipe
		char buf;
		while ( read(pipefd[0], &buf, 1) > 0) {
			write(STDOUT_FILENO, &buf, 1);
		}

		while ( read(pipefderr[0], &buf, 1) > 0) {
			write(STDERR_FILENO, &buf, 1);
		}
		close(pipefd[0]); // close read end of the pipe
		close(pipefderr[0]); // close read end of the pipe
		fflush(stdout);
		fflush(stderr);
		int child_exit_code = 0;
		int wait_return_val = -1;
		wait_return_val = waitpid(child_pid, &child_exit_code, 0);
		if ( wait_return_val == -1) {
			exit(EXIT_FAILURE);
		}

		if (WIFEXITED(child_exit_code)) {
			exit(WEXITSTATUS(child_exit_code));
		}
	}

	exit(EXIT_FAILURE);
}
