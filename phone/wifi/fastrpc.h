/*
 * FastRPC client + reverse-RPC file listener for the ADSP sensors PD over glink "fastrpcglink-apps-dsp"
 * (sunfish downstream adsprpc.c wire format, quic/fastrpc adsp_listener + apps_std). Polled: one invoke in
 * flight; the caller feeds every complete packet of that channel to fastrpc_rx.
 */
#ifndef FASTRPC_H
#define FASTRPC_H
#include <stddef.h>
#include <stdint.h>

#define FASTRPC_DMA_SIZE	0x80000u

typedef void (*fastrpc_send_fn)(const void *pkt, size_t n);

/* Use this buffer (PA for the CPU, DSP its address on the DSP side) instead of a pm_alloc one at PA. */
void fastrpc_buffer(uint64_t pa, uint64_t dsp);
/* 1: attach to the root PD (pd 0, first half of the buffer) instead, print the reply + SMMU faults, then open
 * and serve its adsp_default_listener (audio step 6). */
void fastrpc_root(int on);
/* audio step 6, with fastrpc_root: once the root PD's listener serves, also attach the sensors PD (second half of
 * the buffer); without it only the root PD is served. */
void fastrpc_sensors(int on);
/* Allocate the DMA buffer and send the sensors-PD attach; 0 = started. */
int fastrpc_start(fastrpc_send_fn send);
/* One received message (smq_invoke_rsp). */
void fastrpc_rx(const void *p, size_t n);
/* au14: one heartbeat line part: per session state, calls served, the ctx in flight. */
void fastrpc_status(char *buf, size_t n);
/* Calls per method, files opened, errors. */
void fastrpc_summary(void);

#endif
