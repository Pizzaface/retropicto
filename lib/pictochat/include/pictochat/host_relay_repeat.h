#pragma once
#include "host_identity.h"

// Experimental retry pacing, not permanent deduplication: a client may have
// missed the application relay even when its polled radio reply arrived.
#define HOST_RELAY_REPEAT_US 100000

typedef struct {
    bool valid;
    uint32_t generation;
    uint16_t sequence;
    int64_t delivered_us;
    host_id_packet_t packet; // successfully delivered relay (announcement type 1)
} host_relay_repeat_t;

static inline bool host_relay_repeat_skip(const host_relay_repeat_t *s,
    uint32_t generation, uint16_t sequence, const uint8_t *app, size_t len,
    int64_t now_us) {
    return s->valid && s->generation == generation && s->sequence == sequence &&
        now_us >= s->delivered_us && now_us - s->delivered_us < HOST_RELAY_REPEAT_US &&
        app && len > 0 && len <= sizeof(s->packet.bytes) && s->packet.len == len &&
        s->packet.bytes[0] == (app[0] == 0 ? 1 : app[0]) &&
        !memcmp(s->packet.bytes + 1, app + 1, len - 1);
}
