/*
 * mgq: MobileGestalt answers (ambient light, 2026-10-30).
 *   mgq KEY...     prints KEY = the answer (CFCopyDescription), or "(none)"
 * libMobileGestalt is opened with dlopen: the SDK has no stub for it.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv)
{
	void *mg = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW);
	CFTypeRef (*copy)(CFStringRef, CFDictionaryRef) = mg ? (CFTypeRef (*)(CFStringRef, CFDictionaryRef))dlsym(mg, "MGCopyAnswer") : NULL;
	if (copy == NULL) {
		fprintf(stderr, "mgq: no MGCopyAnswer: %s\n", dlerror());
		return 1;
	}
	if (argc < 2) {
		fprintf(stderr, "usage: mgq KEY...\n");
		return 2;
	}
	for (int i = 1; i < argc; i++) {
		CFStringRef key = CFStringCreateWithCString(NULL, argv[i], kCFStringEncodingUTF8);
		CFTypeRef v = copy(key, NULL);
		char buf[512] = "(none)";
		if (v != NULL) {
			CFStringRef d = CFCopyDescription(v);
			CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8);
			CFRelease(d);
			CFRelease(v);
		}
		printf("%s = %s\n", argv[i], buf);
		CFRelease(key);
	}
	return 0;
}
