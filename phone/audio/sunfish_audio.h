/*
 * The sound iOS plays, on its way to whatever can make it heard: the FIFO /var/run/sunfish-audio (sp23), read by
 * adspboot --audio-fifo.
 *
 * From ios-redfin's redwood_audio.h, which is HackiOS's (MIT, Copyright (c) 2026 OOSEX GANG,
 * LICENSE.HackiOS); renamed, and only the user-space part kept. With no reader on the FIFO,
 * SunfishAudio.driver counts what it is given as refused and keeps running.
 *
 *   write(): whole frames in the format below. Never blocks: the writer is the audio server's
 *            real-time thread, which paces itself (48000 frames a second while anything
 *            plays, nothing at all otherwise). What the sink cannot take is dropped.
 */
#ifndef _SYS_SUNFISH_AUDIO_H_
#define _SYS_SUNFISH_AUDIO_H_

#include <stdint.h>

#define SUNFISH_AUDIO_FIFO      "/var/run/sunfish-audio"

/* The one format: 16-bit signed little-endian samples, left then right. */
#define SUNFISH_AUDIO_RATE      48000
#define SUNFISH_AUDIO_CHANNELS  2
#define SUNFISH_AUDIO_FRAME     4       /* bytes */

#endif /* _SYS_SUNFISH_AUDIO_H_ */
