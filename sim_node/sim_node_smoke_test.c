/*
 * Локальний "smoke test" для Блоку D -- НЕ потребує MQTT чи Gateway.
 * Доводить: pkt генерується правильно, protocol_pack працює, і
 * пошкоджений пакет ловиться -- рівно те, що потрібно ПЕРЕД тим, як
 * підключати живий MQTT.
 *
 * Компіляція:
 *   cc -Wall -Wextra sim_node_smoke_test.c ../protocol/protocol.c -I../protocol -o smoke_test
 * Запуск:
 *   ./smoke_test
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "protocol.h"

/* На ПК (не ESP32) немає millis() -- ось звідки береться час.
 * Це wall-clock (мс з 1970 року), саме те, що документація протоколу
 * очікує від timestamp_ms. */
static uint64_t get_current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

int main(void) {
    uint32_t seq_counter = 0;

    /* ==== 1. Генерація нормального пакета (як у інструкції) ==== */
    SensorPacket pkt = {0};
    pkt.version = PROTOCOL_VERSION;
    pkt.msg_type = MSG_TELEMETRY;
    pkt.node_id = 3;  /* симульований вузол */
    pkt.sequence = seq_counter++;
    pkt.timestamp_ms = get_current_time_ms();

    const char *json = "{\"t\":23.1,\"h\":50.0}";
    pkt.payload_len = (uint16_t)strlen(json);
    memcpy(pkt.payload, json, pkt.payload_len);

    uint8_t tx_buf[256];
    int packed_len = protocol_pack(&pkt, tx_buf, sizeof(tx_buf));
    printf("1) Згенеровано пакет: node_id=%d seq=%u, packed_len=%d байт\n",
           pkt.node_id, pkt.sequence, packed_len);

    /* ==== 2. "Відправка" -- поки без MQTT просто симулюємо приймач ====
     * Це те, що зробить Gateway (protocol_unpack на прийняті байти). */
    SensorPacket received = {0};
    int rc = protocol_unpack(tx_buf, (size_t)packed_len, &received);
    printf("2) Приймач розпакував: rc=%d (0=OK), node_id=%d seq=%u payload=\"%.*s\"\n",
           rc, received.node_id, received.sequence,
           received.payload_len, received.payload);

    /* ==== 3. Тест №5: псуємо байт, перевіряємо, що приймач це ловить ==== */
    uint8_t corrupted[256];
    memcpy(corrupted, tx_buf, (size_t)packed_len);
    corrupted[5] ^= 0xFF;  /* той самий байт, що і в інструкції -- усередині sequence */

    SensorPacket after_corruption = {0};
    int rc_corrupt = protocol_unpack(corrupted, (size_t)packed_len, &after_corruption);
    printf("3) Тест №5 (псування байта 5): rc=%d "
           "(очікуємо %d = PROTO_ERR_BAD_CRC) -> %s\n",
           rc_corrupt, PROTO_ERR_BAD_CRC,
           rc_corrupt == PROTO_ERR_BAD_CRC ? "ПРОЙШОВ" : "ПРОВАЛЕНО");

    /* ==== 4. Тест №3 (дублікат) -- чесна демонстрація МЕЖІ цього тесту ====
     * protocol_unpack сам по собі НЕ знає про дублікати -- він розпакує
     * той самий пакет успішно скільки завгодно раз. Дедуплікація -- це
     * робота Gateway (NodeState.record_packet), а не кодека. */
    SensorPacket second_receive = {0};
    int rc_dup = protocol_unpack(tx_buf, (size_t)packed_len, &second_receive);
    printf("4) Тест №3 (дублікат, той самий tx_buf ще раз): rc=%d "
           "-- це ОК, protocol_unpack не повинен це відхиляти;\n"
           "   справжня перевірка дублікату -- на стороні Gateway (Блок C), "
           "коли він буде готовий.\n", rc_dup);

    return 0;
}
