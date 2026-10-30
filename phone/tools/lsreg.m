/*
 * lsreg: LaunchServices registration on the phone (no uicache on iOS).
 *   lsreg APP_PATH...        register apps (e.g. /Applications/Calculator.app)
 *   lsreg -u APP_PATH...     unregister
 *   lsreg -r                 rebuild the app databases (system + internal + user)
 *   lsreg -l                 list installed apps: bundle id, type, path
 *   lsreg -p APPEX...        register plugins (.appex) by themselves
 *   lsreg -i BUNDLEID        show a plugin's LaunchServices record (data container, host, extension point)
 *   lsreg -c BUNDLEID APPEX  create the plugin's data container (MobileContainerManager), re-register APPEX
 * Signed with com.apple.migrationpluginwrapper's entitlements.
 */
#import <Foundation/Foundation.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <objc/runtime.h>

@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)registerApplication:(NSURL *)url;
- (BOOL)unregisterApplication:(NSURL *)url;
- (BOOL)registerPlugin:(NSURL *)url;
- (BOOL)_LSPrivateRebuildApplicationDatabasesForSystemApps:(BOOL)s internal:(BOOL)i user:(BOOL)u;
- (NSArray *)allInstalledApplications;
@end

@interface LSApplicationProxy : NSObject
- (NSString *)applicationIdentifier;
- (NSString *)applicationType;
- (NSURL *)bundleURL;
- (BOOL)isPlaceholder;
@end

@interface LSApplicationExtensionRecord : NSObject
- (instancetype)initWithBundleIdentifier:(NSString *)bid error:(NSError **)err;
- (NSURL *)dataContainerURL;
- (id)containingBundleRecord;
- (NSString *)extensionPointIdentifier;
@end

@interface MCMContainer : NSObject
+ (instancetype)containerWithIdentifier:(NSString *)bid createIfNecessary:(BOOL)c existed:(BOOL *)e error:(NSError **)err;
- (NSURL *)url;
@end

/* print what LaunchServices has for a plugin (patch files5: Files "On My iPhone" needs its data container) */
static int show_ext(NSString *bid)
{
	Class c = NSClassFromString(@"LSApplicationExtensionRecord");
	if (!c || ![c instancesRespondToSelector:@selector(initWithBundleIdentifier:error:)]) {
		printf("no LSApplicationExtensionRecord\n");
		return 1;
	}
	NSError *err = nil;
	LSApplicationExtensionRecord *r = [[c alloc] initWithBundleIdentifier:bid error:&err];
	if (!r) {
		printf("%s: not in LaunchServices (%s)\n", bid.UTF8String, err.description.UTF8String);
		return 1;
	}
	if ([r respondsToSelector:@selector(dataContainerURL)])
		printf("dataContainerURL: %s\n", r.dataContainerURL.path.UTF8String ?: "(none)");
	if ([r respondsToSelector:@selector(containingBundleRecord)])
		printf("containingBundle: %s\n", [[r.containingBundleRecord description] UTF8String] ?: "(none)");
	if ([r respondsToSelector:@selector(extensionPointIdentifier)])
		printf("extensionPoint:   %s\n", r.extensionPointIdentifier.UTF8String ?: "(none)");
	return 0;
}

/* what -registerPlugin: does, keeping the status it throws away */
static int reg_plugin(LSApplicationWorkspace *ws, const char *path)
{
	NSURL *u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
	/* not exported: -registerPlugin: calls it with bl at +0x28, target = start + 0x334 (20H392) */
	IMP imp = [ws methodForSelector:@selector(registerPlugin:)];
	printf("registerPlugin: at %p\n", (void *)imp);
	if (!imp)
		return -1;
	uint32_t bl = *(uint32_t *)((char *)imp + 0x28);
	int (*reg)(NSURL *, NSDictionary *) = (bl >> 26) == 0x25 ?
		(void *)((char *)imp + 0x28 + ((int32_t)(bl << 6) >> 4)) : NULL;
	int st = reg ? reg(u, nil) : -1;
	printf("register plugin %s -> %d\n", path, st);
	return st;
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		if (getenv("LSREG_UID")) {	/* e.g. 501 = mobile, whose database SpringBoard uses */
			setgid(atoi(getenv("LSREG_UID")));
			setuid(atoi(getenv("LSREG_UID")));
		}
		LSApplicationWorkspace *ws = [NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace];
		if (argc < 2) {
			fprintf(stderr, "usage: lsreg [-u] APP... | -r | -l | -p APPEX... | -i BUNDLEID | -c BUNDLEID APPEX\n");
			return 2;
		}
		if (!strcmp(argv[1], "-r")) {
			BOOL ok = [ws _LSPrivateRebuildApplicationDatabasesForSystemApps:YES internal:YES user:YES];
			printf("rebuild -> %d\n", ok);
			return !ok;
		}
		if (!strcmp(argv[1], "-l")) {
			for (LSApplicationProxy *p in [ws allInstalledApplications])
				printf("%-45s %-8s %s%s\n", p.applicationIdentifier.UTF8String, p.applicationType.UTF8String,
				       p.bundleURL.path.UTF8String, p.isPlaceholder ? " (placeholder)" : "");
			return 0;
		}
		if (!strcmp(argv[1], "-p")) {
			int bad = 0;
			for (int i = 2; i < argc; i++)
				bad += reg_plugin(ws, argv[i]) != 0;
			return bad != 0;
		}
		if (!strcmp(argv[1], "-i") && argc == 3)
			return show_ext([NSString stringWithUTF8String:argv[2]]);
		if (!strcmp(argv[1], "-c") && argc == 4) {
			/* files5: our /var never had a MobileInstallation pass, so plugins have no data container */
			NSString *bid = [NSString stringWithUTF8String:argv[2]];
			if (!dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW))
				printf("dlopen MobileContainerManager: %s\n", dlerror());
			Class mc = NSClassFromString(@"MCMPluginKitPluginDataContainer");
			SEL sel = @selector(containerWithIdentifier:createIfNecessary:existed:error:);
			if (!mc || ![mc respondsToSelector:sel]) {
				printf("no MCMPluginKitPluginDataContainer +containerWithIdentifier:createIfNecessary:existed:error:\n");
				return 1;
			}
			BOOL existed = NO;
			NSError *err = nil;
			MCMContainer *ct = [mc containerWithIdentifier:bid createIfNecessary:YES existed:&existed error:&err];
			if (!ct) {
				printf("container: failed (%s)\n", err.description.UTF8String);
				return 1;
			}
			printf("container: %s (existed %d)\n",
			       [ct respondsToSelector:@selector(url)] ? ct.url.path.UTF8String : "(no -url)", existed);
			int st = reg_plugin(ws, argv[3]);
			show_ext(bid);
			return st != 0;
		}
		BOOL un = !strcmp(argv[1], "-u");
		int bad = 0;
		for (int i = un ? 2 : 1; i < argc; i++) {
			NSURL *u = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[i]]];
			BOOL ok = un ? [ws unregisterApplication:u] : [ws registerApplication:u];
			printf("%s %s -> %d\n", un ? "unregister" : "register", argv[i], ok);
			bad += !ok;
		}
		return bad != 0;
	}
}
