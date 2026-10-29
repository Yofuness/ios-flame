/*
 * sns1: adspboot's SNS client (QMI service 400, SEE) on top of ios-redfin's sensor.c, driven from adspboot's
 * edge loop. Modes (need --rpc --root --sensors):
 *   --sns-list                          SUID + attributes of accel gyro mag pressure ambient_light proximity
 *   --sns-stream TYPE [--sns-rate HZ] [--sns-secs N]   stream one sensor, a line per 0.5 s
 *   --sns-motion [--sns-motion-dev PATH] [--sns-secs N]  every sensor into the kernel's motion device, at the
 *                                       rates iOS asks for (MOTION_GET_WANT), as redfin's sensd --daemon
 *   --sns-one-port (sns2 off: all streams on client port 0x40), --sns-unregistered (sns2 off: also stream sensors
 *   whose kernel service is not registered)
 * Common: --sns-delay S (wait after svc 400 appears, default 10), --sns-dump (every QMI message in hex).
 */
#ifndef SNS_RUN_H
#define SNS_RUN_H
#include <stdint.h>
#include "sns.h"
#include "sns_link.h"

/* argv[*i] is one of ours: consume it (and its value), return 1; else 0; -1 on a missing value. */
int sns_opt(int argc, char **argv, int *i);
int sns_wanted(void);
/* adspboot: the ADSP announced SNS (NEW_SERVER svc 400) at node:port. */
void sns_service(uint32_t node, uint32_t port);
/* adspboot: called from its edge loop (never from inside a packet handler); ops is installed on the first call. */
void sns_tick(const struct sns_link_ops *ops);
void sns_summary(void);

#endif
