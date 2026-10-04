#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "mp_reply.h"

// Default: first 64 MP frames after association. Fast delivery witnesses may
// extend this window to retain ACKs and empty replies during drawing transfers.
// All lengths include the driver's four-byte capture trailer.
#ifndef MP_TRACE_FRAMES
#define MP_TRACE_FRAMES 64u
#endif
#ifndef MP_TRACE_APP_FRAMES
#define MP_TRACE_APP_FRAMES 256u
#endif
typedef struct {
    unsigned remaining;
    unsigned app_remaining;
    bool have_assoc;
    uint16_t assoc_seq;
    uint8_t client[6];
} mp_trace_t;

static inline bool mp_trace_select(mp_trace_t *s, const uint8_t *f, size_t len,
                                   const uint8_t host[6]) {
    if (len < 28)
        return false;
    if (f[0] == 0x10 && len >= 34 && memcmp(f + 10, host, 6) == 0 && f[26] == 0 && f[27] == 0) {
        uint16_t seq = f[22] | ((uint16_t)f[23] << 8);
        if (!s->have_assoc || seq != s->assoc_seq || memcmp(f + 4, s->client, 6)) {
            s->have_assoc = true;
            s->assoc_seq = seq;
            memcpy(s->client, f + 4, 6);
            s->remaining = MP_TRACE_FRAMES;
            s->app_remaining = MP_TRACE_APP_FRAMES;
        }
        return false; // management logger already records this frame
    }
    static const uint8_t cmd[6] = {3, 9, 0xbf, 0, 0, 0};
    static const uint8_t ack[6] = {3, 9, 0xbf, 0, 0, 3};
    bool host_mp = ((f[0] >> 2) & 3) == 2 && memcmp(f + 10, host, 6) == 0 &&
                   (memcmp(f + 4, cmd, 6) == 0 || memcmp(f + 4, ack, 6) == 0);
    bool client_mp = mp_reply_payload_bytes(f, len, host) >= 0 && memcmp(f + 10, s->client, 6) == 0;
    if (!host_mp && !client_mp)
        return false;
    // Keep a separate bounded window for application transfers after admission.
    // Routine heartbeat/ACK traffic must not consume the drawing capture budget.
    bool host_app =
        host_mp && f[0] == 0x28 && len >= 42 && (f[30] == 1 || f[30] == 2) && f[31] == 0;
    bool client_app = client_mp && len >= 36 && (f[26] == 0 || f[26] == 2) && f[27] == 0;
    if (s->have_assoc && s->app_remaining && (host_app || client_app)) {
        --s->app_remaining;
        return true;
    }
    if (!s->remaining)
        return false;
    --s->remaining;
    return true;
}
