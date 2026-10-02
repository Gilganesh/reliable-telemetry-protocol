#include <stdio.h>
#include <string.h>
#include "protocol.h"
#include "reliability.h"

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

#define MAX_RECORDED_SENDS 16

static uint8_t sent_bufs[MAX_RECORDED_SENDS][RELIABLE_MAX_PACKET];
static int     sent_lens[MAX_RECORDED_SENDS];
static int     sent_count;

static void reset_fake_transport(void) {
    sent_count = 0;
    memset(sent_bufs, 0, sizeof(sent_bufs));
    memset(sent_lens, 0, sizeof(sent_lens));
}

static void fake_send(void *userdata, const uint8_t *buf, int len) {
    (void)userdata;
    if (sent_count < MAX_RECORDED_SENDS) {
        memcpy(sent_bufs[sent_count], buf, (size_t)len);
        sent_lens[sent_count] = len;
        sent_count++;
    }
}

static SensorPacket make_alarm(uint32_t sequence) {
    SensorPacket pkt = {0};
    pkt.version = PROTOCOL_VERSION;
    pkt.msg_type = MSG_ALARM;
    pkt.node_id = 1;
    pkt.sequence = sequence;
    pkt.timestamp_ms = 1000;
    const char *json = "{\"alarm\":\"gas\"}";
    pkt.payload_len = (uint16_t)strlen(json);
    memcpy(pkt.payload, json, pkt.payload_len);
    return pkt;
}

static void test_success_on_first_ack(void) {
    printf("test_success_on_first_ack:\n");
    reset_fake_transport();
    ReliableCtx ctx;
    reliable_init(&ctx, fake_send, NULL);

    SensorPacket pkt = make_alarm(10);
    bool started = reliable_send_critical(&ctx, &pkt, 0);
    CHECK(started, "reliable_send_critical starts for MSG_ALARM");
    CHECK(sent_count == 1, "first attempt is sent immediately");
    CHECK(reliable_is_busy(&ctx), "context is busy after start");

    reliable_on_ack_received(&ctx, 10);
    ReliableStatus st = reliable_tick(&ctx, 100);
    CHECK(st == RELIABLE_SUCCESS, "tick() returns SUCCESS right after ACK");
    CHECK(!reliable_is_busy(&ctx), "context is free after SUCCESS");

    ReliableStatus st2 = reliable_tick(&ctx, 200);
    CHECK(st2 == RELIABLE_IDLE, "SUCCESS is reported once, then IDLE");
    CHECK(sent_count == 1, "no retransmission was needed");
}

static void test_retry_then_success(void) {
    printf("test_retry_then_success:\n");
    reset_fake_transport();
    ReliableCtx ctx;
    reliable_init(&ctx, fake_send, NULL);

    SensorPacket pkt = make_alarm(20);
    reliable_send_critical(&ctx, &pkt, 0);
    CHECK(sent_count == 1, "first attempt is sent immediately");

    ReliableStatus st = reliable_tick(&ctx, ACK_TIMEOUT_MS);
    CHECK(st == RELIABLE_WAITING, "status stays WAITING after a retry");
    CHECK(sent_count == 2, "deadline passed -> second attempt sent");
    CHECK(ctx.attempts == 2, "attempt counter is 2");

    reliable_on_ack_received(&ctx, 20);
    st = reliable_tick(&ctx, ACK_TIMEOUT_MS + 50);
    CHECK(st == RELIABLE_SUCCESS, "ACK after the second attempt confirms delivery");
}

static void test_exhausted_after_max_retries(void) {
    printf("test_exhausted_after_max_retries:\n");
    reset_fake_transport();
    ReliableCtx ctx;
    reliable_init(&ctx, fake_send, NULL);

    SensorPacket pkt = make_alarm(30);
    reliable_send_critical(&ctx, &pkt, 0);

    uint64_t now = 0;
    ReliableStatus st = RELIABLE_WAITING;
    for (int i = 0; i < MAX_RETRIES; i++) {
        now += ACK_TIMEOUT_MS;
        st = reliable_tick(&ctx, now);
        CHECK(st == RELIABLE_WAITING, "intermediate deadlines -> still WAITING");
    }
    CHECK(sent_count == MAX_RETRIES + 1,
          "exactly MAX_RETRIES+1 copies were sent");

    now += ACK_TIMEOUT_MS;
    st = reliable_tick(&ctx, now);
    CHECK(st == RELIABLE_EXHAUSTED, "last deadline -> EXHAUSTED");
    CHECK(!reliable_is_busy(&ctx), "context is free after EXHAUSTED");

    st = reliable_tick(&ctx, now + 10);
    CHECK(st == RELIABLE_IDLE, "EXHAUSTED is reported once, then IDLE");
    CHECK(sent_count == MAX_RETRIES + 1,
          "no sends after EXHAUSTED");
}

static void test_same_bytes_resent(void) {
    printf("test_same_bytes_resent:\n");
    reset_fake_transport();
    ReliableCtx ctx;
    reliable_init(&ctx, fake_send, NULL);

    SensorPacket pkt = make_alarm(40);
    reliable_send_critical(&ctx, &pkt, 0);
    reliable_tick(&ctx, ACK_TIMEOUT_MS);
    reliable_tick(&ctx, 2 * ACK_TIMEOUT_MS);

    CHECK(sent_count == 3, "3 copies sent (1 original + 2 retries)");
    CHECK(sent_lens[0] == sent_lens[1] && sent_lens[1] == sent_lens[2],
          "packet length is identical across attempts");
    CHECK(memcmp(sent_bufs[0], sent_bufs[1], (size_t)sent_lens[0]) == 0,
          "attempts 1 and 2 are byte-identical (same sequence)");
    CHECK(memcmp(sent_bufs[1], sent_bufs[2], (size_t)sent_lens[1]) == 0,
          "attempts 2 and 3 are byte-identical");
}

static void test_busy_blocks_new_send(void) {
    printf("test_busy_blocks_new_send:\n");
    reset_fake_transport();
    ReliableCtx ctx;
    reliable_init(&ctx, fake_send, NULL);

    SensorPacket first = make_alarm(50);
    SensorPacket second = make_alarm(51);
    CHECK(reliable_send_critical(&ctx, &first, 0),
          "first critical send starts");
    CHECK(!reliable_send_critical(&ctx, &second, 10),
          "second send is rejected while the first is in flight");
    CHECK(sent_count == 1, "rejected send does not transmit");
}

static void test_ack_wrong_sequence_ignored(void) {
    printf("test_ack_wrong_sequence_ignored:\n");
    reset_fake_transport();
    ReliableCtx ctx;
    reliable_init(&ctx, fake_send, NULL);

    SensorPacket pkt = make_alarm(60);
    reliable_send_critical(&ctx, &pkt, 0);

    reliable_on_ack_received(&ctx, 9999);
    ReliableStatus st = reliable_tick(&ctx, 100);
    CHECK(st == RELIABLE_WAITING,
          "ACK with a different sequence is ignored");
}

static void test_non_critical_rejected(void) {
    printf("test_non_critical_rejected:\n");
    reset_fake_transport();
    ReliableCtx ctx;
    reliable_init(&ctx, fake_send, NULL);

    SensorPacket pkt = make_alarm(70);
    pkt.msg_type = MSG_TELEMETRY;
    bool started = reliable_send_critical(&ctx, &pkt, 0);
    CHECK(!started, "MSG_TELEMETRY is rejected (ALARM/CONFIG only)");
    CHECK(sent_count == 0, "nothing is sent for a non-critical type");
}

int main(void) {
    test_success_on_first_ack();
    test_retry_then_success();
    test_exhausted_after_max_retries();
    test_same_bytes_resent();
    test_busy_blocks_new_send();
    test_ack_wrong_sequence_ignored();
    test_non_critical_rejected();

    printf("\n----------------------------------------\n");
    printf("Tests: %d, failed: %d\n", tests_run, tests_failed);
    if (tests_failed == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("SOME TESTS FAILED\n");
    return 1;
}
