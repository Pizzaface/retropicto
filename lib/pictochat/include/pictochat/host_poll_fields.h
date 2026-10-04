#pragma once
#include <stdbool.h>
#include <stdint.h>

// Owned by the host task. Initial empty CMD does not consume this schedule.
// Preserve one-in-three grants, but grant the first typed poll after each join.
// WM toggles on every typed poll, independent of whether that poll is granted.
// This is an isolated policy experiment, not an exact replay of a DS trace.
typedef struct {
    uint8_t phase;
    bool wm_high;
} host_poll_fields_t;

typedef struct {
    uint16_t bitmask;
    uint16_t wm;
} host_poll_fields_result_t;

static inline void host_poll_fields_reset(host_poll_fields_t *s) {
    *s = (host_poll_fields_t){0};
}

static inline host_poll_fields_result_t host_poll_fields_next(host_poll_fields_t *s,
                                                              bool have_client) {
    host_poll_fields_result_t fields = {
        .bitmask = have_client && s->phase == 0 ? 2 : 0, // current single-client AID 1
        .wm = s->wm_high ? 0x9c34 : 0x1c34};
    s->phase = (s->phase + 1) % 3;
    s->wm_high = !s->wm_high;
    return fields;
}

// Post-admission reference CMDs address the same client set in their footer and
// reply-slot mask. The initial pre-admission heartbeat has a zero footer mask.
static inline void host_poll_footer(uint8_t footer[4], uint16_t sequence, uint16_t grant_mask,
                                    bool admitted) {
    uint16_t target = admitted ? grant_mask : 0;
    footer[0] = (uint8_t)sequence;
    footer[1] = (uint8_t)(sequence >> 8);
    footer[2] = (uint8_t)target;
    footer[3] = (uint8_t)(target >> 8);
}
