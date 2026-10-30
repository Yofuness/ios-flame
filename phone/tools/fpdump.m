/* fpdump [--enable ID]: list FileProvider domains the way Files' DocumentManager sees them (B37). Uses
   +[FPProviderDomain beginMonitoringProviderDomainChangesWithHandler:] (the same call as DocumentManager's
   startSearchingSourcesForBundleIdentifier:), prints each domain's flags, exits after the first update (10 s max).
   --enable ID: -[FPProviderDomain setEnabled:completionHandler:] then setHiddenByUser:NO on the domain whose
   identifier or providerID matches ID (both go to fileproviderd: FPDDaemon setEnabled:forDomainIdentifier:...). */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/message.h>

static BOOL flag(id o, const char *sel)
{
	SEL s = sel_registerName(sel);
	if (![o respondsToSelector:s])
		return NO;
	return ((BOOL (*)(id, SEL))objc_msgSend)(o, s);
}

static id obj(id o, const char *sel)
{
	SEL s = sel_registerName(sel);
	return [o respondsToSelector:s] ? ((id (*)(id, SEL))objc_msgSend)(o, s) : nil;
}

static void dump(id d)
{
	static const char *flags[] = { "isEnabled", "isHidden", "isHiddenByUser", "isHiddenByProvider",
		"supportsEnumeration", "needsAuthentication", "isAvailableSystemWide", "canDisconnect", "isEjectable",
		"isUsingFPFS", "isReadOnly", "canDisable", "isConnectedToAppExtension" };
	printf("domain %s provider %s\n", [[obj(d, "identifier") description] UTF8String],
	       [[obj(d, "providerID") description] UTF8String]);
	printf("  displayName %s / %s, topLevelBundle %s\n", [[obj(d, "domainDisplayName") description] UTF8String],
	       [[obj(d, "providerDisplayName") description] UTF8String],
	       [[obj(d, "topLevelBundleIdentifier") description] UTF8String]);
	printf("  flags");
	for (unsigned i = 0; i < sizeof flags / sizeof *flags; i++)
		printf(" %s=%d", flags[i], flag(d, flags[i]));
	printf("\n");
	for (NSURL *u in obj(d, "storageURLs"))
		printf("  storage %s\n", u.path.UTF8String);
	id err = obj(d, "error");
	if (err)
		printf("  error %s\n", [[err description] UTF8String]);
	printf("  desc %s\n", [[d description] UTF8String]);
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		dlopen("/System/Library/Frameworks/FileProvider.framework/FileProvider", RTLD_NOW);
		Class C = NSClassFromString(@"FPProviderDomain");
		SEL mon = @selector(beginMonitoringProviderDomainChangesWithHandler:);
		if (!C || ![C respondsToSelector:mon]) {
			printf("no FPProviderDomain beginMonitoring SPI\n");
			return 1;
		}
		NSString *want = argc > 2 && !strcmp(argv[1], "--enable") ? @(argv[2]) : nil;
		dispatch_semaphore_t done = dispatch_semaphore_create(0);
		__block int fired = 0, rc = 0;
		id token = ((id (*)(id, SEL, id))objc_msgSend)(C, mon, ^(id domains, NSError *error) {
			if (__sync_fetch_and_add(&fired, 1))
				return;
			NSArray *list = [domains isKindOfClass:[NSDictionary class]] ? [domains allValues] : domains;
			printf("%lu domains%s%s\n", (unsigned long)list.count, error ? ", error " : "",
			       error ? error.description.UTF8String : "");
			id target = nil;
			for (id d in list) {
				if (!want)
					dump(d);
				else if ([want isEqual:obj(d, "identifier")] || [want isEqual:obj(d, "providerID")])
					target = d;
			}
			if (!want) {
				dispatch_semaphore_signal(done);
				return;
			}
			if (!target) {
				printf("no domain %s\n", want.UTF8String);
				rc = 1;
				dispatch_semaphore_signal(done);
				return;
			}
			dump(target);
			SEL en = @selector(setEnabled:completionHandler:), hid = @selector(setHiddenByUser:completionHandler:);
			if (![target respondsToSelector:en]) {
				printf("no setEnabled:completionHandler: SPI\n");
				rc = 1;
				dispatch_semaphore_signal(done);
				return;
			}
			((void (*)(id, SEL, BOOL, id))objc_msgSend)(target, en, YES, ^(NSError *e) {
				printf("setEnabled:YES -> %s\n", e ? e.description.UTF8String : "ok");
				if (e)
					rc = 1;
				if (![target respondsToSelector:hid]) {
					printf("no setHiddenByUser:completionHandler: SPI\n");
					dispatch_semaphore_signal(done);
					return;
				}
				((void (*)(id, SEL, BOOL, id))objc_msgSend)(target, hid, NO, ^(NSError *e2) {
					printf("setHiddenByUser:NO -> %s\n", e2 ? e2.description.UTF8String : "ok");
					if (e2)
						rc = 1;
					dispatch_semaphore_signal(done);
				});
			});
		});
		if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC))) {
			printf("timeout: %s after 10 s\n", fired ? "handler fired, SPI call did not return" : "no update");
			rc = 2;
		}
		SEL end = @selector(endMonitoringProviderDomainChanges:);
		if (token && [C respondsToSelector:end])
			((void (*)(id, SEL, id))objc_msgSend)(C, end, token);
		return rc;
	}
}
