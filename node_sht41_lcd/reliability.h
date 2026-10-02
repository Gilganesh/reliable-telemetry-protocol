#ifndef RELIABILITY_H
#define RELIABILITY_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

#define ACK_TIMEOUT_MS   2000
#define MAX_RETRIES      3

#define RELIABLE_MAX_PACKET (HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE)

typedef enum {
    RELIABLE_IDLE = 0,
    RELIABLE_WAITING,
    RELIABLE_SUCCESS,
    RELIABLE_EXHAUSTED,
} ReliableStatus;

typedef void (*reliable_send_fn)(void *userdata, const uint8_t *buf, int len);

typedef struct {
    ReliableStatus status;
    uint8_t  tx_buf[RELIABLE_MAX_PACKET];
    int      tx_len;
    uint32_t sequence;
    int      attempts;
    uint64_t deadline_ms;
    bool     pending_success;
    reliable_send_fn send_fn;
    void     *userdata;
} ReliableCtx;

void reliable_init(ReliableCtx *ctx, reliable_send_fn send_fn, void *userdata);

bool reliable_send_critical(ReliableCtx *ctx, const SensorPacket *pkt, uint64_t now_ms);

void reliable_on_ack_received(ReliableCtx *ctx, uint32_t ack_sequence);

ReliableStatus reliable_tick(ReliableCtx *ctx, uint64_t now_ms);

bool reliable_is_busy(const ReliableCtx *ctx);

#endif
