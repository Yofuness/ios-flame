/*
 * camera_client: /dev/redwood-camera's open/read/write/close (redwood_camera.h) over camd's socket and shared slots
 * (sunfish_camera.h), for camera_device.c inside mediaserverd (camera stage f2, research/notes/camera-stage-f.md).
 *
 *   camera_client_open(s)  connect, SENSOR s (if not 0), START (waits for camd's reply: frames flowing), map the slots read-only: a
 *                          handle (the socket), or -1 with errno. One handle at a time (EBUSY).
 *   camera_client_read()   waits at most 1 s for a frame newer than the last one read (EAGAIN): its header, then as
 *                          many of its bytes as fit; a buffer of the header's size learns the size. EIO: camd faulted
 *                          or went away.
 *   camera_client_write()  a struct redwood_camera_command to camd (its reply is drained, logged if not 0).
 *   camera_client_close()  STOP, unmap, close.
 *   camera_client_enabled() the gate, checked once: /var/local/camdev.on exists, or boot-arg sweetcamdev=1.
 *   camera_client_log()    a line to os_log and /var/local/camdev.log (bounded), "sunfish-cam: " in front.
 */
#ifndef CAMERA_CLIENT_H
#define CAMERA_CLIENT_H
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define CAMERA_CLIENT_GATE	"/var/local/camdev.on"
#define CAMERA_CLIENT_LOG	"/var/local/camdev.log"

int camera_client_open(unsigned sensor);
ssize_t camera_client_read(int handle, void *buffer, size_t bytes);
ssize_t camera_client_write(int handle, const void *buffer, size_t bytes);
int camera_client_close(int handle);
bool camera_client_enabled(void);
void camera_client_log(const char *line);

#endif
