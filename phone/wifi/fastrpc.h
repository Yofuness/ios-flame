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
/* 1: attach to the root PD (pd 0, client id = tid) instead, print the reply + SMMU faults, stop. */
void fastrpc_root(int on);
/* Allocate the DMA buffer and send the sensors-PD attach; 0 = started. */
int fastrpc_start(fastrpc_send_fn send);
/* One received message (smq_invoke_rsp). */
void fastrpc_rx(const void *p, size_t n);
/* Calls per method, files opened, errors. */
void fastrpc_summary(void);

#endif
