/*
 * sysq: read an int or string sysctl, or set an int one (xnu 0238 v2: kern.sweet_ui_idle/preempt/first).
 *   sysq NAME          print the value (4/8-byte values as numbers, else as text)
 *   sysq NAME VALUE    set an int sysctl (decimal or 0x hex), then print it back
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/sysctl.h>

int main(int argc, char **argv)
{
	char buf[4096];
	size_t len = sizeof(buf) - 1;
	if (argc != 2 && argc != 3) {
		fprintf(stderr, "usage: sysq NAME [INT]\n");
		return 2;
	}
	if (argc == 3) {
		int v = (int)strtol(argv[2], NULL, 0);
		if (sysctlbyname(argv[1], NULL, NULL, &v, sizeof(v)) != 0) {
			perror(argv[1]);
			return 1;
		}
	}
	if (sysctlbyname(argv[1], buf, &len, NULL, 0) != 0) {
		perror(argv[1]);
		return 1;
	}
	if (len == sizeof(int32_t)) {
		int32_t v;
		memcpy(&v, buf, sizeof(v));
		printf("%d\n", v);
	} else if (len == sizeof(int64_t) && buf[len - 1] != 0	/* a 7-char string ends in NUL */) {
		int64_t v;
		memcpy(&v, buf, sizeof(v));
		printf("%lld\n", (long long)v);
	} else {
		buf[len] = 0;
		printf("%s\n", buf);
	}
	return 0;
}
