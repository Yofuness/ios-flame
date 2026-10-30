/*
 * camd's interface (camera f1, research/notes/camera-stage-f.md): frames in a shared file, commands over a socket.
 *
 * /var/run/sunfish-camera.sock (unix stream, 0666): struct redwood_camera_command (redwood_camera.h) verbatim,
 *   plus `bytes` bytes that camd skips; one struct sunfish_camera_reply back per command. SUNFISH_CAMERA_START powers
 *   the camera up (once per connection; the reply comes when frames flow or bring-up failed), _STOP or closing the
 *   connection drops it; when no connection has it started, everything goes down. EXPOSURE a lines, b gain (256 =
 *   1x, linear; 0 = keep); c, d ignored (frame length stays the mode's). SENSOR a 1 rear (default),
 *   2 front (IMX355 1640x924, stride 2064; a running stream restarts on it; data_bytes and the frame header follow),
 *   others -EINVAL. FOCUS, FLASH, KEEP accepted, ignored.
 * /var/run/sunfish-camera.shm (0666, mmap read-only): struct sunfish_camera_shm, then SUNFISH_CAMERA_SLOTS slots of
 *   slot_bytes at shm + head_bytes: struct sunfish_camera_slot (gen + the frame header), the RAW10 bytes at
 *   slot + data_offset (2016x1512, stride 2528: cam34, the WM pads RAW10 lines to 16 B; data_bytes = height * stride).
 * A new frame: latest_seq goes up (written after the slot). Wait by polling latest_seq (1 ms steps; camd has no
 * other notification), copy slot latest_slot, then check its gen is even and unchanged (else camd rewrote it
 * while you copied: take the newer one).
 */
#ifndef SUNFISH_CAMERA_H
#define SUNFISH_CAMERA_H
#include <stdint.h>
#include "redwood_camera.h"

#define SUNFISH_CAMERA_SOCK	"/var/run/sunfish-camera.sock"
#define SUNFISH_CAMERA_SHM	"/var/run/sunfish-camera.shm"
#define SUNFISH_CAMERA_SHM_MAGIC "SFCAMSHM"
#define SUNFISH_CAMERA_SLOTS	3
#define SUNFISH_CAMERA_HEAD	4096u
#define SUNFISH_CAMERA_DATA_OFF	4096u
#define SUNFISH_CAMERA_SLOT_BYTES (4u << 20)

enum {
	SUNFISH_CAMERA_START = 0x100,
	SUNFISH_CAMERA_STOP = 0x101,
};
enum { SUNFISH_CAMERA_IDLE = 0, SUNFISH_CAMERA_STARTING = 1, SUNFISH_CAMERA_STREAMING = 2, SUNFISH_CAMERA_FAULT = 3,
       SUNFISH_CAMERA_STOPPING = 4 };

struct sunfish_camera_reply {
	uint32_t command;
	int32_t status;		/* 0 or -errno (-EIO: the camera had a fault, it stays off until camd restarts) */
};

struct sunfish_camera_shm {
	char magic[8];		/* SUNFISH_CAMERA_SHM_MAGIC */
	uint32_t version;	/* 1 */
	uint32_t head_bytes;	/* where slot 0 starts */
	uint32_t slots, slot_bytes, data_offset, data_bytes;
	uint32_t state;		/* SUNFISH_CAMERA_* */
	uint32_t latest_slot;
	uint64_t latest_seq;	/* the newest frame's sequence, 0 none yet */
	uint64_t published, dropped;	/* frames copied out / done by the WM but not copied */
	uint32_t pid;		/* camd's */
	uint32_t copy_us;	/* the last slot copy's time */
};

struct sunfish_camera_slot {
	uint32_t gen;		/* odd while camd writes the slot */
	uint32_t pad;
	struct redwood_camera_frame frame;
};

#endif
