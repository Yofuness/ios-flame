/*
 * SweetMetal2.bundle: the Metal driver for the Adreno 640 on metal2vulkan + Turnip (boot-arg sweetmtl=2: the kernel's
 * SweetAccelerator names this bundle and class SweetMTL2Device). Metal.framework loads it in every process that asks
 * for a device; -initWithAcceleratorPort: hands back the frontend's device (NativeBridgeMetalFrontend.mm, UniXNU's,
 * over the bridge in bridge/, which drives Turnip, the libvulkan_freedreno.dylib inside this bundle, through the
 * kernel's v2 user client). Which processes get one: backboardd always, others when /var/local/mtl2-clients lists
 * them ("*" = all, "-NAME" excludes NAME); the rest get none, so their frameworks take their CPU paths.
 * Log: /dev/console and /var/local/mtl2.log (when it exists, mode 666), lines "MTL2[pid name]: ...".
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <objc/message.h>
#include <unistd.h>
#include <string.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

/*
 * /var/local/mtl2.log, opened for appending. Clients run as mobile and cannot create or rename files in
 * /var/local, so the log is kept small by truncating it in place (it is mode 666) once it passes 8 MB.
 */
static int sm2_open_log(void)
{
	int fd = open("/var/local/mtl2.log", O_WRONLY | O_APPEND);
	struct stat st;
	if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > (8 << 20) && ftruncate(fd, 0) == 0)
		dprintf(fd, "MTL2[%d %s]: log truncated at %lld bytes\n", getpid(), getprogname(), (long long)st.st_size);
	return fd;
}

extern "C" void sm2_logs(const char *s)
{
	char buf[700];
	int n = snprintf(buf, sizeof(buf), "MTL2[%d %s]: %s\n", getpid(), getprogname(), s);
	if (n > (int)sizeof(buf) - 1)
		n = (int)sizeof(buf) - 1;
	static int console = -2, file = -2;
	if (console == -2)
		console = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (file == -2)
		file = sm2_open_log();
	if (console >= 0)
		write(console, buf, (size_t)n);
	if (file >= 0)
		write(file, buf, (size_t)n);
}

static void sm2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sm2_log(const char *fmt, ...)
{
	char buf[600];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	sm2_logs(buf);
}

/*
 * The A640 does not snoop the CPU caches: libSystem's sys_dcache_flush (a no-op where the GPU is coherent) must
 * really clean and invalidate here. User space may (SCTLR_EL1.UCI). This definition wins at link time, for the
 * bridge and the frontend alike (IOSurface imports).
 */
extern "C" void sys_dcache_flush(void *start, size_t len)
{
	uintptr_t a = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + len;
	for (; a < end; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Metal validation failures abort by default; frameworks with Apple-GPU-only kernels (MPS) report one on any other device. */
__attribute__((constructor)) static void sm2_error_mode(void)
{
	void (*set)(unsigned, unsigned) = (void (*)(unsigned, unsigned))dlsym(RTLD_DEFAULT, "MTLFailureTypeSetErrorModeType");
	for (unsigned t = 0; set && t < 5; t++)
		set(t, 5);	/* 5 = NSLog */
}

/* Selectors the frontend lacks, each logged once (the frontend then raises the usual unrecognised-selector exception) */
static void missing(const char *cls, const char *sel)
{
	static NSMutableSet *seen;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		seen = [NSMutableSet new];
	});
	NSString *k = [NSString stringWithFormat:@"%s %s", cls, sel];
	@synchronized(seen) {
		if ([seen containsObject:k])
			return;
		[seen addObject:k];
	}
	sm2_log("MISSING %s -%s", cls, sel);
}

@interface NBDeviceMethods : NSObject
@end

/* iOS 16's QuartzCore asks for its precompiled pipeline archive (Apple GPU code): none, it builds its pipelines itself. */
@interface NBDeviceMethods (SweetMetal2)
@end
@implementation NBDeviceMethods (SweetMetal2)
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error
{
	if (error)
		*error = [NSError errorWithDomain:@"SweetMetal2" code:1 userInfo:@{ NSLocalizedDescriptionKey : @"no pipeline libraries" }];
	return nil;
}
@end

static bool listed(void)
{
	if (!strcmp(getprogname(), "backboardd"))
		return true;
	FILE *f = fopen("/var/local/mtl2-clients", "r");
	if (!f)
		return false;
	char text[2048] = { 0 };
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = 0;
	/* names, "*" = all, "-NAME" = never NAME (wins over "*") */
	char *save = NULL;
	bool yes = false;
	for (char *name = strtok_r(text, " ,\t\r\n", &save); name; name = strtok_r(NULL, " ,\t\r\n", &save)) {
		if (name[0] == '-' && !strcmp(name + 1, getprogname()))
			return false;
		if (!strcmp(name, "*") || !strcmp(name, getprogname()))
			yes = true;
	}
	return yes;
}

@interface _MTLDevice : NSObject
@end

@interface SweetMTL2Device : _MTLDevice
- (id)initWithAcceleratorPort:(mach_port_t)port;
@end

@implementation SweetMTL2Device
- (id)initWithAcceleratorPort:(mach_port_t)port
{
	/* this object is dropped for the frontend's device: initialised, so its -[_MTLDevice dealloc] has queues to release */
	if (!(self = [super init]))
		return nil;
	static id device;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		if (!listed()) {
			sm2_log("not listed (/var/local/mtl2-clients): no Metal device");
			return;
		}
		/* Turnip's per-name BO census (TU_DEBUG=bos, every 300 submits: phone/turnip/patches/0101) into
		 * /var/local/tubos/NAME-PID.log while /var/local/tubos exists (mode 777) */
		if (access("/var/local/tubos", W_OK) == 0) {
			char log[256];
			snprintf(log, sizeof(log), "/var/local/tubos/%s-%d.log", getprogname(), getpid());
			setenv("TU_DEBUG", "bos", 1);
			setenv("MESA_LOG_FILE", log, 1);
			sm2_log("BO census -> %s", log);
		}
		NSString *bundle = [[NSBundle bundleForClass:[SweetMTL2Device class]] bundlePath];
		NSString *turnip = [bundle stringByAppendingPathComponent:@"libvulkan_freedreno.dylib"];
		char cache[256];
		mkdir("/var/local/m2v-cache", 0777);
		chmod("/var/local/m2v-cache", 0777);
		snprintf(cache, sizeof(cache), "/var/local/m2v-cache/%s", getprogname());
		mkdir(cache, 0777);
		if (access(cache, W_OK) != 0) {
			/* a sandboxed process: its own caches directory */
			char dir[256];
			if (confstr(_CS_DARWIN_USER_CACHE_DIR, dir, sizeof(dir) - 8)) {
				snprintf(cache, sizeof(cache), "%sm2v", dir);
				mkdir(cache, 0700);
			}
		}
		/* plain rows everywhere: no Apple framebuffer compression, no 10-bit deep framebuffer */
		setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
		CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
		CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
		CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
		/* the runtime's VkPipeline makings count into the telemetry (V row) */
		setenv("METAL2VULKAN_LOG_PIPELINES", "1", 0);
		/* debugging: KEY=VALUE lines of /var/local/mtl2-env into this process's environment */
		if (FILE *ef = fopen("/var/local/mtl2-env", "r")) {
			char line[256];
			while (fgets(line, sizeof(line), ef)) {
				line[strcspn(line, "\r\n")] = 0;
				char *eq = strchr(line, '=');
				if (line[0] != '#' && eq) {
					*eq = 0;
					setenv(line, eq + 1, 1);
					sm2_log("env %s=%s", line, eq + 1);
				}
			}
			fclose(ef);
		}
		/* debugging: Turnip options for this process from /var/local/mtl2-tu-debug (TU_DEBUG), its log to /var/local/turnip-<proc>.log (or /var/local/turnip/<proc>-<pid>.log) */
		{
			char opts[128] = { 0 };
			FILE *f = fopen("/var/local/mtl2-tu-debug", "r");
			if (f) {
				if (fgets(opts, sizeof(opts), f))
					opts[strcspn(opts, "\r\n")] = 0;
				fclose(f);
			}
			if (opts[0]) {
				char path[128];
				/* one log per process instance while /var/local/turnip exists (mode 777): a respawned
				 * client would otherwise truncate its predecessor's log */
				if (access("/var/local/turnip", W_OK) == 0)
					snprintf(path, sizeof(path), "/var/local/turnip/%s-%d.log", getprogname(), getpid());
				else
					snprintf(path, sizeof(path), "/var/local/turnip-%s.log", getprogname());
				setenv("TU_DEBUG", opts, 1);
				setenv("MESA_LOG_FILE", path, 1);
				sm2_log("TU_DEBUG=%s, log %s", opts, path);
			}
		}
		uint64_t registryID = 0;
		IORegistryEntryGetRegistryEntryID(port, &registryID);
		double t0 = CFAbsoluteTimeGetCurrent();
		@try {
			device = native_bridge_metal_open(turnip.UTF8String, cache, registryID, missing);
		} @catch (NSException *e) {
			sm2_log("device: exception %s", e.reason.UTF8String);
			device = nil;
		}
		sm2_log("device %s in %.0f ms (cache %s)%s%s", device ? [[device name] UTF8String] : "refused",
			(CFAbsoluteTimeGetCurrent() - t0) * 1000, cache, device ? "" : ": ", device ? "" : nmb_last_error());
		/*
		 * Extended-range (XR10) formats: the frontend withholds them so backboardd keeps an 8-bit framebuffer; other
		 * processes may render into them (AegirPoster's CAMetalLayer throws "unsupported extended range format" without).
		 */
		if (device && strcmp(getprogname(), "backboardd") != 0)
			class_replaceMethod(object_getClass(device), sel_registerName("supportsExtendedXR10Formats"),
					    imp_implementationWithBlock(^BOOL(id me) { (void)me; return YES; }), "B16@0:8");
		/* image bases, for symbolicating pstack samples (atos -o out/SweetMetal2.dSYM -l BASE ADDR) */
		Dl_info self = { 0 }, tu = { 0 };
		dladdr((void *)sm2_logs, &self);
		void *th = dlopen(turnip.UTF8String, RTLD_NOW | RTLD_NOLOAD);
		void *vk = th ? dlsym(th, "vk_icdGetInstanceProcAddr") : NULL;
		if (vk)
			dladdr(vk, &tu);
		sm2_log("images: SweetMetal2 %p, libvulkan_freedreno %p", self.dli_fbase, tu.dli_fbase);
		/* backboardd: the frontend's counters every 10 s, into /var/local/mtl2.log only (with /var/local/mtl2-telemetry) */
		if (device && !strcmp(getprogname(), "backboardd"))
			dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
				for (;;) {
					sleep(10);
					if (access("/var/local/mtl2-telemetry", F_OK) != 0)
						continue;
					static char text[8192];
					native_bridge_metal_telemetry(text, sizeof(text));
					int fd = sm2_open_log();
					if (fd >= 0) {
						dprintf(fd, "MTL2[%d %s]: telemetry\n%s", getpid(), getprogname(), text);
						close(fd);
					}
				}
			});
	});
	return device;
}
@end

/*
 * Control Center flashlight (switch file /var/local/sweet-flashlight, SpringBoard only): AVFlashlight's methods are
 * replaced so they drive the kernel's kern.sweet_torch (mA, sweettorch=1) instead of the camera stack (no Fig object).
 * Level 1.0 = 100 mA, the kernel's cap.
 */
static float sm2_torch_level;	/* the stored level, 0 = off */
static float sm2_torch_last = 1.0f;

static BOOL sm2_torch_set(id self, float level)
{
	int ma = (int)(level * 100.0f + 0.5f);
	if (ma < 0)
		ma = 0;
	int err = sysctlbyname("kern.sweet_torch", NULL, NULL, &ma, sizeof(ma));
	if (err)
		sm2_log("flashlight: kern.sweet_torch=%d failed (%d)", ma, errno);
	else if (ma)
		sm2_log("flashlight: on %d mA", ma);
	else
		sm2_log("flashlight: off");
	[self willChangeValueForKey:@"flashlightLevel"];
	sm2_torch_level = err ? 0.0f : level;
	if (!err && level > 0.0f)
		sm2_torch_last = level;
	[self didChangeValueForKey:@"flashlightLevel"];
	return err == 0;
}

static void sm2_torch_swizzle(void)
{
	Class c = objc_getClass("AVFlashlight");
	if (!c)
		return;
	Class m = object_getClass(c);
	struct { Class cls; const char *sel; id imp; } r[] = {
		{ m, "hasFlashlight", ^BOOL(id s) { return YES; } },
		{ c, "init", ^id(id s) {
			struct objc_super sup = { s, [NSObject class] };
			return ((id (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, @selector(init));
		} },
		{ c, "dealloc", ^void(id s) {
			struct objc_super sup = { s, [NSObject class] };
			((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, NSSelectorFromString(@"dealloc"));
		} },
		{ c, "isAvailable", ^BOOL(id s) { return YES; } },
		{ c, "isOverheated", ^BOOL(id s) { return NO; } },
		{ c, "turnPowerOnWithError:", ^BOOL(id s, NSError **e) { return sm2_torch_set(s, sm2_torch_last); } },
		{ c, "turnPowerOff", ^void(id s) { sm2_torch_set(s, 0.0f); } },
		{ c, "setFlashlightLevel:withError:", ^BOOL(id s, float level, NSError **e) {
			sm2_log("flashlight: level %.2f", level);
			return sm2_torch_set(s, level);
		} },
		{ c, "flashlightLevel", ^float(id s) { return sm2_torch_level; } },
	};
	for (auto &x : r) {
		Method mt = class_getInstanceMethod(x.cls, sel_registerName(x.sel));
		if (!mt) {
			sm2_log("flashlight: AVFlashlight %s missing", x.sel);
			continue;
		}
		method_setImplementation(mt, imp_implementationWithBlock(x.imp));
	}
	sm2_log("flashlight: AVFlashlight -> kern.sweet_torch");
}

/* Control Center drops modules whose UIRequiredDeviceCapabilities MobileGestalt denies (FlashlightModule: camera-flash,
 * k90a: no tile). Clear them for that module only; the original list is logged once. */
static void sm2_torch_module(void)
{
	if (!dlopen("/System/Library/PrivateFrameworks/ControlCenterServices.framework/ControlCenterServices", RTLD_NOW))
		sm2_log("flashlight: ControlCenterServices: %s", dlerror());
	Class c = objc_getClass("CCSModuleMetadata");
	SEL s = sel_registerName("requiredDeviceCapabilities");
	Method mt = c ? class_getInstanceMethod(c, s) : NULL;
	if (!mt) {
		sm2_log("flashlight: CCSModuleMetadata%s missing", c ? " -requiredDeviceCapabilities" : "");
		return;
	}
	IMP orig = method_getImplementation(mt);
	method_setImplementation(mt, imp_implementationWithBlock(^id(id self) {
		id caps = ((id (*)(id, SEL))orig)(self, s);
		id mid = [self respondsToSelector:@selector(moduleIdentifier)] ? [self performSelector:@selector(moduleIdentifier)] : nil;
		if (![mid isEqual:@"com.apple.control-center.FlashlightModule"])
			return caps;
		static bool logged;
		if (!logged) {
			logged = true;
			sm2_log("flashlight: module caps %s cleared", [[caps description] UTF8String]);
		}
		return [caps isKindOfClass:[NSSet class]] ? (id)[NSSet set] : (id)@[];
	}));
	sm2_log("flashlight: CCSModuleMetadata caps hook");
}

__attribute__((constructor)) static void sm2_torch_init(void)
{
	if (strcmp(getprogname(), "SpringBoard") != 0 || access("/var/local/sweet-flashlight", F_OK) != 0)
		return;
	/* SBUIFlashlightController makes its AVFlashlight later; loading the framework now is enough to swizzle first. */
	if (!dlopen("/System/Library/PrivateFrameworks/AVFCapture.framework/AVFCapture", RTLD_NOW))
		sm2_log("flashlight: AVFCapture: %s", dlerror());
	sm2_torch_swizzle();
	sm2_torch_module();
}

/*
 * Orientation from a file (no sensors; /var/local/sweet-orient, SpringBoard only): a 1 s timer reads one
 * UIDeviceOrientation (1 portrait, 2 upside down, 3 landscape left, 4 landscape right). "N" = method A, AX
 * setDeviceOrientation:; "B N" = method B, SpringBoard's SBDeviceOrientationUpdateManager; "C N" = method C,
 * BKSHIDServicesSetDeviceOrientationForAutomation after an orientation unlock; "D N [S]" = method D, in backboardd:
 * BKOrientationManager updateDeviceOrientation:N changeSource:S. Delete the file to turn it off.
 */
static void sm2_orient_b(long n)
{
	Class c = objc_getClass("SBDeviceOrientationUpdateManager");
	if (!c) {
		sm2_log("orient: SBDeviceOrientationUpdateManager missing");
		return;
	}
	unsigned cnt = 0;
	Method *ms = class_copyMethodList(object_getClass(c), &cnt);
	for (unsigned i = 0; i < cnt; i++)
		sm2_log("orient: B class method +%s", sel_getName(method_getName(ms[i])));
	free(ms);
	id mgr = nil;
	static const char *const names[] = { "sharedInstance", "sharedManager", "defaultManager" };
	for (const char *s : names)
		if (class_respondsToSelector(object_getClass(c), sel_registerName(s))) {
			mgr = ((id (*)(id, SEL))objc_msgSend)(c, sel_registerName(s));
			break;
		}
	if (!mgr) {
		id app = ((id (*)(id, SEL))objc_msgSend)(objc_getClass("SpringBoard"), sel_registerName("sharedApplication"));
		id objs[] = { app, app ? ((id (*)(id, SEL))objc_msgSend)(app, sel_registerName("delegate")) : nil };
		for (id o : objs) {
			unsigned ni = 0;
			Ivar *iv = o ? class_copyIvarList(object_getClass(o), &ni) : NULL;
			for (unsigned i = 0; i < ni && !mgr; i++) {
				const char *t = ivar_getTypeEncoding(iv[i]);
				if (t && strstr(t, "SBDeviceOrientationUpdateManager"))
					mgr = object_getIvar(o, iv[i]);
			}
			free(iv);
		}
	}
	sm2_log("orient: B manager %p", mgr);
	if (!mgr)
		return;
	SEL e = sel_registerName("_enqueueOrientationUpdateToDeviceOrientation:");
	SEL l = sel_registerName("_legacy_enqueueOrientationUpdateToDeviceOrientation:source:");
	if ([mgr respondsToSelector:e]) {
		((void (*)(id, SEL, long))objc_msgSend)(mgr, e, n);
		sm2_log("orient: B enqueue %ld", n);
	} else if ([mgr respondsToSelector:l]) {
		((void (*)(id, SEL, long, long))objc_msgSend)(mgr, l, n, 0);
		sm2_log("orient: B legacy enqueue %ld", n);
	} else
		sm2_log("orient: B no enqueue selector");
}

static void sm2_orient_a(long n)
{
	Class c = objc_getClass("AXBackBoardServer");
	SEL s = sel_registerName("setDeviceOrientation:");
	Method m = c ? class_getInstanceMethod(c, s) : NULL;
	if (!m) {
		sm2_log("orient: AXBackBoardServer%s missing", c ? " -setDeviceOrientation:" : "");
		return;
	}
	id srv = ((id (*)(id, SEL))objc_msgSend)(c, sel_registerName("server"));
	sm2_log("orient: AX setDeviceOrientation %ld (types %s, server %p)", n, method_getTypeEncoding(m), srv);
	((void (*)(id, SEL, long))objc_msgSend)(srv, s, n);
}

/* Method C: backboardd's automation entry (BackBoardServices MIG), as if the sensor reported it; unlock first. */
static void sm2_orient_c(long n)
{
	static void (*set_auto)(int), (*unlock_o)(void);
	static bool loaded;
	if (!loaded) {
		loaded = true;
		void *h = dlopen("/System/Library/PrivateFrameworks/BackBoardServices.framework/BackBoardServices", RTLD_NOW);
		set_auto = h ? (void (*)(int))dlsym(h, "BKSHIDServicesSetDeviceOrientationForAutomation") : NULL;
		unlock_o = h ? (void (*)(void))dlsym(h, "BKSHIDServicesUnlockOrientation") : NULL;
		sm2_log("orient: C set %p unlock %p", (void *)set_auto, (void *)unlock_o);
	}
	Class lc = objc_getClass("SBOrientationLockManager");
	id lm = [lc respondsToSelector:@selector(sharedInstance)] ? [lc performSelector:@selector(sharedInstance)] : nil;
	SEL ul = sel_registerName("unlock"), il = sel_registerName("isUserLocked");
	int locked = [lm respondsToSelector:il] ? ((BOOL (*)(id, SEL))objc_msgSend)(lm, il) : -1;
	if ([lm respondsToSelector:ul])
		((void (*)(id, SEL))objc_msgSend)(lm, ul);
	if (unlock_o)
		unlock_o();
	if (set_auto)
		set_auto((int)n);
	sm2_log("orient: C %ld (lock mgr %p, was locked %d)", n, lm, locked);
}

/* Method D (backboardd only): BKOrientationManager holds the system orientation and is fed by CoreMotion, which never
 * reports here. Set it directly ("D N [SOURCE]") and stop CoreMotion updates from resetting it. */
static void sm2_cm_drop(id self, SEL _cmd, id o) {}

static void sm2_orient_d(long n, long src)
{
	Class k = objc_getClass("BKOrientationManager");
	SEL up = sel_registerName("updateDeviceOrientation:changeSource:"), sh = sel_registerName("sharedInstance");
	if (!k || ![k respondsToSelector:sh]) {
		sm2_log("orient: D BKOrientationManager missing");
		return;
	}
	id m = ((id (*)(id, SEL))objc_msgSend)(k, sh);
	static bool dropped;
	Method cm = class_getInstanceMethod(k, sel_registerName("_queue_updateDeviceOrientationFromCoreMotion:"));
	if (cm && !dropped) {
		dropped = true;
		method_setImplementation(cm, (IMP)sm2_cm_drop);
	}
	if (![m respondsToSelector:up]) {
		sm2_log("orient: D -updateDeviceOrientation:changeSource: missing");
		return;
	}
	((void (*)(id, SEL, long, long))objc_msgSend)(m, up, n, src);
	SEL ef = sel_registerName("effectiveDeviceOrientation");
	long e = [m respondsToSelector:ef] ? ((long (*)(id, SEL))objc_msgSend)(m, ef) : -1;
	sm2_log("orient: D %ld source %ld -> effective %ld (CoreMotion hook %d)", n, src, e, cm != NULL);
}

__attribute__((constructor)) static void sm2_orient_init(void)
{
	bool bbd = strcmp(getprogname(), "backboardd") == 0;
	if ((!bbd && strcmp(getprogname(), "SpringBoard") != 0) || access("/var/local/sweet-orient", F_OK) != 0)
		return;
	if (!bbd && !dlopen("/System/Library/PrivateFrameworks/AccessibilityUtilities.framework/AccessibilityUtilities", RTLD_NOW))
		sm2_log("orient: AccessibilityUtilities: %s", dlerror());
	/* static: a local source was freed under ARC when the constructor returned (k90a: no orient lines at all). */
	static dispatch_source_t t;
	t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
	sm2_log("orient: watching /var/local/sweet-orient");
	dispatch_source_set_event_handler(t, ^{
		static struct timespec last;
		struct stat st;
		if (stat("/var/local/sweet-orient", &st) != 0 || (st.st_mtimespec.tv_sec == last.tv_sec &&
		    st.st_mtimespec.tv_nsec == last.tv_nsec))
			return;
		last = st.st_mtimespec;
		char buf[16] = {0};
		int fd = open("/var/local/sweet-orient", O_RDONLY);
		if (fd < 0 || read(fd, buf, sizeof(buf) - 1) < 0)
			buf[0] = 0;
		if (fd >= 0)
			close(fd);
		bool b = buf[0] == 'B', c = buf[0] == 'C', d = buf[0] == 'D';
		char *end = NULL;
		long n = strtol(b || c || d ? buf + 1 : buf, &end, 10);
		if (n < 1 || n > 4 || d != bbd)	/* "D" lines are backboardd's, the rest SpringBoard's */
			return;
		if (d) {
			sm2_orient_d(n, strtol(end, NULL, 10));
			return;
		}
		@try {
			c ? sm2_orient_c(n) : b ? sm2_orient_b(n) : sm2_orient_a(n);
		} @catch (id ex) {
			sm2_log("orient: exception %s", [[ex description] UTF8String]);
		}
	});
	dispatch_resume(t);
}
