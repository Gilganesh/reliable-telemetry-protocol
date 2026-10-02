#include "reliability.h"
#include <string.h>

void reliable_init(ReliableCtx *ctx, reliable_send_fn send_fn, void *userdata) {
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->status = RELIABLE_IDLE;
    ctx->send_fn = send_fn;
    ctx->userdata = userdata;
}

bool reliable_is_busy(const ReliableCtx *ctx) {
    return ctx != NULL && ctx->status == RELIABLE_WAITING;
}

bool reliable_send_critical(ReliableCtx *ctx, const SensorPacket *pkt, uint64_t now_ms) {
    if (ctx == NULL || pkt == NULL || ctx->send_fn == NULL) {
        return false;
    }
    if (pkt->msg_type != MSG_ALARM && pkt->msg_type != MSG_CONFIG) {
        return false;
    }
    if (ctx->status == RELIABLE_WAITING) {
        return false;
    }

    int len = protocol_pack(pkt, ctx->tx_buf, sizeof(ctx->tx_buf));
    if (len <= 0) {
        return false;
    }

    ctx->tx_len = len;
    ctx->sequence = pkt->sequence;
    ctx->attempts = 1;
    ctx->deadline_ms = now_ms + ACK_TIMEOUT_MS;
    ctx->status = RELIABLE_WAITING;
    ctx->pending_success = false;

    ctx->send_fn(ctx->userdata, ctx->tx_buf, ctx->tx_len);
    return true;
}

void reliable_on_ack_received(ReliableCtx *ctx, uint32_t ack_sequence) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->status == RELIABLE_WAITING && ctx->sequence == ack_sequence) {
        ctx->status = RELIABLE_IDLE;
        ctx->pending_success = true;
    }
}

ReliableStatus reliable_tick(ReliableCtx *ctx, uint64_t now_ms) {
    if (ctx == NULL) {
        return RELIABLE_IDLE;
    }

    if (ctx->pending_success) {
        ctx->pending_success = false;
        return RELIABLE_SUCCESS;
    }

    if (ctx->status != RELIABLE_WAITING) {
        return RELIABLE_IDLE;
    }

    if (now_ms < ctx->deadline_ms) {
        return RELIABLE_WAITING;
    }

    if (ctx->attempts >= MAX_RETRIES + 1) {
        ctx->status = RELIABLE_IDLE;
        return RELIABLE_EXHAUSTED;
    }

    ctx->attempts++;
    ctx->deadline_ms = now_ms + ACK_TIMEOUT_MS;
    ctx->send_fn(ctx->userdata, ctx->tx_buf, ctx->tx_len);
    return RELIABLE_WAITING;
}
