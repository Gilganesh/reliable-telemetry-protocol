#ifndef THRESHOLD_H
#define THRESHOLD_H

#include <stdbool.h>

typedef struct {
    bool active;
} ThresholdAlarm;

static inline void threshold_init(ThresholdAlarm *t) {
    t->active = false;
}

static inline bool threshold_update(ThresholdAlarm *t, float value, float limit, float hysteresis) {
    if (!t->active && value >= limit) {
        t->active = true;
        return true;
    }
    if (t->active && value < limit - hysteresis) t->active = false;
    return false;
}

#endif
