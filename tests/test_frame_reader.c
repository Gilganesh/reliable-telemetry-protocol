#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "frame_reader.h"

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

#define MAX_SEQ 16

static int make_frame(uint8_t *out, uint32_t seq) {
    SensorPacket p = {0};
    p.version = PROTOCOL_VERSION;
    p.msg_type = MSG_TELEMETRY;
    p.node_id = 1;
    p.sequence = seq;
    p.timestamp_ms = seq * 1000ULL;
    const char *json = "{\"temperature\":21.30,\"humidity\":49.80}";
    p.payload_len = (uint16_t)strlen(json);
    memcpy(p.payload, json, p.payload_len);
    return protocol_pack(&p, out, 256);
}

typedef struct {
    uint32_t seqs[64];
    int count;
    uint32_t bad;
} Result;

static Result feed_stream(const uint8_t *stream, size_t n) {
    Result r = {{0}, 0, 0};
    FrameReader fr;
    memset(&fr, 0, sizeof(fr));
    frame_reader_init(&fr);
    for (size_t i = 0; i < n; i++) {
        if (!frame_reader_feed_byte(&fr, stream[i])) continue;
        do {
            SensorPacket out;
            if (protocol_unpack(fr.buf, fr.need, &out) == PROTO_OK && r.count < 64) r.seqs[r.count++] = out.sequence;
        } while (frame_reader_next(&fr));
    }
    r.bad = fr.bad_frames;
    return r;
}

static bool has_seq(const Result *r, uint32_t seq) {
    for (int i = 0; i < r->count; i++)
        if (r->seqs[i] == seq) return true;
    return false;
}

static size_t build(uint8_t *s, uint32_t first, uint32_t last, uint32_t damage_seq, int mode) {
    size_t n = 0;
    for (uint32_t q = first; q <= last; q++) {
        int k = make_frame(s + n, q);
        if (q == damage_seq) {
            if (mode == 1) s[n + 16] = 120;
            if (mode == 2) s[n + 25] ^= 0x10;
            if (mode == 3) {
                n += (size_t)k - 7;
                continue;
            }
        }
        n += (size_t)k;
    }
    return n;
}

static void test_clean_stream(void) {
    printf("test_clean_stream:\n");
    uint8_t s[4096];
    size_t n = build(s, 1, 6, 0, 0);
    Result r = feed_stream(s, n);
    CHECK(r.count == 6 && r.bad == 0, "six intact frames are all delivered, nothing counted as bad");
}

static void test_corrupted_length_byte(void) {
    printf("test_corrupted_length_byte:\n");
    uint8_t s[4096];
    size_t n = build(s, 1, 6, 2, 1);
    Result r = feed_stream(s, n);
    CHECK(!has_seq(&r, 2), "the frame with the damaged length is rejected");
    CHECK(has_seq(&r, 3) && has_seq(&r, 4), "the intact frames after it are not swallowed");
    CHECK(r.count == 5 && r.bad >= 1, "exactly one frame is lost and the damage is counted");
}

static void test_payload_bit_flip(void) {
    printf("test_payload_bit_flip:\n");
    uint8_t s[4096];
    size_t n = build(s, 1, 6, 3, 2);
    Result r = feed_stream(s, n);
    CHECK(r.count == 5 && !has_seq(&r, 3) && r.bad >= 1, "only the flipped frame is lost and counted");
}

static void test_garbage_before_stream(void) {
    printf("test_garbage_before_stream:\n");
    uint8_t s[4096];
    size_t n = 0;
    s[n++] = 0x01;
    s[n++] = 0xAA;
    s[n++] = 0x01;
    n += build(s + n, 1, 4, 0, 0);
    Result r = feed_stream(s, n);
    CHECK(r.count == 4, "a stream that starts with junk still yields every frame");
}

static void test_truncated_frame(void) {
    printf("test_truncated_frame:\n");
    uint8_t s[4096];
    size_t n = build(s, 1, 4, 2, 3);
    Result r = feed_stream(s, n);
    CHECK(has_seq(&r, 1) && has_seq(&r, 3) && has_seq(&r, 4) && !has_seq(&r, 2),
          "a truncated frame costs only itself, the next frames are recovered");
}

static void test_random_bit_flips(void) {
    printf("test_random_bit_flips:\n");
    srand(1);
    int delivered = 0, expected_min = 0, order_ok = 1, streams = 2000;
    for (int it = 0; it < streams; it++) {
        uint8_t s[4096];
        size_t n = build(s, 1, 10, 0, 0);
        s[(size_t)rand() % n] ^= (uint8_t)(1u << (rand() % 8));
        Result r = feed_stream(s, n);
        delivered += r.count;
        expected_min += 9;
        for (int i = 1; i < r.count; i++)
            if (r.seqs[i] <= r.seqs[i - 1]) order_ok = 0;
    }
    CHECK(delivered >= expected_min, "one flipped bit per stream loses at most one frame (no collateral loss)");
    CHECK(order_ok, "delivered frames keep their order and are never repeated");
}

int main(void) {
    test_clean_stream();
    test_corrupted_length_byte();
    test_payload_bit_flip();
    test_garbage_before_stream();
    test_truncated_frame();
    test_random_bit_flips();

    printf("\n----------------------------------------\n");
    printf("Tests: %d, failed: %d\n", tests_run, tests_failed);
    if (tests_failed == 0) printf("ALL TESTS PASSED\n");
    return tests_failed == 0 ? 0 : 1;
}
