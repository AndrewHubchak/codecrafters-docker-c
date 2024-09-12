#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

const char* tmpDirectory = "./sandbox";
const char* sandboxCommand = "./sandbox/ex";

bool createDir(const char* dirName) {
	struct stat st = {0};
	bool result = false;

	if (stat(dirName, &st) == -1) {
		mkdir(dirName, 0700);
		result = true;
	}
	return result;
}

void copy_file(const char *from, const char *to) {
	struct stat file_stats;
	int fd_from = open(from, O_RDONLY);

	fstat(fd_from, &file_stats);

	void *address = mmap(0, file_stats.st_size, PROT_READ, MAP_PRIVATE, fd_from, 0);

	int fd_to = creat(to, 0777);

	write(fd_to, address, file_stats.st_size);

	close(fd_from);

	close(fd_to);

}

// Usage: your_docker.sh run <image> <command> <arg1> <arg2> ...
int main(int argc, char *argv[]) {
	int status = EXIT_FAILURE;
	// Disable output buffering
	setbuf(stdout, NULL);

	int pipefd[2];
	int pipefderr[2];

	if (pipe(pipefd) == -1) {
		printf("was not able to create pipe");
		exit(status);
	}

	if (pipe(pipefderr) == -1) {
		printf("was not able to create pipe for stderr");
		exit(status);
	}

	const char *command = argv[3];

	createDir(tmpDirectory);

	copy_file(command, sandboxCommand);
	chroot(tmpDirectory);

	int child_pid = fork();
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

		int exec_status = execv(sandboxCommand, &argv[3]);
		status = EXIT_SUCCESS;
	} else if (child_pid > 0) {
		// We're in parent
		close(pipefd[1]); // close write end of the pipe
		close(pipefderr[1]); // close write end of the pipe
		char buf;
		while (read(pipefd[0], &buf, 1) > 0) {
			write(STDOUT_FILENO, &buf, 1);
		}

		while (read(pipefderr[0], &buf, 1) > 0) {
			write(STDERR_FILENO, &buf, 1);
		}
		close(pipefd[0]); // close read end of the pipe
		close(pipefderr[0]); // close read end of the pipe
		fflush(stdout);
		fflush(stderr);
		int child_exit_code = 0;
		int wait_return_val = -1;
		wait_return_val = waitpid(child_pid, &child_exit_code, 0);
		if (wait_return_val == -1) {
			exit(EXIT_FAILURE);
		}

		if (WIFEXITED(child_exit_code)) {
			exit(WEXITSTATUS(child_exit_code));
		}
	} else {
	    printf("Error forking!");
	    status = EXIT_FAILURE;
	}

	exit(status);
}
