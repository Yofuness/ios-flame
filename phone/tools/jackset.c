/*
 * jackset 0|1|3: set the headphone jack state that SunfishAudio.driver listens to (headphone jack S4):
 * the state of the Darwin notification org.iosonpixel.jack (bit 0 plugged, bit 1 microphone), then a post.
 * For testing the iOS side without the codec; the MBHC poller will do the same.
 */
#include <notify.h>
#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char **argv)
{
	uint64_t state;
	int token;

	if (argc != 2 || (state = strtoull(argv[1], NULL, 0)) > 3) {
		fprintf(stderr, "usage: jackset 0|1|3 (bit 0 plugged, bit 1 microphone)\n");
		return 2;
	}
	if (notify_register_check("org.iosonpixel.jack", &token) != NOTIFY_STATUS_OK ||
	    notify_set_state(token, state) != NOTIFY_STATUS_OK || notify_post("org.iosonpixel.jack") != NOTIFY_STATUS_OK) {
		fprintf(stderr, "jackset: notify failed\n");
		return 1;
	}
	return 0;
}
