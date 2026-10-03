#include <stdio.h>
#include "threshold.h"

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

#define LIMIT 35.0f
#define HYST 2.0f

static void test_below_limit_is_silent(void) {
    printf("test_below_limit_is_silent:\n");
    ThresholdAlarm t;
    threshold_init(&t);
    CHECK(!threshold_update(&t, 22.0f, LIMIT, HYST), "normal temperature raises nothing");
    CHECK(!threshold_update(&t, 34.9f, LIMIT, HYST), "just below the limit raises nothing");
    CHECK(!t.active, "the alarm stays inactive");
}

static void test_fires_once_per_crossing(void) {
    printf("test_fires_once_per_crossing:\n");
    ThresholdAlarm t;
    threshold_init(&t);
    CHECK(threshold_update(&t, 35.0f, LIMIT, HYST), "reaching the limit exactly fires");
    CHECK(t.active, "the alarm becomes active");
    CHECK(!threshold_update(&t, 36.5f, LIMIT, HYST), "staying above does not fire again");
    CHECK(!threshold_update(&t, 41.0f, LIMIT, HYST), "getting hotter does not fire again");
}

static void test_hysteresis_prevents_flapping(void) {
    printf("test_hysteresis_prevents_flapping:\n");
    ThresholdAlarm t;
    threshold_init(&t);
    threshold_update(&t, 36.0f, LIMIT, HYST);
    CHECK(!threshold_update(&t, 34.5f, LIMIT, HYST), "a dip inside the hysteresis band does not re-arm");
    CHECK(t.active, "the alarm stays active inside the band");
    CHECK(!threshold_update(&t, 35.4f, LIMIT, HYST), "coming back above does not fire a second time");
}

static void test_rearms_after_cooling(void) {
    printf("test_rearms_after_cooling:\n");
    ThresholdAlarm t;
    threshold_init(&t);
    threshold_update(&t, 36.0f, LIMIT, HYST);
    CHECK(!threshold_update(&t, 32.9f, LIMIT, HYST), "cooling below limit minus hysteresis is silent");
    CHECK(!t.active, "the alarm is re-armed");
    CHECK(threshold_update(&t, 35.2f, LIMIT, HYST), "the next crossing fires again");
}

int main(void) {
    test_below_limit_is_silent();
    test_fires_once_per_crossing();
    test_hysteresis_prevents_flapping();
    test_rearms_after_cooling();

    printf("\n----------------------------------------\n");
    printf("Tests: %d, failed: %d\n", tests_run, tests_failed);
    if (tests_failed == 0) printf("ALL TESTS PASSED\n");
    return tests_failed == 0 ? 0 : 1;
}
