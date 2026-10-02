#include <stdio.h>
#include <string.h>
#include "protocol.h"
#include "../node_common/packet_queue.h"

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        tests_failed++; \
        printf("  [FAIL] %s\n", msg); \
    } else { \
        printf("  [ok]   %s\n", msg); \
    } \
} while (0)

#define CAP 4

static SensorPacket mk(uint32_t seq) {
    SensorPacket p;
    memset(&p, 0, sizeof(p));
    p.version = PROTOCOL_VERSION;
    p.msg_type = MSG_TELEMETRY;
    p.sequence = seq;
    return p;
}

static void test_fifo_order(void) {
    printf("test_fifo_order:\n");
    SensorPacket st[CAP];
    PacketQueue q;
    pq_init(&q, st, CAP);
    CHECK(pq_empty(&q) && pq_peek(&q) == NULL, "new queue is empty, peek returns NULL");

    for (uint32_t i = 1; i <= 3; i++) { SensorPacket p = mk(i); pq_push(&q, &p); }
    CHECK(q.count == 3, "3 pushes -> count is 3");
    CHECK(pq_peek(&q)->sequence == 1, "peek returns the oldest without removing it");

    SensorPacket out;
    int ok = 1;
    for (uint32_t i = 1; i <= 3; i++) ok = ok && pq_pop(&q, &out) && out.sequence == i;
    CHECK(ok, "pop returns items in FIFO order");
    CHECK(!pq_pop(&q, &out) && !pq_pop(&q, NULL), "pop on an empty queue returns false");
}

static void test_overflow_drops_oldest(void) {
    printf("test_overflow_drops_oldest:\n");
    SensorPacket st[CAP];
    PacketQueue q;
    pq_init(&q, st, CAP);
    bool any_overflow = false;
    for (uint32_t i = 1; i <= CAP; i++) { SensorPacket p = mk(i); any_overflow |= pq_push(&q, &p); }
    CHECK(!any_overflow && q.dropped == 0, "nothing is dropped before the queue is full");

    SensorPacket p = mk(100);
    CHECK(pq_push(&q, &p), "push into a full queue reports overflow");
    CHECK(q.count == CAP && q.dropped == 1, "capacity is respected, dropped = 1");

    SensorPacket out;
    pq_pop(&q, &out);
    CHECK(out.sequence == 2, "oldest (1) was dropped, head is now 2");
    int last_ok = 1;
    uint32_t expect[] = {3, 4, 100};
    for (int i = 0; i < 3; i++) last_ok = last_ok && pq_pop(&q, &out) && out.sequence == expect[i];
    CHECK(last_ok, "remaining order intact, newest at the tail");
}

static void test_wraparound(void) {
    printf("test_wraparound:\n");
    SensorPacket st[CAP];
    PacketQueue q;
    pq_init(&q, st, CAP);
    SensorPacket out;
    int ok = 1;
    uint32_t next_in = 0, next_out = 0;
    for (int round = 0; round < 50; round++) {
        for (int k = 0; k < 3; k++) { SensorPacket p = mk(next_in++); pq_push(&q, &p); }
        for (int k = 0; k < 3; k++) ok = ok && pq_pop(&q, &out) && out.sequence == next_out++;
    }
    CHECK(ok && pq_empty(&q) && q.dropped == 0, "order is preserved across many wrap-arounds");
}

static void test_push_front(void) {
    printf("test_push_front:\n");
    SensorPacket st[CAP];
    PacketQueue q;
    pq_init(&q, st, CAP);
    SensorPacket a = mk(10), b = mk(11), c = mk(5);
    pq_push(&q, &a);
    pq_push(&q, &b);
    CHECK(!pq_push_front(&q, &c), "push_front into a non-full queue drops nothing");
    SensorPacket out;
    pq_pop(&q, &out);
    CHECK(out.sequence == 5, "push_front puts the packet at the head");
    pq_pop(&q, &out);
    CHECK(out.sequence == 10, "previous head follows");

    pq_init(&q, st, CAP);
    for (uint32_t i = 1; i <= CAP; i++) { SensorPacket p = mk(i); pq_push(&q, &p); }
    SensorPacket f = mk(0);
    CHECK(pq_push_front(&q, &f), "push_front into a full queue reports overflow");
    int ok = 1;
    uint32_t expect[] = {0, 1, 2, 3};
    for (int i = 0; i < CAP; i++) ok = ok && pq_pop(&q, &out) && out.sequence == expect[i];
    CHECK(ok && q.dropped == 1, "newest (4) was dropped, order is 0,1,2,3");
}

static void test_payload_preserved(void) {
    printf("test_payload_preserved:\n");
    SensorPacket st[CAP];
    PacketQueue q;
    pq_init(&q, st, CAP);
    SensorPacket p = mk(7);
    p.timestamp_ms = 123456789ULL;
    p.payload_len = 5;
    memcpy(p.payload, "hello", 5);
    pq_push(&q, &p);
    SensorPacket out;
    pq_pop(&q, &out);
    CHECK(out.timestamp_ms == 123456789ULL && out.payload_len == 5 && memcmp(out.payload, "hello", 5) == 0,
          "timestamp and payload are preserved in the queue");
}

int main(void) {
    test_fifo_order();
    test_overflow_drops_oldest();
    test_wraparound();
    test_push_front();
    test_payload_preserved();
    printf("\n----------------------------------------\n");
    printf("Tests: %d, failed: %d\n", tests_run, tests_failed);
    if (tests_failed == 0) printf("ALL TESTS PASSED\n");
    return tests_failed == 0 ? 0 : 1;
}
