#ifndef PACKET_QUEUE_H
#define PACKET_QUEUE_H

#include <stdbool.h>
#include <stdint.h>
#include "protocol.h"

typedef struct {
    SensorPacket *items;
    int capacity;
    int head;
    int count;
    uint32_t dropped;
} PacketQueue;

static inline void pq_init(PacketQueue *q, SensorPacket *storage, int capacity) {
    q->items = storage;
    q->capacity = capacity;
    q->head = 0;
    q->count = 0;
    q->dropped = 0;
}

static inline bool pq_empty(const PacketQueue *q) { return q->count == 0; }

static inline bool pq_push(PacketQueue *q, const SensorPacket *pkt) {
    bool overflow = false;
    if (q->count == q->capacity) {
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        q->dropped++;
        overflow = true;
    }
    q->items[(q->head + q->count) % q->capacity] = *pkt;
    q->count++;
    return overflow;
}

static inline bool pq_push_front(PacketQueue *q, const SensorPacket *pkt) {
    bool overflow = false;
    if (q->count == q->capacity) {
        q->count--;
        q->dropped++;
        overflow = true;
    }
    q->head = (q->head + q->capacity - 1) % q->capacity;
    q->items[q->head] = *pkt;
    q->count++;
    return overflow;
}

static inline SensorPacket *pq_peek(PacketQueue *q) {
    return q->count == 0 ? NULL : &q->items[q->head];
}

static inline bool pq_pop(PacketQueue *q, SensorPacket *out) {
    if (q->count == 0) return false;
    if (out) *out = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return true;
}

#endif
