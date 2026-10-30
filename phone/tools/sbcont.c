/*
 * sbcont: sandbox_container_path_for_pid as an extension's PlugInKit calls it (xnu 0240 test, 2026-10-30).
 *   sbcont [PID]     prints the container path or the error (PID 0 / none = self)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

int sandbox_container_path_for_pid(int pid, char *buf, size_t len);

int main(int argc, char **argv)
{
	char buf[MAXPATHLEN];
	int pid = argc > 1 ? atoi(argv[1]) : 0;
	int r = sandbox_container_path_for_pid(pid, buf, sizeof(buf));
	if (r != 0)
		printf("pid %d: error %d (%s), errno %d\n", pid, r, strerror(r < 0 ? errno : r), errno);
	else
		printf("pid %d: %s\n", pid, buf);
	return r != 0;
}
