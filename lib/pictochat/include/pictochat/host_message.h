#pragma once
#include "host_identity.h"

#define HOST_MESSAGE_HEADER 36u
#define HOST_MESSAGE_MAX 10276u
#define HOST_MESSAGE_CHUNK 160u
#define HOST_MESSAGE_COPIES 3u

typedef struct {
    bool active, invalid, final_seen, complete;
    uint16_t total, covered;
    uint8_t announcement[20];
    uint8_t body[HOST_MESSAGE_MAX];
    uint8_t coverage[(HOST_MESSAGE_MAX + 7) / 8];
} host_message_rx_t;

static inline unsigned host_message_u16(const uint8_t *p) {
    return p[0] | ((unsigned)p[1] << 8);
}

static inline void host_message_reset(host_message_rx_t *s) {
    memset(s, 0, sizeof(*s));
}

// Returns 1 exactly once per complete announced bitmap, -1 for invalid data,
// and 0 while incomplete or for unrelated identity/control packets.
static inline int host_message_receive_slot(host_message_rx_t *s, const uint8_t *app,
                                            size_t len, uint8_t client_slot) {
    if (len < 4 || host_message_u16(app + 2) != len) return -1;
    unsigned kind = host_message_u16(app);
    if (kind == 0 && len == 20) {
        unsigned total = host_message_u16(app + 8);
        if (app[4] != client_slot || total <= HOST_MESSAGE_HEADER || total > HOST_MESSAGE_MAX ||
            (total - HOST_MESSAGE_HEADER) % 1024 || app[10] || app[11]) {
            host_message_reset(s);
            return 0;
        }
        if (s->active && memcmp(s->announcement, app, 20) == 0) return 0;
        host_message_reset(s);
        s->active = true;
        s->total = (uint16_t)total;
        memcpy(s->announcement, app, 20);
        return 0;
    }
    if (kind != 2 || !s->active || s->invalid || s->complete) return 0;
    if (len < 13 || app[4] != client_slot) return -1;
    unsigned count = app[6], offset = host_message_u16(app + 8);
    bool final = (app[7] & 1) != 0;
    if (!count || len != 12 + count || offset + count > s->total ||
        (final && offset + count != s->total)) {
        s->invalid = true;
        return -1;
    }
    for (unsigned i = 0; i < count; ++i) {
        unsigned at = offset + i, bit = 1u << (at & 7);
        if (s->coverage[at >> 3] & bit) {
            if (s->body[at] != app[12 + i]) {
                s->invalid = true;
                return -1;
            }
        } else {
            s->body[at] = app[12 + i];
            s->coverage[at >> 3] |= (uint8_t)bit;
            ++s->covered;
        }
    }
    s->final_seen |= final;
    if (s->covered == s->total && s->final_seen) {
        if (s->body[0] != 3 || s->body[1] != 2) {
            s->invalid = true;
            return -1;
        }
        s->complete = true;
        return 1;
    }
    return 0;
}

static inline int host_message_receive(host_message_rx_t *s, const uint8_t *app, size_t len) {
    return host_message_receive_slot(s, app, len, 1);
}

typedef struct {
    bool active, announced;
    uint16_t offset;
    uint8_t copies;
} host_message_cursor_t;
typedef struct {
    host_message_cursor_t cursor;
    uint16_t total;
    uint8_t announcement[20];
    uint8_t body[HOST_MESSAGE_MAX];
} host_message_tx_t;

static inline void host_message_reply(host_message_tx_t *s, const uint8_t announcement[20],
                                       const uint8_t *body, uint16_t total,
                                       const uint8_t host_mac[6], uint32_t token) {
    s->total = total;
    memcpy(s->announcement, announcement, 20);
    s->announcement[0] = 1;
    s->announcement[4] = 0;
    for (unsigned i = 0; i < 4; ++i) s->announcement[16 + i] = (uint8_t)(token >> (8 * i));
    memcpy(s->body, body, total);
    // Message sender MAC uses the same halfword swapping as ConsoleId.
    for (unsigned i = 0; i < 6; ++i) s->body[2 + i] = host_mac[i ^ 1];
    s->cursor = (host_message_cursor_t){.active = true};
}

static inline bool host_message_next(host_message_tx_t *s, host_id_packet_t *out) {
    if (!s->cursor.active) return false;
    if (!s->cursor.announced) {
        *out = (host_id_packet_t){.len = 20};
        memcpy(out->bytes, s->announcement, 20);
        if (++s->cursor.copies == HOST_MESSAGE_COPIES) {
            s->cursor.announced = true;
            s->cursor.copies = 0;
        }
        return true;
    }
    unsigned remaining = s->total - s->cursor.offset;
    unsigned count = remaining < HOST_MESSAGE_CHUNK ? remaining : HOST_MESSAGE_CHUNK;
    *out = (host_id_packet_t){.len = (uint16_t)(12 + count)};
    uint8_t *a = out->bytes;
    a[0] = 2; a[2] = (uint8_t)out->len; a[3] = (uint8_t)(out->len >> 8);
    a[4] = s->announcement[4];
    a[6] = (uint8_t)count; a[7] = count == remaining;
    a[8] = (uint8_t)s->cursor.offset; a[9] = (uint8_t)(s->cursor.offset >> 8);
    memcpy(a + 12, s->body + s->cursor.offset, count);
    // Native host captures repeat announcements and chunks. Each submission
    // gets a fresh WM sequence from the caller; the application bytes repeat.
    if (++s->cursor.copies == HOST_MESSAGE_COPIES) {
        s->cursor.copies = 0;
        s->cursor.offset += (uint16_t)count;
        if (count == remaining) s->cursor.active = false;
    }
    return true;
}

static inline uint32_t host_message_hash(const uint8_t *p, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) hash = (hash ^ p[i]) * 16777619u;
    return hash;
}
