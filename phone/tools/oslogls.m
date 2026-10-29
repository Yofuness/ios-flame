/*
 * oslog: stream the unified log (there is no `log` on iOS), like `log stream`.
 *   oslog [-t SECS] [-l] [PROCESS...]   only these processes (name, substring); -l = info+debug too
 * LoggingSupport's live store, through diagnosticd. Signed with oslog-ents.plist.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

@interface OSLogEventLiveStore : NSObject
+ (id)liveLocalStore;
- (void)prepareWithCompletionHandler:(void (^)(id source, NSError *error))handler;
@end
@interface OSLogEventLiveStream : NSObject
- (id)initWithLiveSource:(id)source;
- (void)setEventHandler:(void (^)(id event))handler;
- (void)setInvalidationHandler:(void (^)(int reason, id position))handler;
- (void)setFlags:(unsigned long long)flags;
- (void)activate;
@end
@interface OSLogEventProxy : NSObject
- (NSString *)composedMessage;
- (NSString *)process;
- (NSString *)sender;
- (NSString *)subsystem;
- (int)processIdentifier;
- (unsigned long long)logType;
- (struct timeval *)unixDate;
@end

static const char *type_name(unsigned long long t)
{
	switch (t) {
	case 0x00: return "Df";
	case 0x01: return "In";
	case 0x02: return "Db";
	case 0x10: return "Er";
	case 0x11: return "Ft";
	default: return "  ";
	}
}

int main(int argc, char **argv)
{
	@autoreleasepool {
		int secs = 0, levels = 0, first = 1;
		for (; first < argc && argv[first][0] == '-'; first++) {
			if (!strcmp(argv[first], "-t") && first + 1 < argc)
				secs = atoi(argv[++first]);
			else if (!strcmp(argv[first], "-l"))
				levels = 1;
		}
		NSMutableArray *procs = [NSMutableArray array];
		for (int i = first; i < argc; i++)
			[procs addObject:@(argv[i])];
		setvbuf(stdout, NULL, _IOLBF, 0);
		dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_NOW);
		__block OSLogEventLiveStream *stream = nil;
		[[NSClassFromString(@"OSLogEventLiveStore") liveLocalStore] prepareWithCompletionHandler:^(id source, NSError *err) {
			if (!source) {
				printf("oslog: no live source: %s\n", err.description.UTF8String);
				exit(1);
			}
			stream = [[NSClassFromString(@"OSLogEventLiveStream") alloc] initWithLiveSource:source];
			[stream setFlags:levels ? 0x1 | 0x2 | 0x4 : 0];	/* info, debug */
			[stream setEventHandler:^(OSLogEventProxy *e) {
				NSString *p = e.process ?: @"?";
				if (procs.count) {
					BOOL hit = NO;
					for (NSString *want in procs)
						if ([p rangeOfString:want].location != NSNotFound)
							hit = YES;
					if (!hit)
						return;
				}
				struct timeval *tv = e.unixDate;
				struct tm tm;
				time_t s = tv ? tv->tv_sec : 0;
				localtime_r(&s, &tm);
				printf("%02d:%02d:%02d.%03d %s %s[%d] (%s) %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
				       tv ? (int)(tv->tv_usec / 1000) : 0, type_name(e.logType), p.UTF8String,
				       e.processIdentifier, (e.sender ?: @"").UTF8String,
				       (e.composedMessage ?: @"").UTF8String);
			}];
			[stream setInvalidationHandler:^(int reason, id pos) {
				printf("oslog: stream ended (%d)\n", reason);
				exit(0);
			}];
			[stream activate];
		}];
		if (secs)
			[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:secs]];
		else
			[[NSRunLoop currentRunLoop] run];
	}
	return 0;
}
