/*
 * APR over the glink "lpass" edge (apr_audio_svc), polled. The transport of aprtest.c as a module for apr_tone;
 * aprtest keeps its own copy. Every command is logged with its opcode, token and the result status.
 */
#ifndef APR_H
#define APR_H
#include <stdint.h>
#include <stddef.h>

#define APR_SVC_CORE	0x3
#define APR_SVC_AFE	0x4
#define APR_BASIC_RSP	0x000110e8	/* payload: opcode, status */

/* last reply (any opcode) */
extern uint32_t apr_rsp_op, apr_rsp_token, apr_rsp[64], apr_rsp_len;

/* Dry run: apr_open/apr_cmd only print the packets (hex) and pretend status 0. */
void apr_set_dry(int dry);
/* Reattach to the lpass edge (adspboot brought it up) and open apr_audio_svc; 0 = up. reset: as aprtest -r. */
int apr_open(int reset);
void apr_close(void);
/*
 * Send one command and wait up to ms for its answer: APR_BASIC_RSP for this opcode, or rsp_op (0 = only
 * BASIC_RSP). Returns the APR status (0 = ok), or -1 on a send failure or timeout.
 */
int apr_cmd(uint8_t svc, uint32_t opcode, const void *payload, size_t plen, uint32_t rsp_op, int ms);
/* Send without waiting (stream writes); 0 = sent. */
int apr_send_nowait(uint8_t svc, uint32_t opcode, const void *payload, size_t plen);
/* Handle whatever the ADSP sent, for up to ms. */
void apr_pump(int ms);

#endif
