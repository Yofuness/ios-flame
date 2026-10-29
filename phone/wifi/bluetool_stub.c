/*
 * bluetool-stub: stand-in for /usr/sbin/BlueTool -R (launchd job com.apple.BlueTool, Mach service com.apple.BlueTool)
 * on the Pixel 4a. btbridge has already loaded the WCN3990 firmware, so every request succeeds without touching the
 * controller: reply {result: 0, firmware: "crbtfw21", manufacturer: 29 (Qualcomm)}; RunCommand also gets output "".
 * Each request's keys and values are logged to /private/var/tmp/bluetool-stub.log (the job runs as mobile).
 * From ios-redfin phone/bt/bluetool.c (MIT, research/re/redfin/; ideas only, code written here): the first request
 * (or script "boot") waits up to 20 s for btbridge's ready file /private/var/tmp/btbridge.pid, and a "btaddress"
 * in the request (top level or kMsgArgs) is echoed in the reply. Off: /var/local/bluetool-stub.nowait (no wait).
 *
 * Adapted from iOSPlus10 userland/bluetooth/negroni-bluetool.c (research/re/iosplus10/), which carries:
 *   Copyright (c) 2026 iOSPlus10 contributors
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 *   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 *   to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *   The above copyright notice and this permission notice shall be included in all copies or substantial portions
 *   of the Software.
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 *   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *   IN THE SOFTWARE.
 * Their facts [20H392 bluetoothd/BlueTool]: request kMsgId RunBuiltinScript / RunCommand / RunExternalScript with
 * kMsgArgs {script, command, btaddress}; reply int64 "result" (0 ok, 10/13 unknown chipset), string "firmware",
 * int64 "manufacturer"; RunCommand -> "output".
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "xpc_decl.h"

#define LOG_PATH "/private/var/tmp/bluetool-stub.log"
#define READY "/private/var/tmp/btbridge.pid"
#define NOWAIT "/var/local/bluetool-stub.nowait"

static unsigned requests;

/* btbridge has the chip and the pty (its pid file names a live process; root's, so EPERM = alive) */
static int bridge_ready(void)
{
	FILE *f = fopen(READY, "r");
	int pid = 0;
	if (f) {
		if (fscanf(f, "%d", &pid) != 1)
			pid = 0;
		fclose(f);
	}
	return pid > 0 && (!kill(pid, 0) || errno == EPERM);
}

static xpc_object_t arg(xpc_object_t msg, const char *key)
{
	xpc_object_t v = xpc_dictionary_get_value(msg, key), a = xpc_dictionary_get_value(msg, "kMsgArgs");
	if (!v && a && xpc_get_type(a) == XPC_TYPE_DICTIONARY)
		v = xpc_dictionary_get_value(a, key);
	return v;
}

static void dump(const char *indent, xpc_object_t d)
{
	xpc_dictionary_apply(d, ^bool(const char *key, xpc_object_t v) {
		char *s = xpc_copy_description(v);
		printf("%s%s = %s\n", indent, key, s ? s : "?");
		free(s);
		return true;
	});
}

static void handle(xpc_connection_t peer, xpc_object_t msg)
{
	if (xpc_get_type(msg) != XPC_TYPE_DICTIONARY) {
		char *s = xpc_copy_description(msg);
		printf("event: %s\n", s ? s : "?");
		free(s);
		return;
	}
	const char *id = xpc_dictionary_get_string(msg, "kMsgId");
	printf("request %s from pid %d:\n", id ? id : "(no kMsgId)", xpc_connection_get_pid(peer));
	dump("  ", msg);
	xpc_object_t script = arg(msg, "script");
	const char *sc = script && xpc_get_type(script) == XPC_TYPE_STRING ? xpc_string_get_string_ptr(script) : NULL;
	if ((++requests == 1 || (sc && !strcmp(sc, "boot"))) && access(NOWAIT, F_OK)) {
		int ms = 0;
		for (; !bridge_ready() && ms < 20000; ms += 250)
			usleep(250000);
		printf("  btbridge %s after %d ms\n", bridge_ready() ? "ready" : "NOT ready (answering anyway)", ms);
	}
	xpc_object_t reply = xpc_dictionary_create_reply(msg);
	if (!reply) {
		printf("  no reply port: ignored\n");
		return;
	}
	xpc_dictionary_set_int64(reply, "result", 0);
	xpc_dictionary_set_string(reply, "firmware", "crbtfw21");
	xpc_dictionary_set_int64(reply, "manufacturer", 29);
	xpc_object_t addr = arg(msg, "btaddress");
	if (addr)
		xpc_dictionary_set_value(reply, "btaddress", addr);
	if (id && !strcmp(id, "RunCommand"))
		xpc_dictionary_set_string(reply, "output", "");
	xpc_connection_send_message(peer, reply);
	xpc_release(reply);
	printf("  -> result 0, firmware crbtfw21, manufacturer 29\n");
}

int main(void)
{
	freopen(LOG_PATH, "a", stdout);
	setvbuf(stdout, NULL, _IOLBF, 0);
	xpc_connection_t l = xpc_connection_create_mach_service("com.apple.BlueTool", NULL,
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);
	if (!l) {
		printf("cannot listen on com.apple.BlueTool\n");
		return 1;
	}
	xpc_connection_set_event_handler(l, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION)
			return;
		xpc_connection_set_event_handler(peer, ^(xpc_object_t msg) { handle(peer, msg); });
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(l);
	printf("bluetool-stub: listening on com.apple.BlueTool\n");
	dispatch_main();
}
