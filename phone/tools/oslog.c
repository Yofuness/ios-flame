/*
 * oslog: stream the unified log (os_log) to stdout through LoggingSupport's ActivityStream SPI.
 *   oslog [-p PID | -n PROCNAME] [-t SECONDS] [-g SUBSTRING] [-l]
 *   -p only this pid; -n only processes whose executable name contains PROCNAME; -t exit after N s
 *   (default 10, 0 = forever); -g only lines containing SUBSTRING; -l also info and debug messages.
 * Output: HH:MM:SS.mmm proc[pid] <subsystem:category> text
 * Signed with oslog-ents.plist (com.apple.private.logging.*, com.apple.diagnosticd.diagnostic).
 * oslogls (oslogls.m) is the older OSLogEventLiveStore-based reader.
 *
 * SPI layout from limneos/oslog ActivityStreamAPI.h
 * (https://github.com/limneos/oslog/blob/master/ActivityStreamAPI.h), the same header LLDB's debugserver
 * carries as DarwinLog/ActivityStreamSPI.h. Only the parts used here are copied.
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <sys/ucontext.h>

enum {
	OS_ACTIVITY_STREAM_PROCESS_ONLY = 0x001,
	OS_ACTIVITY_STREAM_DEBUG = 0x020,
	OS_ACTIVITY_STREAM_INFO = 0x100,
};
enum {
	OS_ACTIVITY_STREAM_TYPE_LOG_MESSAGE = 0x0400,
	OS_ACTIVITY_STREAM_TYPE_LEGACY_LOG_MESSAGE = 0x0480,
};
enum {
	OS_ACTIVITY_STREAM_EVENT_STARTED = 1, OS_ACTIVITY_STREAM_EVENT_STOPPED, OS_ACTIVITY_STREAM_EVENT_FAILED,
	OS_ACTIVITY_STREAM_EVENT_CHUNK_STARTED, OS_ACTIVITY_STREAM_EVENT_CHUNK_FINISHED,
};
typedef uint32_t os_activity_stream_flag_t;
typedef uint32_t os_activity_stream_type_t;
typedef uint32_t os_activity_stream_event_t;
typedef uint64_t os_activity_id_t;
typedef struct os_activity_stream_s *os_activity_stream_t;

#define OS_ACTIVITY_STREAM_COMMON()	\
	uint64_t trace_id;		\
	uint64_t timestamp;		\
	uint64_t thread;		\
	const uint8_t *image_uuid;	\
	const char *image_path;		\
	struct timeval tv_gmt;		\
	struct timezone tz;		\
	uint32_t offset

typedef struct os_log_message_s {
	OS_ACTIVITY_STREAM_COMMON();
	const char *format;
	const uint8_t *buffer;
	size_t buffer_sz;
	const uint8_t *privdata;
	size_t privdata_sz;
	const char *subsystem;
	const char *category;
	uint32_t oversize_id;
	uint8_t ttl;
	bool persisted;
} *os_log_message_t;

typedef struct os_activity_stream_entry_s {
	os_activity_stream_type_t type;
	pid_t pid;
	uint64_t proc_id;
	const uint8_t *proc_imageuuid;
	const char *proc_imagepath;
	os_activity_id_t activity_id;
	os_activity_id_t parent_id;
	union {
		struct os_log_message_s log_message;
		char pad[4096 + 256];	/* the header's largest member is the statedump (PATH_MAX buffer) */
	};
} *os_activity_stream_entry_t;

typedef bool (^os_activity_stream_block_t)(os_activity_stream_entry_t entry, int error);
typedef void (^os_activity_stream_event_block_t)(os_activity_stream_t stream, os_activity_stream_event_t event);

static os_activity_stream_t (*stream_for_pid)(pid_t, os_activity_stream_flag_t, os_activity_stream_block_t);
static void (*stream_resume)(os_activity_stream_t);
static void (*stream_cancel)(os_activity_stream_t);
static void (*stream_set_event_handler)(os_activity_stream_t, os_activity_stream_event_block_t);
static char *(*copy_formatted_message)(os_log_message_t);

static void usage(void)
{
	fprintf(stderr, "usage: oslog [-p PID|-n PROCNAME] [-t SECONDS] [-g SUBSTRING] [-l]\n");
	exit(2);
}

static void *need(void *h, const char *sym)
{
	void *p = dlsym(h, sym);
	if (!p) {
		fprintf(stderr, "oslog: %s missing in LoggingSupport\n", sym);
		exit(1);
	}
	return p;
}

/* B31: print where a crash happened (async-signal-safe: snprintf on a stack buffer + write) */
static void on_segv(int sig, siginfo_t *si, void *uc)
{
	char b[96];
	unsigned long pc = 0;
#if defined(__arm64__)
	pc = (unsigned long)((ucontext_t *)uc)->uc_mcontext->__ss.__pc;
#else
	(void)uc;
#endif
	int n = snprintf(b, sizeof(b), "oslog: SIG%s at %p pc 0x%lx\n", sig == SIGBUS ? "BUS" : "SEGV", si->si_addr, pc);
	if (n > 0)
		write(2, b, (size_t)n);
	_exit(139);
}

static pid_t want_pid = -1;
static const char *want_name, *grep;
static os_activity_stream_flag_t flags;
static os_activity_stream_t cur;	/* touched on the main queue only */
static unsigned restarts;

static bool on_entry(os_activity_stream_entry_t e, int err)
{
	if (err) {
		printf("oslog: entry error %d\n", err);
		return true;
	}
	if (!e)
		return true;
	if (e->type != OS_ACTIVITY_STREAM_TYPE_LOG_MESSAGE && e->type != OS_ACTIVITY_STREAM_TYPE_LEGACY_LOG_MESSAGE)
		return true;
	if (want_pid > 0 && e->pid != want_pid)
		return true;
	/* -n matches the name on every entry, so a restarted process is followed */
	const char *path = e->proc_imagepath ? e->proc_imagepath : "?";
	const char *proc = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	if (want_name && !strstr(proc, want_name))
		return true;
	os_log_message_t m = &e->log_message;
	char *text = copy_formatted_message(m);
	const char *t = text ? text : (m->format ? m->format : "");
	if (grep && !strstr(t, grep)) {
		free(text);
		return true;
	}
	struct tm tm;
	time_t sec = m->tv_gmt.tv_sec;
	localtime_r(&sec, &tm);
	printf("%02d:%02d:%02d.%03d %s[%d] <%s:%s> %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
	       (int)(m->tv_gmt.tv_usec / 1000), proc, e->pid, m->subsystem ? m->subsystem : "",
	       m->category ? m->category : "", t);
	free(text);
	return true;
}

static void start_stream(void);

/* B31: a stopped/failed stream (logd/diagnosticd connection dropped, e.g. while processes are killed
 * and relaunched) used to _exit; now it is reconnected until -t expires */
static void restart_later(const char *why)
{
	printf("oslog: stream ended (%s), reconnecting\n", why);
	if (cur) {
		stream_cancel(cur);
		cur = NULL;
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(restarts < 5 ? 100 : 1000) * NSEC_PER_MSEC),
		       dispatch_get_main_queue(), ^{ start_stream(); });
}

static void start_stream(void)
{
	os_activity_stream_t s = stream_for_pid(want_pid, flags, ^bool(os_activity_stream_entry_t e, int err) {
		return on_entry(e, err);
	});
	restarts++;
	if (!s) {
		/* early after boot diagnosticd may not be up yet: retry instead of giving up */
		if (restarts == 1)
			fprintf(stderr, "oslog: os_activity_stream_for_pid failed (entitlements? diagnosticd not up?), retrying\n");
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)NSEC_PER_SEC), dispatch_get_main_queue(),
			       ^{ start_stream(); });
		return;
	}
	cur = s;
	stream_set_event_handler(s, ^(os_activity_stream_t st, os_activity_stream_event_t ev) {
		if (ev != OS_ACTIVITY_STREAM_EVENT_FAILED && ev != OS_ACTIVITY_STREAM_EVENT_STOPPED)
			return;
		const char *why = ev == OS_ACTIVITY_STREAM_EVENT_FAILED ? "failed" : "stopped";
		dispatch_async(dispatch_get_main_queue(), ^{
			if (st == cur)	/* ignore late events from an already replaced stream */
				restart_later(why);
		});
	});
	stream_resume(s);
}

int main(int argc, char **argv)
{
	int secs = 10, levels = 0, c;

	while ((c = getopt(argc, argv, "p:n:t:g:l")) != -1) {
		switch (c) {
		case 'p': want_pid = atoi(optarg); break;
		case 'n': want_name = optarg; break;
		case 't': secs = atoi(optarg); break;
		case 'g': grep = optarg; break;
		case 'l': levels = 1; break;
		default: usage();
		}
	}
	if (optind != argc || (want_pid > 0 && want_name))
		usage();
	setvbuf(stdout, NULL, _IOLBF, 0);	/* line-buffered: a crash loses at most the current line */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_segv;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);	/* a closed reader shows up as write errors, not a silent kill */

	void *h = dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_NOW);
	if (!h) {
		fprintf(stderr, "oslog: dlopen LoggingSupport: %s\n", dlerror());
		return 1;
	}
	stream_for_pid = need(h, "os_activity_stream_for_pid");
	stream_resume = need(h, "os_activity_stream_resume");
	stream_cancel = need(h, "os_activity_stream_cancel");
	stream_set_event_handler = need(h, "os_activity_stream_set_event_handler");
	copy_formatted_message = need(h, "os_log_copy_formatted_message");

	flags = levels ? OS_ACTIVITY_STREAM_INFO | OS_ACTIVITY_STREAM_DEBUG : 0;
	if (want_pid > 0)
		flags |= OS_ACTIVITY_STREAM_PROCESS_ONLY;
	dispatch_async(dispatch_get_main_queue(), ^{ start_stream(); });
	if (secs > 0)
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)secs * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			/* B31: exit() ran atexit/teardown while LoggingSupport threads still delivered entries
			 * (SIGSEGV far 0x10/0x20 at the timeout); flush and leave without teardown */
			if (cur)
				stream_cancel(cur);
			if (restarts > 1)
				fprintf(stderr, "oslog: %u stream starts\n", restarts);
			fflush(stdout);
			_exit(0);
		});
	dispatch_main();
}
