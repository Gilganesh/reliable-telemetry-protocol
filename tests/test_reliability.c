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

/* ---- Фейковий "передавач": замість реального MQTT просто записує
 * кожен відправлений пакет у масив, щоб тест міг перевірити скільки
 * разів і що саме було "відправлено". ---- */
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
    CHECK(started, "reliable_send_critical стартує успішно для MSG_ALARM");
    CHECK(sent_count == 1, "перша спроба відправлена одразу, без чекання");
    CHECK(reliable_is_busy(&ctx), "після старту є активна критична відправка");

    /* ACK приходить швидко, задовго до дедлайну (2000мс). */
    reliable_on_ack_received(&ctx, 10);
    ReliableStatus st = reliable_tick(&ctx, 100);
    CHECK(st == RELIABLE_SUCCESS, "tick() повертає SUCCESS одразу після ACK");
    CHECK(!reliable_is_busy(&ctx), "після SUCCESS слот знову вільний");

    ReliableStatus st2 = reliable_tick(&ctx, 200);
    CHECK(st2 == RELIABLE_IDLE, "SUCCESS -- одноразовий сигнал, вдруге вже IDLE");
    CHECK(sent_count == 1, "жодної повторної відправки не було потрібно");
}

static void test_retry_then_success(void) {
    printf("test_retry_then_success:\n");
    reset_fake_transport();
    ReliableCtx ctx;
    reliable_init(&ctx, fake_send, NULL);

    SensorPacket pkt = make_alarm(20);
    reliable_send_critical(&ctx, &pkt, 0);
    CHECK(sent_count == 1, "1-ша спроба відправлена одразу");

    /* ACK не приходить до дедлайну -- tick рівно на дедлайні має повторити. */
    ReliableStatus st = reliable_tick(&ctx, ACK_TIMEOUT_MS);
    CHECK(st == RELIABLE_WAITING, "після retry статус все ще WAITING (не EXHAUSTED)");
    CHECK(sent_count == 2, "дедлайн вийшов -> 2-га спроба відправлена");
    CHECK(ctx.attempts == 2, "лічильник спроб піднявся до 2");

    /* Тепер ACK нарешті приходить. */
    reliable_on_ack_received(&ctx, 20);
    st = reliable_tick(&ctx, ACK_TIMEOUT_MS + 50);
    CHECK(st == RELIABLE_SUCCESS, "після 2-ї спроби ACK таки підтверджує доставку");
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
    /* ACK ніколи не приходить. Псевдокод: attempts від 0 до MAX_RETRIES
     * включно (це MAX_RETRIES+1 спроб усього), потім RETRY ВИЧЕРПАНО. */
    for (int i = 0; i < MAX_RETRIES; i++) {
        now += ACK_TIMEOUT_MS;
        st = reliable_tick(&ctx, now);
        CHECK(st == RELIABLE_WAITING, "проміжні дедлайни -> ще WAITING, ще є спроби");
    }
    CHECK(sent_count == MAX_RETRIES + 1,
          "усього відправлено рівно MAX_RETRIES+1 копій пакета");

    now += ACK_TIMEOUT_MS;
    st = reliable_tick(&ctx, now);
    CHECK(st == RELIABLE_EXHAUSTED, "після останнього дедлайну -> RETRY ВИЧЕРПАНО");
    CHECK(!reliable_is_busy(&ctx), "після EXHAUSTED слот знову вільний");

    st = reliable_tick(&ctx, now + 10);
    CHECK(st == RELIABLE_IDLE, "EXHAUSTED -- теж одноразовий сигнал, вдруге вже IDLE");
    CHECK(sent_count == MAX_RETRIES + 1,
          "після EXHAUSTED жодної зайвої відправки більше не було");
}

static void test_same_bytes_resent(void) {
    printf("test_same_bytes_resent:\n");
    reset_fake_transport();
    ReliableCtx ctx;
    reliable_init(&ctx, fake_send, NULL);

    SensorPacket pkt = make_alarm(40);
    reliable_send_critical(&ctx, &pkt, 0);
    reliable_tick(&ctx, ACK_TIMEOUT_MS);      /* retry #1 */
    reliable_tick(&ctx, 2 * ACK_TIMEOUT_MS);  /* retry #2 */

    CHECK(sent_count == 3, "3 копії відправлено (1 оригінал + 2 retry)");
    CHECK(sent_lens[0] == sent_lens[1] && sent_lens[1] == sent_lens[2],
          "довжина пакета однакова на всіх спробах");
    CHECK(memcmp(sent_bufs[0], sent_bufs[1], (size_t)sent_lens[0]) == 0,
          "байти 1-ї та 2-ї спроби побайтово ідентичні (sequence НЕ змінився)");
    CHECK(memcmp(sent_bufs[1], sent_bufs[2], (size_t)sent_lens[1]) == 0,
          "байти 2-ї та 3-ї спроби побайтово ідентичні");
}

static void test_busy_blocks_new_send(void) {
    printf("test_busy_blocks_new_send:\n");
    reset_fake_transport();
    ReliableCtx ctx;
    reliable_init(&ctx, fake_send, NULL);

    SensorPacket first = make_alarm(50);
    SensorPacket second = make_alarm(51);
    CHECK(reliable_send_critical(&ctx, &first, 0),
          "перша критична відправка стартує");
    CHECK(!reliable_send_critical(&ctx, &second, 10),
          "друга відправка відхиляється, поки перша ще активна (немає черги)");
    CHECK(sent_count == 1, "друга спроба не спричинила зайвої відправки");
}

static void test_ack_wrong_sequence_ignored(void) {
    printf("test_ack_wrong_sequence_ignored:\n");
    reset_fake_transport();
    ReliableCtx ctx;
    reliable_init(&ctx, fake_send, NULL);

    SensorPacket pkt = make_alarm(60);
    reliable_send_critical(&ctx, &pkt, 0);

    reliable_on_ack_received(&ctx, 9999); /* ACK на зовсім інший sequence */
    ReliableStatus st = reliable_tick(&ctx, 100);
    CHECK(st == RELIABLE_WAITING,
          "ACK з чужим sequence ігнорується, відправка й далі активна");
}

static void test_non_critical_rejected(void) {
    printf("test_non_critical_rejected:\n");
    reset_fake_transport();
    ReliableCtx ctx;
    reliable_init(&ctx, fake_send, NULL);

    SensorPacket pkt = make_alarm(70);
    pkt.msg_type = MSG_TELEMETRY; /* не критичний тип */
    bool started = reliable_send_critical(&ctx, &pkt, 0);
    CHECK(!started, "MSG_TELEMETRY відхиляється -- цей механізм тільки для ALARM/CONFIG");
    CHECK(sent_count == 0, "нічого не відправлено для не-критичного типу");
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
    printf("Тестів: %d, провалено: %d\n", tests_run, tests_failed);
    if (tests_failed == 0) {
        printf("УСІ ТЕСТИ ПРОЙШЛИ\n");
        return 0;
    }
    printf("Є ПРОВАЛЕНІ ТЕСТИ\n");
    return 1;
}
