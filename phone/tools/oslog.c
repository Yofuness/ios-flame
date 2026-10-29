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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum {
	OS_ACTIVITY_STREAM_PROCESS_ONLY = 0x001,
	OS_ACTIVITY_STREAM_DEBUG = 0x020,
	OS_ACTIVITY_STREAM_INFO = 0x100,
};
enum {
	OS_ACTIVITY_STREAM_TYPE_LOG_MESSAGE = 0x0400,
	OS_ACTIVITY_STREAM_TYPE_LEGACY_LOG_MESSAGE = 0x0480,
};
enum { OS_ACTIVITY_STREAM_EVENT_STARTED = 1, OS_ACTIVITY_STREAM_EVENT_STOPPED, OS_ACTIVITY_STREAM_EVENT_FAILED };
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

int main(int argc, char **argv)
{
	pid_t want_pid = -1;
	const char *want_name = NULL, *grep = NULL;
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
	setvbuf(stdout, NULL, _IOLBF, 0);

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

	os_activity_stream_flag_t flags = levels ? OS_ACTIVITY_STREAM_INFO | OS_ACTIVITY_STREAM_DEBUG : 0;
	if (want_pid > 0)
		flags |= OS_ACTIVITY_STREAM_PROCESS_ONLY;

	os_activity_stream_t s = stream_for_pid(want_pid, flags, ^bool(os_activity_stream_entry_t e, int err) {
		if (err || !e)
			return true;
		if (e->type != OS_ACTIVITY_STREAM_TYPE_LOG_MESSAGE && e->type != OS_ACTIVITY_STREAM_TYPE_LEGACY_LOG_MESSAGE)
			return true;
		if (want_pid > 0 && e->pid != want_pid)
			return true;
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
	});
	if (!s) {
		fprintf(stderr, "oslog: os_activity_stream_for_pid failed (entitlements?)\n");
		return 1;
	}
	stream_set_event_handler(s, ^(os_activity_stream_t st, os_activity_stream_event_t ev) {
		if (ev == OS_ACTIVITY_STREAM_EVENT_FAILED || ev == OS_ACTIVITY_STREAM_EVENT_STOPPED) {
			fprintf(stderr, "oslog: stream %s\n", ev == OS_ACTIVITY_STREAM_EVENT_FAILED ? "failed" : "stopped");
			exit(ev == OS_ACTIVITY_STREAM_EVENT_FAILED);
		}
	});
	stream_resume(s);
	if (secs > 0)
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)secs * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			stream_cancel(s);
			exit(0);
		});
	dispatch_main();
}
