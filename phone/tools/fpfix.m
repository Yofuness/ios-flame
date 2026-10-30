/*
 * fpfix.dylib: lets fileproviderd keep its FP library on our HFSX /private/var (Files "On My iPhone", B37,
 * 2026-10-30; research/notes/files-on-my-iphone.md "non-APFS"). Loaded with DYLD_INSERT_LIBRARIES from the
 * com.apple.FileProvider launchd job (tools/sunfish-install-fpfix.sh; kernel 0114 grants DYLD_* variables).
 * In fileproviderd only (getprogname), two FileProviderDaemon methods are replaced:
 *   -[FPDVolume isEligibleForFPFS:]      -> YES (was: NO unless f_fstypename is "apfs")
 *   -[FPDVolume resolveRoleForVolume:]   -> 3   (what APFS role 0 gives; HFS failed _APFSVolumeRole -> 0)
 * A missing class or selector is a no-op. One "fpfix: ..." os_log line per swizzle.
 * Off: tools/sunfish-install-fpfix.sh --undo (or FPFIX_OFF=1 in the job's environment).
 */
#import <Foundation/Foundation.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <stdlib.h>
#include <string.h>

static BOOL fpfix_eligible(id self, SEL _cmd, void *volinfo)
{
	return YES;
}

static int fpfix_role(id self, SEL _cmd, void *volinfo)
{
	return 3;
}

static int fpfix_done;

static void fpfix_swizzle(Class c, const char *name, IMP imp)
{
	SEL sel = sel_registerName(name);
	Method m = class_getInstanceMethod(c, sel);
	if (!m || ![c instancesRespondToSelector:sel]) {
		os_log(OS_LOG_DEFAULT, "fpfix: -[FPDVolume %{public}s] missing, skipped", name);
		return;
	}
	method_setImplementation(m, imp);
	os_log(OS_LOG_DEFAULT, "fpfix: -[FPDVolume %{public}s] replaced", name);
}

/* FileProviderDaemon may be initialized after us: retry on every image load until the class exists */
static void fpfix_try(const struct mach_header *mh, intptr_t slide)
{
	if (fpfix_done)
		return;
	Class c = objc_getClass("FPDVolume");
	if (!c)
		return;
	fpfix_done = 1;
	fpfix_swizzle(c, "isEligibleForFPFS:", (IMP)fpfix_eligible);
	fpfix_swizzle(c, "resolveRoleForVolume:", (IMP)fpfix_role);
}

__attribute__((constructor)) static void fpfix_init(void)
{
	if (strcmp(getprogname(), "fileproviderd") != 0)
		return;
	if (getenv("FPFIX_OFF")) {
		os_log(OS_LOG_DEFAULT, "fpfix: FPFIX_OFF set, not patching");
		return;
	}
	_dyld_register_func_for_add_image(fpfix_try);	/* also called for every image already loaded */
}
