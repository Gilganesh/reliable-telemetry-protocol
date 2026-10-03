#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define PROTOCOL_VERSION   1
#define MAX_PAYLOAD_SIZE   128

#define HEADER_SIZE 18
#define CRC_SIZE    4

typedef enum {
    MSG_HEARTBEAT = 0,
    MSG_TELEMETRY = 1,
    MSG_ALARM     = 2,
    MSG_CONFIG    = 3,
    MSG_ACK       = 4,
} MessageType;

typedef struct {
    uint8_t  version;
    uint8_t  msg_type;
    uint16_t node_id;
    uint32_t sequence;
    uint64_t timestamp_ms;
    uint16_t payload_len;
    uint8_t  payload[MAX_PAYLOAD_SIZE];
} SensorPacket;

#define PROTO_OK                   0
#define PROTO_ERR_TOO_SHORT       -1
#define PROTO_ERR_BAD_CRC         -2
#define PROTO_ERR_UNKNOWN_TYPE    -3
#define PROTO_ERR_LEN_MISMATCH    -4
#define PROTO_ERR_PAYLOAD_TOO_BIG -5
#define PROTO_ERR_BUF_TOO_SMALL   -6
#define PROTO_ERR_BAD_VERSION     -7

uint32_t crc32(const uint8_t *data, size_t len);

int protocol_pack(const SensorPacket *pkt, uint8_t *out_buf, size_t out_buf_size);

int protocol_unpack(const uint8_t *raw, size_t raw_len, SensorPacket *out_pkt);

#endif
