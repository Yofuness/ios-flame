/*
 * autobright: Auto-Brightness through CoreBrightness's client, as the Settings switch does (ambient light,
 * 2026-10-30; research/notes/ambient-light.md).
 *   autobright          prints DisplayBrightnessAuto
 *   autobright 0|1      sets it, then prints it
 * CoreBrightness is a private framework: opened with dlopen, BrightnessSystemClient found by name.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>

@interface NSObject (BrightnessSystemClient)
- (id)copyPropertyForKey:(NSString *)key;
- (BOOL)setProperty:(id)value forKey:(NSString *)key;
@end

int main(int argc, char **argv)
{
	@autoreleasepool {
		if (!dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness", RTLD_NOW)) {
			fprintf(stderr, "autobright: %s\n", dlerror());
			return 1;
		}
		Class c = NSClassFromString(@"BrightnessSystemClient");
		if (c == nil) {
			fprintf(stderr, "autobright: no BrightnessSystemClient\n");
			return 1;
		}
		id client = [[c alloc] init];
		if (argc > 1) {
			BOOL ok = [client setProperty:@(atoi(argv[1]) != 0) forKey:@"DisplayBrightnessAuto"];
			printf("set DisplayBrightnessAuto %d -> %s\n", atoi(argv[1]) != 0, ok ? "ok" : "refused");
		}
		id v = [client copyPropertyForKey:@"DisplayBrightnessAuto"];
		printf("DisplayBrightnessAuto = %s\n", v ? [[v description] UTF8String] : "(none)");
	}
	return 0;
}
