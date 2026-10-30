/*
 * /dev/sunfish-audio: the sound iOS plays, on its way to whatever can make it heard.
 *
 * From ios-redfin's redwood_audio.h, which is HackiOS's (MIT, Copyright (c) 2026 OOSEX GANG,
 * LICENSE.HackiOS); renamed, and only the user-space part kept. The node does not exist on sunfish
 * yet: SunfishAudio.driver then counts what it is given as refused and keeps running.
 *
 *   write(): whole frames in the format below. Never blocks: the writer is the audio server's
 *            real-time thread, which paces itself (48000 frames a second while anything
 *            plays, nothing at all otherwise). What the sink cannot take is dropped.
 */
#ifndef _SYS_SUNFISH_AUDIO_H_
#define _SYS_SUNFISH_AUDIO_H_

#include <stdint.h>
#include <sys/ioccom.h>

#define SUNFISH_AUDIO_DEVICE    "/dev/sunfish-audio"

/* The one format: 16-bit signed little-endian samples, left then right. */
#define SUNFISH_AUDIO_RATE      48000
#define SUNFISH_AUDIO_CHANNELS  2
#define SUNFISH_AUDIO_FRAME     4       /* bytes */

/*
 * ioctl(SUNFISH_AUDIO_SET_VOLUME): the speakers' volume, as iOS sets it on the plug-in's volume
 * control -- how many eighths of a dB under the loudest they may play, and whether they are
 * muted (iOS's volume at 0). Same ABI as redfin's REDWOOD_AUDIO_SET_VOLUME.
 */
struct sunfish_audio_volume {
	uint32_t below;                 /* eighths of a dB */
	uint32_t muted;
};
#define SUNFISH_AUDIO_SET_VOLUME _IOW('r', 1, struct sunfish_audio_volume)

#endif /* _SYS_SUNFISH_AUDIO_H_ */
