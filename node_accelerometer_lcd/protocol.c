#include "protocol.h"
#include <string.h>

uint32_t crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc = crc >> 1;
            }
        }
    }
    return ~crc;
}

int protocol_pack(const SensorPacket *pkt, uint8_t *out_buf, size_t out_buf_size) {
    if (pkt->payload_len > MAX_PAYLOAD_SIZE) {
        return PROTO_ERR_PAYLOAD_TOO_BIG;
    }

    size_t total_size = HEADER_SIZE + pkt->payload_len + CRC_SIZE;
    if (out_buf_size < total_size) {
        return PROTO_ERR_BUF_TOO_SMALL;
    }

    uint8_t *p = out_buf;

    memcpy(p, &pkt->version,      1); p += 1;
    memcpy(p, &pkt->msg_type,     1); p += 1;
    memcpy(p, &pkt->node_id,      2); p += 2;
    memcpy(p, &pkt->sequence,     4); p += 4;
    memcpy(p, &pkt->timestamp_ms, 8); p += 8;
    memcpy(p, &pkt->payload_len,  2); p += 2;

    memcpy(p, pkt->payload, pkt->payload_len);
    p += pkt->payload_len;

    uint32_t crc = crc32(out_buf, (size_t)(p - out_buf));
    memcpy(p, &crc, 4);
    p += 4;

    return (int)(p - out_buf);
}

int protocol_unpack(const uint8_t *raw, size_t raw_len, SensorPacket *out_pkt) {
    if (raw_len < HEADER_SIZE + CRC_SIZE) {
        return PROTO_ERR_TOO_SHORT;
    }

    size_t body_len = raw_len - CRC_SIZE;

    uint32_t received_crc;
    memcpy(&received_crc, raw + body_len, 4);

    uint32_t actual_crc = crc32(raw, body_len);
    if (actual_crc != received_crc) {
        return PROTO_ERR_BAD_CRC;
    }

    const uint8_t *p = raw;
    memcpy(&out_pkt->version,      p, 1); p += 1;
    memcpy(&out_pkt->msg_type,     p, 1); p += 1;
    memcpy(&out_pkt->node_id,      p, 2); p += 2;
    memcpy(&out_pkt->sequence,     p, 4); p += 4;
    memcpy(&out_pkt->timestamp_ms, p, 8); p += 8;
    memcpy(&out_pkt->payload_len,  p, 2); p += 2;

    if (out_pkt->version != PROTOCOL_VERSION) {
        return PROTO_ERR_BAD_VERSION;
    }

    if (out_pkt->msg_type > MSG_ACK) {
        return PROTO_ERR_UNKNOWN_TYPE;
    }

    size_t remaining = body_len - HEADER_SIZE;
    if (out_pkt->payload_len != remaining) {
        return PROTO_ERR_LEN_MISMATCH;
    }
    if (out_pkt->payload_len > MAX_PAYLOAD_SIZE) {
        return PROTO_ERR_PAYLOAD_TOO_BIG;
    }

    memcpy(out_pkt->payload, p, out_pkt->payload_len);

    return PROTO_OK;
}
