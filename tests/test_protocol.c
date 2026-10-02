#include <stdio.h>
#include <string.h>
#include "protocol.h"

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

static void test_roundtrip_telemetry(void) {
    printf("test_roundtrip_telemetry:\n");
    SensorPacket pkt = {0};
    pkt.version = PROTOCOL_VERSION;
    pkt.msg_type = MSG_TELEMETRY;
    pkt.node_id = 5;
    pkt.sequence = 42;
    pkt.timestamp_ms = 1234567890ULL;
    const char *json = "{\"t\":21.4,\"h\":45.2}";
    pkt.payload_len = (uint16_t)strlen(json);
    memcpy(pkt.payload, json, pkt.payload_len);

    uint8_t buf[256];
    int packed_len = protocol_pack(&pkt, buf, sizeof(buf));
    CHECK(packed_len > 0, "protocol_pack returns a positive length");

    SensorPacket out = {0};
    int rc = protocol_unpack(buf, (size_t)packed_len, &out);
    CHECK(rc == PROTO_OK, "protocol_unpack returns PROTO_OK");
    CHECK(out.node_id == 5, "node_id survives round-trip");
    CHECK(out.sequence == 42, "sequence survives round-trip");
    CHECK(out.timestamp_ms == 1234567890ULL, "timestamp_ms survives round-trip");
    CHECK(out.msg_type == MSG_TELEMETRY, "msg_type survives round-trip");
    CHECK(out.payload_len == pkt.payload_len, "payload_len survives round-trip");
    CHECK(memcmp(out.payload, pkt.payload, pkt.payload_len) == 0,
          "payload is byte-identical");
}

static void test_roundtrip_empty_payload_heartbeat(void) {
    printf("test_roundtrip_empty_payload_heartbeat:\n");
    SensorPacket pkt = {0};
    pkt.version = PROTOCOL_VERSION;
    pkt.msg_type = MSG_HEARTBEAT;
    pkt.node_id = 1;
    pkt.sequence = 0;
    pkt.payload_len = 0;

    uint8_t buf[256];
    int packed_len = protocol_pack(&pkt, buf, sizeof(buf));
    CHECK(packed_len == HEADER_SIZE + CRC_SIZE,
          "packet length is header + CRC when payload is empty");

    SensorPacket out = {0};
    int rc = protocol_unpack(buf, (size_t)packed_len, &out);
    CHECK(rc == PROTO_OK, "protocol_unpack returns PROTO_OK");
    CHECK(out.msg_type == MSG_HEARTBEAT, "msg_type == MSG_HEARTBEAT");
    CHECK(out.payload_len == 0, "payload_len == 0");
}

static void test_corrupted_payload_byte_detected(void) {
    printf("test_corrupted_payload_byte_detected:\n");
    SensorPacket pkt = {0};
    pkt.msg_type = MSG_CONFIG;
    pkt.node_id = 2;
    pkt.sequence = 9;
    const char *data = "config-data";
    pkt.payload_len = (uint16_t)strlen(data);
    memcpy(pkt.payload, data, pkt.payload_len);

    uint8_t buf[256];
    int packed_len = protocol_pack(&pkt, buf, sizeof(buf));
    buf[HEADER_SIZE + 2] ^= 0xFF;

    SensorPacket out = {0};
    int rc = protocol_unpack(buf, (size_t)packed_len, &out);
    CHECK(rc == PROTO_ERR_BAD_CRC,
          "corrupted payload byte -> PROTO_ERR_BAD_CRC");
}

static void test_corrupted_crc_byte_detected(void) {
    printf("test_corrupted_crc_byte_detected:\n");
    SensorPacket pkt = {0};
    pkt.msg_type = MSG_ALARM;
    pkt.node_id = 3;
    pkt.sequence = 1;
    pkt.payload_len = 0;

    uint8_t buf[256];
    int packed_len = protocol_pack(&pkt, buf, sizeof(buf));
    buf[packed_len - 1] ^= 0xFF;

    SensorPacket out = {0};
    int rc = protocol_unpack(buf, (size_t)packed_len, &out);
    CHECK(rc == PROTO_ERR_BAD_CRC,
          "corrupted CRC byte -> PROTO_ERR_BAD_CRC");
}

static void test_truncated_packet_rejected(void) {
    printf("test_truncated_packet_rejected:\n");
    SensorPacket pkt = {0};
    pkt.msg_type = MSG_TELEMETRY;
    pkt.node_id = 1;
    pkt.sequence = 1;
    pkt.payload_len = 20;
    memset(pkt.payload, 'x', 20);

    uint8_t buf[256];
    protocol_pack(&pkt, buf, sizeof(buf));

    SensorPacket out = {0};
    int rc = protocol_unpack(buf, 10, &out);
    CHECK(rc == PROTO_ERR_TOO_SHORT, "truncated packet -> PROTO_ERR_TOO_SHORT");
}

static void test_too_short_rejected(void) {
    printf("test_too_short_rejected:\n");
    uint8_t tiny[2] = {0x00, 0x01};
    SensorPacket out = {0};
    int rc = protocol_unpack(tiny, sizeof(tiny), &out);
    CHECK(rc == PROTO_ERR_TOO_SHORT, "2-byte input -> PROTO_ERR_TOO_SHORT");
}

static void test_random_garbage_rejected(void) {
    printf("test_random_garbage_rejected:\n");
    uint8_t garbage[30];
    for (int i = 0; i < 30; i++) {
        garbage[i] = (uint8_t)i;
    }
    SensorPacket out = {0};
    int rc = protocol_unpack(garbage, sizeof(garbage), &out);
    CHECK(rc != PROTO_OK,
          "random garbage -> an error code, never PROTO_OK or a crash");
}

static void test_payload_too_large_rejected(void) {
    printf("test_payload_too_large_rejected:\n");
    SensorPacket pkt = {0};
    pkt.msg_type = MSG_TELEMETRY;
    pkt.payload_len = MAX_PAYLOAD_SIZE + 1;

    uint8_t buf[512];
    int rc = protocol_pack(&pkt, buf, sizeof(buf));
    CHECK(rc == PROTO_ERR_PAYLOAD_TOO_BIG,
          "payload_len > MAX_PAYLOAD_SIZE -> PROTO_ERR_PAYLOAD_TOO_BIG");
}

static void test_unknown_msg_type_rejected(void) {
    printf("test_unknown_msg_type_rejected:\n");
    SensorPacket pkt = {0};
    pkt.msg_type = 99;
    pkt.payload_len = 0;

    uint8_t buf[256];
    int packed_len = protocol_pack(&pkt, buf, sizeof(buf));

    SensorPacket out = {0};
    int rc = protocol_unpack(buf, (size_t)packed_len, &out);
    CHECK(rc == PROTO_ERR_UNKNOWN_TYPE, "msg_type=99 -> PROTO_ERR_UNKNOWN_TYPE");
}

int main(void) {
    test_roundtrip_telemetry();
    test_roundtrip_empty_payload_heartbeat();
    test_corrupted_payload_byte_detected();
    test_corrupted_crc_byte_detected();
    test_truncated_packet_rejected();
    test_too_short_rejected();
    test_random_garbage_rejected();
    test_payload_too_large_rejected();
    test_unknown_msg_type_rejected();

    printf("\n----------------------------------------\n");
    printf("Tests: %d, failed: %d\n", tests_run, tests_failed);
    if (tests_failed == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("SOME TESTS FAILED\n");
    return 1;
}
