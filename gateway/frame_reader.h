#ifndef FRAME_READER_H
#define FRAME_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "protocol.h"

#define FRAME_MAX_SIZE (HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE)

typedef struct {
    uint8_t  buf[FRAME_MAX_SIZE];
    size_t   have;
    size_t   need;
    bool     header_done;
    uint32_t bad_frames;
    uint32_t bad_reported;
} FrameReader;

static inline void frame_reader_init(FrameReader *fr) {
    fr->have = 0;
    fr->need = HEADER_SIZE;
    fr->header_done = false;
}

static inline bool frame_prefix_plausible(const FrameReader *fr) {
    if (fr->have >= 1 && fr->buf[0] != PROTOCOL_VERSION) return false;
    if (fr->have >= 2 && fr->buf[1] > MSG_ACK) return false;
    if (fr->have >= HEADER_SIZE) {
        uint16_t payload_len;
        memcpy(&payload_len, fr->buf + 16, 2);
        if (payload_len > MAX_PAYLOAD_SIZE) return false;
    }
    return true;
}

static inline void frame_reader_drop(FrameReader *fr, size_t n) {
    if (n > fr->have) n = fr->have;
    memmove(fr->buf, fr->buf + n, fr->have - n);
    fr->have -= n;
}

static inline bool frame_reader_scan(FrameReader *fr) {
    for (;;) {
        while (fr->have > 0 && !frame_prefix_plausible(fr)) frame_reader_drop(fr, 1);
        fr->header_done = fr->have >= HEADER_SIZE;
        if (!fr->header_done) {
            fr->need = HEADER_SIZE;
            return false;
        }
        uint16_t payload_len;
        memcpy(&payload_len, fr->buf + 16, 2);
        fr->need = HEADER_SIZE + payload_len + CRC_SIZE;
        if (fr->have < fr->need) return false;
        uint32_t rx_crc;
        memcpy(&rx_crc, fr->buf + fr->need - CRC_SIZE, CRC_SIZE);
        if (crc32(fr->buf, fr->need - CRC_SIZE) == rx_crc) return true;
        fr->bad_frames++;
        frame_reader_drop(fr, 1);
    }
}

static inline bool frame_reader_feed_byte(FrameReader *fr, uint8_t byte) {
    if (fr->have >= sizeof(fr->buf)) frame_reader_drop(fr, 1);
    fr->buf[fr->have++] = byte;
    return frame_reader_scan(fr);
}

static inline bool frame_reader_next(FrameReader *fr) {
    frame_reader_drop(fr, fr->need);
    return frame_reader_scan(fr);
}

#endif
