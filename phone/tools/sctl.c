/*
 * sctl: read or set an integer sysctl (the phone has no sysctl binary).
 *   sctl NAME        print NAME's value (int; string sysctls print as text)
 *   sctl NAME=VALUE  set an int sysctl (decimal or 0x hex; > INT_MAX is written as 64 bits), then print it back
 * E.g. sctl kern.sweet_torch=50 (xnu 0190/0195, boot with sweettorch=1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

static int show(const char *name)
{
	static char buf[16384];	/* string sysctls like kern.sweet_mdpdump (~8 KB) */
	size_t len = sizeof(buf) - 1;
	if (sysctlbyname(name, buf, &len, NULL, 0) != 0) {
		perror(name);
		return 1;
	}
	if (len == sizeof(int)) {
		int v;
		memcpy(&v, buf, sizeof(v));
		printf("%s: %d\n", name, v);
	} else if (len == sizeof(long long)) {
		long long v;
		memcpy(&v, buf, sizeof(v));
		printf("%s: %lld\n", name, v);
	} else {
		buf[len] = 0;
		printf("%s: %s\n", name, buf);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: sctl NAME | NAME=VALUE\n");
		return 2;
	}
	char *eq = strchr(argv[1], '=');
	if (eq) {
		*eq = 0;
		unsigned long long q = strtoull(eq + 1, NULL, 0);	/* decimal or 0x hex */
		int v = (int)q;
		/* values above INT_MAX go as 64 bits (quad sysctls such as kern.sweet_mdp_src) */
		int big = q > 0x7fffffffull;
		if (sysctlbyname(argv[1], NULL, NULL, big ? (void *)&q : (void *)&v, big ? sizeof(q) : sizeof(v)) != 0) {
			perror(argv[1]);
			return 1;
		}
	}
	return show(argv[1]);
}
