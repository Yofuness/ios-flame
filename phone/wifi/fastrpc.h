/*
 * FastRPC client + reverse-RPC file listener for the ADSP sensors PD over glink "fastrpcglink-apps-dsp"
 * (sunfish downstream adsprpc.c wire format, quic/fastrpc adsp_listener + apps_std). Polled: one invoke in
 * flight; the caller feeds every complete packet of that channel to fastrpc_rx.
 */
#ifndef FASTRPC_H
#define FASTRPC_H
#include <stddef.h>

typedef void (*fastrpc_send_fn)(const void *pkt, size_t n);

/* Allocate the DMA buffer and send the sensors-PD attach; 0 = started. */
int fastrpc_start(fastrpc_send_fn send);
/* One received message (smq_invoke_rsp). */
void fastrpc_rx(const void *p, size_t n);
/* Calls per method, files opened, errors. */
void fastrpc_summary(void);

#endif
