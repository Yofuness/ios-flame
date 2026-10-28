/*
 * fcopy: copy one file's data with fcopyfile(COPYFILE_DATA), the path that got EFAULT on
 * /dev/ufsroot* files before xnu 0198.
 *   fcopy [-n] SRC DST   -n: F_NOCACHE on both fds, forcing cluster direct I/O
 */
#include <copyfile.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int nocache = argc > 1 && strcmp(argv[1], "-n") == 0;
	if (argc != 3 + nocache) {
		fprintf(stderr, "usage: fcopy [-n] SRC DST\n");
		return 2;
	}
	int s = open(argv[1 + nocache], O_RDONLY);
	int d = open(argv[2 + nocache], O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (s < 0 || d < 0) {
		perror("open");
		return 1;
	}
	if (nocache && (fcntl(s, F_NOCACHE, 1) || fcntl(d, F_NOCACHE, 1))) {
		perror("F_NOCACHE");
	}
	if (fcopyfile(s, d, NULL, COPYFILE_DATA) != 0) {
		perror("fcopyfile");
		return 1;
	}
	printf("fcopy: ok\n");
	return 0;
}
