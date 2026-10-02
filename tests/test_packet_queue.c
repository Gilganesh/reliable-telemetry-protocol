#include <stdio.h>
#include <string.h>
#include "protocol.h"
#include "../node_esp32/packet_queue.h"

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
    CHECK(pq_empty(&q) && pq_peek(&q) == NULL, "нова черга порожня, peek == NULL");

    for (uint32_t i = 1; i <= 3; i++) { SensorPacket p = mk(i); pq_push(&q, &p); }
    CHECK(q.count == 3, "після 3 push у черзі 3 елементи");
    CHECK(pq_peek(&q)->sequence == 1, "peek дає найстаріший, не видаляючи");

    SensorPacket out;
    int ok = 1;
    for (uint32_t i = 1; i <= 3; i++) ok = ok && pq_pop(&q, &out) && out.sequence == i;
    CHECK(ok, "pop повертає елементи в порядку додавання");
    CHECK(!pq_pop(&q, &out) && !pq_pop(&q, NULL), "pop порожньої черги -> false");
}

static void test_overflow_drops_oldest(void) {
    printf("test_overflow_drops_oldest:\n");
    SensorPacket st[CAP];
    PacketQueue q;
    pq_init(&q, st, CAP);
    bool any_overflow = false;
    for (uint32_t i = 1; i <= CAP; i++) { SensorPacket p = mk(i); any_overflow |= pq_push(&q, &p); }
    CHECK(!any_overflow && q.dropped == 0, "до заповнення нічого не відкидається");

    SensorPacket p = mk(100);
    CHECK(pq_push(&q, &p), "push у повну чергу повідомляє про переповнення");
    CHECK(q.count == CAP && q.dropped == 1, "ємність не перевищена, dropped = 1");

    SensorPacket out;
    pq_pop(&q, &out);
    CHECK(out.sequence == 2, "відкинуто найстаріший (1), першим тепер 2");
    int last_ok = 1;
    uint32_t expect[] = {3, 4, 100};
    for (int i = 0; i < 3; i++) last_ok = last_ok && pq_pop(&q, &out) && out.sequence == expect[i];
    CHECK(last_ok, "решта в порядку, новий пакет в кінці");
}

static void test_wraparound(void) {
    printf("test_wraparound:\n");
    SensorPacket st[CAP];
    PacketQueue q;
    pq_init(&q, st, CAP);
    SensorPacket out;
    int ok = 1;
    uint32_t next_in = 0, next_out = 0;
    /* багато циклів push/pop, щоб head кілька разів обійшов масив */
    for (int round = 0; round < 50; round++) {
        for (int k = 0; k < 3; k++) { SensorPacket p = mk(next_in++); pq_push(&q, &p); }
        for (int k = 0; k < 3; k++) ok = ok && pq_pop(&q, &out) && out.sequence == next_out++;
    }
    CHECK(ok && pq_empty(&q) && q.dropped == 0, "порядок зберігається після багатьох обертань кільця");
}

static void test_push_front(void) {
    printf("test_push_front:\n");
    SensorPacket st[CAP];
    PacketQueue q;
    pq_init(&q, st, CAP);
    SensorPacket a = mk(10), b = mk(11), c = mk(5);
    pq_push(&q, &a);
    pq_push(&q, &b);
    CHECK(!pq_push_front(&q, &c), "push_front у неповну чергу без втрат");
    SensorPacket out;
    pq_pop(&q, &out);
    CHECK(out.sequence == 5, "push_front ставить пакет на початок");
    pq_pop(&q, &out);
    CHECK(out.sequence == 10, "далі йде те, що було першим");

    /* повна черга: push_front відкидає НАЙНОВІШИЙ */
    pq_init(&q, st, CAP);
    for (uint32_t i = 1; i <= CAP; i++) { SensorPacket p = mk(i); pq_push(&q, &p); }
    SensorPacket f = mk(0);
    CHECK(pq_push_front(&q, &f), "push_front у повну чергу повідомляє про переповнення");
    int ok = 1;
    uint32_t expect[] = {0, 1, 2, 3};
    for (int i = 0; i < CAP; i++) ok = ok && pq_pop(&q, &out) && out.sequence == expect[i];
    CHECK(ok && q.dropped == 1, "відкинуто найновіший (4), порядок 0,1,2,3");
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
          "timestamp і payload (час створення пакета) не змінюються в черзі");
}

int main(void) {
    test_fifo_order();
    test_overflow_drops_oldest();
    test_wraparound();
    test_push_front();
    test_payload_preserved();
    printf("\n----------------------------------------\n");
    printf("Тестів: %d, провалено: %d\n", tests_run, tests_failed);
    if (tests_failed == 0) printf("УСІ ТЕСТИ ПРОЙШЛИ\n");
    return tests_failed == 0 ? 0 : 1;
}
