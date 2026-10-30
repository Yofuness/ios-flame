/*
 * proxon: proximity detection on/off through backboardd, as UIDevice.proximityMonitoringEnabled does
 * (B12 proximity, 2026-10-30; research/notes/proximity.md).
 *   proxon 1 [SECONDS]  request mode 1, hold it SECONDS (default 60), print active + near/far each second
 *   proxon MODE [SECONDS]  any other mode (valid: 0-6, 12, 13; BKSHIDServicesProximityDetectionModeIsValid)
 *   proxon 0            drop the request (only ours: the mode assertion lives in this process)
 * void BKSHIDServicesRequestProximityDetectionMode(int mode) keeps a BKSHIDUISensorService assertion in a static
 * (sModeAssertion); backboardd drops it when this process exits, so the process stays alive while it holds it.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>

int main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 2) {
			fprintf(stderr, "usage: proxon MODE [SECONDS]  (1 on, 0 off)\n");
			return 2;
		}
		void *h = dlopen("/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices", RTLD_NOW);
		if (!h) {
			fprintf(stderr, "proxon: %s\n", dlerror());
			return 1;
		}
		void (*request)(int) = dlsym(h, "BKSHIDServicesRequestProximityDetectionMode");
		BOOL (*active)(void) = dlsym(h, "BKSHIDServicesProximityDetectionActive");
		BOOL (*within)(void) = dlsym(h, "BKSHIDServicesGetObjectWithinProximity");
		if (!request) {
			fprintf(stderr, "proxon: no BKSHIDServicesRequestProximityDetectionMode\n");
			return 1;
		}
		if (!active)
			fprintf(stderr, "proxon: no BKSHIDServicesProximityDetectionActive\n");
		if (!within)
			fprintf(stderr, "proxon: no BKSHIDServicesGetObjectWithinProximity\n");
		int mode = atoi(argv[1]);
		int secs = argc > 2 ? atoi(argv[2]) : 60;
		request(mode);
		printf("requested proximity mode %d\n", mode);
		if (mode == 0)
			secs = 1;
		for (int i = 0; i < secs; i++) {
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
			printf("%3d s: active %s, %s\n", i + 1, active ? (active() ? "yes" : "no") : "?",
			    within ? (within() ? "near" : "far") : "?");
			fflush(stdout);
		}
		if (mode != 0)
			request(0);
	}
	return 0;
}
