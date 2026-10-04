#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Single-client identity exchange, owned exclusively by the host task.
// Application payloads here exclude WM headers and sequence footers.
enum {
    HOST_ID_ANNOUNCE0,
    HOST_ID_DATA0,
    HOST_ID_WAIT0,
    HOST_ID_ANNOUNCE1,
    HOST_ID_DATA1,
    HOST_ID_WAIT1,
    HOST_ID_READY,
    HOST_ID_REQUEST
};

typedef struct {
    uint16_t len;
    uint8_t bytes[268]; // 12-byte transfer header plus up to 255 payload bytes
} host_id_packet_t;

typedef struct {
    unsigned phase;
    bool pending;
    bool announced;
    bool relayed[2];
    uint8_t client_stage;
    uint8_t client_slot; // roster/AID slot; zero belongs to the host
    uint16_t transfer_size;
    bool relay_identity;
    uint32_t token[2];
    host_id_packet_t relay;
} host_identity_t;

static inline void host_identity_reset(host_identity_t *s, uint32_t token0, uint32_t token1) {
    *s = (host_identity_t){.client_slot = 1, .token = {token0, token1}};
}

// Relay identity and drawing transfers, preserving the client's application
// bytes. Complete drawing validation/reassembly is handled separately.
static inline bool host_identity_receive(host_identity_t *s, const uint8_t *app, size_t len) {
    if (s->pending || len < 4 || len > sizeof(s->relay.bytes) ||
        (app[2] | ((unsigned)app[3] << 8)) != len)
        return false;
    if (len == 20 && app[0] == 0 && app[1] == 0 && app[4] == s->client_slot && app[10] == 0 &&
        app[11] == 0) {
        unsigned total = app[8] | ((unsigned)app[9] << 8);
        if (total != 84 && !(total > 36 && total <= 10276 && (total - 36) % 1024 == 0))
            return false;
        memcpy(s->relay.bytes, app, len);
        s->relay.bytes[0] = 1;
        s->relay.len = (uint16_t)len;
        s->pending = true;
        s->announced = true;
        s->transfer_size = (uint16_t)total;
        return true;
    }
    if (s->announced && len >= 13 && app[0] == 2 && app[1] == 0 && app[4] == s->client_slot) {
        unsigned end = (app[8] | ((unsigned)app[9] << 8)) + app[6];
        if (!app[6] || len != 12u + app[6] || end > s->transfer_size ||
            ((app[7] & 1) && end != s->transfer_size))
            return false;
        s->relay_identity = s->transfer_size == 84;
        if (s->relay_identity && !(len == 96 && app[7] == 1 && app[8] == 0 && app[9] == 0 &&
                                   app[12] == 3 && app[13] <= 1))
            return false;
        memcpy(s->relay.bytes, app, len);
        s->relay.len = (uint16_t)len;
        if (s->relay_identity)
            s->client_stage = app[13];
        s->pending = true;
        return true;
    }
    return false;
}

// profile is the captured 84-byte identity body with our MAC/name substituted.
// Caller commits this state only after successful TX completion; on failure it
// restores a snapshot and retries the same payload and footer sequence.
static inline bool host_identity_next(host_identity_t *s, const uint8_t profile[84],
                                      host_id_packet_t *out) {
    if (s->pending) {
        *out = s->relay;
        s->pending = false;
        if (out->bytes[0] == 2) {
            // Retain the announced size for duplicate final-fragment retries;
            // a new announcement replaces it. The message assembler emits once.
            if (s->relay_identity)
                s->relayed[s->client_stage] = true;
            if (s->relayed[0] && s->relayed[1] && s->phase == HOST_ID_WAIT1)
                s->phase = HOST_ID_READY;
        }
        return true;
    }
    unsigned stage;
    if (s->phase == HOST_ID_ANNOUNCE0 || s->phase == HOST_ID_ANNOUNCE1) {
        stage = s->phase == HOST_ID_ANNOUNCE1;
        *out = (host_id_packet_t){.len = 20};
        uint8_t *a = out->bytes;
        a[0] = 1;
        a[2] = 20;
        a[5] = stage ? 0 : 0xd0;
        a[6] = 0xff;
        a[7] = 0xff;
        a[8] = 84;
        a[12] = stage ? 0x0c : 0;
        uint32_t token = s->token[stage];
        for (unsigned i = 0; i < 4; ++i)
            a[16 + i] = (uint8_t)(token >> (8 * i));
        s->phase = stage ? HOST_ID_DATA1 : HOST_ID_DATA0;
        return true;
    }
    if (s->phase == HOST_ID_DATA0 || s->phase == HOST_ID_DATA1) {
        stage = s->phase == HOST_ID_DATA1;
        *out = (host_id_packet_t){.len = 96};
        uint8_t *a = out->bytes;
        a[0] = 2;
        a[2] = 96;
        a[5] = stage ? 0xdb : 0x2d;
        a[6] = 84;
        a[7] = 1;
        memcpy(a + 12, profile, 84);
        a[13] = (uint8_t)stage;
        // Do not wait for a client transfer between our own two identity stages.
        // The first radio trial stalled there with only WM acknowledgments.
        s->phase = stage ? (s->relayed[0] && s->relayed[1] ? HOST_ID_READY : HOST_ID_REQUEST)
                         : HOST_ID_ANNOUNCE1;
        return true;
    }
    if (s->phase == HOST_ID_REQUEST) {
        // RequestIdent(1) from the local Rust reference, including its opaque
        // default magic. Hardware must establish whether this solicits identity.
        static const uint8_t request[20] = {1, 0, 20, 0, 1, 0, 0xff, 0xff, 84,   0,
                                            0, 0, 0,  0, 0, 0, 0xb7, 0x78, 0xd5, 0x29};
        *out = (host_id_packet_t){.len = sizeof(request)};
        memcpy(out->bytes, request, sizeof(request));
        out->bytes[4] = s->client_slot;
        s->phase = HOST_ID_WAIT1;
        return true;
    }
    return false; // poll with heartbeats while waiting, or after both stages
}
